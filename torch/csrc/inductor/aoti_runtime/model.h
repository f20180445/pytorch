#pragma once

#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// WARNING: Be careful when adding new includes here. This header will be used
// in model.so, and should not refer to any aten/c10 headers except the stable
// C ABI defined in torch/csrc/inductor/aoti_torch/c/shim.h. The same rule
// applies to other files under torch/csrc/inductor/aoti_runtime/.
#include <torch/csrc/inductor/aoti_runtime/device_utils.h>
#include <torch/csrc/inductor/aoti_torch/c/shim.h>

#define AOTI_RUNTIME_CHECK(EXPR, MSG) \
  do {                                \
    bool ok = EXPR;                   \
    if (!ok) {                        \
      throw std::runtime_error(MSG);  \
    }                                 \
  } while (0)

#if defined(__GNUC__) || defined(__clang__)
#define AOTI_NOINLINE __attribute__((noinline))
#elif _MSC_VER
#define AOTI_NOINLINE __declspec(noinline)
#else
#define AOTI_NOINLINE
#endif

// At codegen time, we write out a binary file called constants.bin.
// We then turn the raw binary to an object file that exposes this
// symbol and link it into the final .so.
// For information on the binary format, see `man objcopy`, under
// the "binary-architecture" flag:
// https://man7.org/linux/man-pages/man1/objcopy.1.html
// todo: use #embed in C++ 23 once available
extern const uint8_t _binary_constants_bin_start[];
extern const uint8_t _binary_constants_bin_end[];

#define AOTI_CONST_GPU_ALIGNMENT 64

namespace {

#ifdef USE_CUDA

using CUDAPtr = std::unique_ptr<void, std::function<void(void*)>>;

CUDAPtr RAII_cudaMalloc(size_t num_bytes) {
  void* data_ptr;
  AOTI_RUNTIME_DEVICE_CHECK(cudaMalloc((void**)&data_ptr, num_bytes));
  auto deleter = [](void* ptr) { AOTI_RUNTIME_DEVICE_CHECK(cudaFree(ptr)); };
  return CUDAPtr(data_ptr, deleter);
}

#endif // USE_CUDA

} // anonymous namespace

AOTI_NOINLINE static void throw_exception(
    const char* call,
    const char* file,
    int64_t line) {
  std::stringstream ss;
  ss << call << " API call failed at " << file << ", line " << line;
  throw std::runtime_error(ss.str());
}

#define AOTI_TORCH_ERROR_CODE_CHECK(call)       \
  if ((call) != AOTI_TORCH_SUCCESS) {           \
    throw_exception(#call, __FILE__, __LINE__); \
  }

using DeleterFnPtr = void (*)(void*);

namespace torch {
namespace aot_inductor {

inline void noop_deleter(void*) {}

inline void delete_tensor_object(void* ptr) {
  AOTI_TORCH_ERROR_CODE_CHECK(
      aoti_torch_delete_tensor_object(reinterpret_cast<AtenTensorHandle>(ptr)));
}

// RAIIAtenTensorHandle steals the tensor objects created by the libtorch C ABI
class RAIIAtenTensorHandle {
 public:
  RAIIAtenTensorHandle() : handle_(nullptr, noop_deleter) {}
  RAIIAtenTensorHandle(const RAIIAtenTensorHandle& other) = delete;
  RAIIAtenTensorHandle& operator=(const RAIIAtenTensorHandle& other) = delete;

  // Steal the ownership from another RAIIAtenTensorHandle using std::move
  RAIIAtenTensorHandle(RAIIAtenTensorHandle&& other) = default;
  RAIIAtenTensorHandle& operator=(RAIIAtenTensorHandle&& other) = default;

  // Steal the ownership from raw AtenTensorHandle
  RAIIAtenTensorHandle(AtenTensorHandle handle)
      : handle_(handle, delete_tensor_object) {}

  ~RAIIAtenTensorHandle() {
    handle_.reset();
  }

  // Return a raw AtenTensorHandle to be used by aoti_torch functions
  // Note: this function does NOT transfer the ownership of the handle
  operator AtenTensorHandle() const {
    return handle_.get();
  }

  AtenTensorHandle release() {
    return handle_.release();
  }

  AtenTensorHandle get() const {
    return handle_.get();
  }

  void reset() {
    handle_.reset();
  }

  int64_t size(int64_t d) {
    int64_t size;
    AOTI_TORCH_ERROR_CODE_CHECK(aoti_torch_get_size(handle_.get(), d, &size));
    return size;
  }

  int64_t stride(int64_t d) {
    int64_t stride;
    AOTI_TORCH_ERROR_CODE_CHECK(
        aoti_torch_get_stride(handle_.get(), d, &stride));
    return stride;
  }

  int64_t storage_offset() {
    int64_t storage_offset;
    AOTI_TORCH_ERROR_CODE_CHECK(
        aoti_torch_get_storage_offset(handle_.get(), &storage_offset));
    return storage_offset;
  }

 private:
  std::unique_ptr<AtenTensorOpaque, DeleterFnPtr> handle_;
};

using ConstantMap = std::unordered_map<std::string, RAIIAtenTensorHandle>;

class ConstantHandle {
 public:
  ConstantHandle() = default;

  explicit ConstantHandle(AtenTensorHandle handle) : handle_(handle) {
    AOTI_TORCH_ERROR_CODE_CHECK(aoti_torch_get_data_ptr(handle_, &data_));
  }

  operator AtenTensorHandle() const {
    return handle_;
  }

  AtenTensorHandle tensor() const {
    return handle_;
  }

  void* data_ptr() const {
    return data_;
  }

 private:
  AtenTensorHandle handle_;
  void* data_ = nullptr;
};

inline void* get_data_ptr_wrapper(const ConstantHandle& constant) {
  return constant.data_ptr();
}

inline const ConstantHandle& unwrap_raii_handle_if_needed(
    const ConstantHandle& handle) {
  return handle;
}

// Shouldn't be called.
inline AtenTensorHandle wrap_with_raii_handle_if_needed(
    const ConstantHandle& handle) = delete;

// Steal the ownership from raw AtenTensorHandle to RAIIAtenTensorHandle
inline std::vector<RAIIAtenTensorHandle> steal_from_raw_handles_to_raii_handles(
    AtenTensorHandle* handles,
    size_t size) {
  std::vector<RAIIAtenTensorHandle> result;
  result.reserve(size);
  for (size_t i = 0; i < size; i++) {
    result.emplace_back(handles[i]);
    handles[i] = nullptr;
  }
  return result;
}

// valid device strs are: cpu, cuda, cuda:0, cuda:1, ...
// Update the list here if more devices are supported in the future
inline void parse_device_str(
    const std::string& device_str,
    int32_t& device_type,
    int32_t& device_idx) {
  std::regex re("(cpu|cuda)(:([0-9]+))?");
  std::smatch sm;
  bool matched = std::regex_match(device_str, sm, re);
  AOTI_RUNTIME_CHECK(matched, "Invalid device: " + device_str);

  if (sm[1].str() == "cpu") {
    device_type = aoti_torch_device_type_cpu();
  } else if (sm[1].str() == "cuda") {
    device_type = aoti_torch_device_type_cuda();
  } else {
    AOTI_RUNTIME_CHECK(false, "Invalid device: " + device_str);
  }

  if (sm[3].matched) {
    device_idx = stoi(sm[3].str());
  } else {
    device_idx = -1;
  }
}

// Defines the base class for AOTInductorModel, which is generated by the
// AOTInductor cpp codegen. Since we do not need dynamic dispatch, we rely
// on curiously recurring template pattern (CRTP) to save some runtime
// v-table overhead. The generated AOTInductorModel is specialized with
// methods such as run_impl.
template <typename Model>
class AOTInductorModelBase {
 public:
  AOTInductorModelBase(
      size_t num_inputs,
      size_t num_outputs,
      size_t num_constants,
      const std::string& device_str,
      std::optional<std::string> cubin_dir)
      : inputs_info_(num_inputs),
        outputs_info_(num_outputs),
        constants_info_(num_constants),
        cubin_dir_(cubin_dir) {
    parse_device_str(device_str, device_type_, device_idx_);

#ifdef USE_CUDA
    if (device_idx_ == -1) {
      AOTI_RUNTIME_DEVICE_CHECK(cudaGetDevice(&device_idx_));
    }
#endif // USE_CUDA
  }

  ~AOTInductorModelBase() {
#ifdef USE_CUDA
    if (run_finished_) {
      auto code = cudaEventDestroy(*run_finished_);
      if (code != cudaSuccess) {
        std::cerr << "Failed to destroy CUDA event in AOTInductor model: "
                  << cudaGetErrorString(code) << std::endl;
      }
    }
#endif // USE_CUDA
  }

  AOTInductorModelBase(AOTInductorModelBase&&) = delete;
  AOTInductorModelBase& operator=(AOTInductorModelBase&&) = delete;
  AOTInductorModelBase(const AOTInductorModelBase&) = delete;
  AOTInductorModelBase& operator=(const AOTInductorModelBase&) = delete;

  void run(
      AtenTensorHandle*
          input_handles, // array of input AtenTensorHandle; handles
                         // are stolen; the array itself is borrowed
      AtenTensorHandle*
          output_handles, // array for writing output AtenTensorHandle; handles
                          // will be stolen by the caller; the array itself is
                          // borrowed
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor) {
#ifdef USE_CUDA
    if (!run_finished_) {
      cudaEvent_t run_finished;
      AOTI_RUNTIME_DEVICE_CHECK(cudaEventCreate(&run_finished));
      run_finished_.emplace(run_finished);
    }

    auto* model = static_cast<Model*>(this);
    model->run_impl(input_handles, output_handles, stream, proxy_executor);
    AOTI_RUNTIME_DEVICE_CHECK(cudaEventRecord(*run_finished_, stream));
#else // !USE_CUDA
    run_finished_ = false;
    auto* model = static_cast<Model*>(this);
    model->run_impl(input_handles, output_handles, stream, proxy_executor);
    run_finished_ = true;
#endif // USE_CUDA
  }

  void load_constants() {
    size_t num_constants = this->num_constants();
    constants_map_->reserve(num_constants);

    std::vector<size_t> constants_internal_offset(num_constants);
    if (device_type_ != aoti_torch_device_type_cpu()) {
      size_t blob_size = 0;
      compute_cuda_constant_blob(blob_size, constants_internal_offset);
#ifdef USE_CUDA
      constant_blob_ = RAII_cudaMalloc(blob_size);
#endif
    }

    size_t bytes_read = 0;
    for (size_t i = 0; i < num_constants; i++) {
      std::string name = this->constant_name(i);
      size_t data_size = this->constant_data_size(i);
      bool from_folded = this->constant_from_folded(i);
      uint8_t* internal_ptr = (data_size != 0)
          ? constant_ptr(
                constants_internal_offset[i],
                bytes_read,
                data_size,
                from_folded)
          : nullptr;
      bytes_read += data_size;

      // Create at::Tensor from copied memory.
      auto dtype = this->constant_dtype(i);
      auto ndim = this->constant_ndim(i);
      auto size = this->constant_shape(i);
      auto stride = this->constant_stride(i);
      auto offset = this->constant_offset(i);

      AtenTensorHandle tensor_handle;
      AOTI_TORCH_ERROR_CODE_CHECK(aoti_torch_create_tensor_from_blob(
          internal_ptr,
          ndim,
          size,
          stride,
          offset,
          dtype,
          device_type_,
          device_idx_,
          &tensor_handle));
      constants_map_->emplace(std::move(name), tensor_handle);
    }
    if (constants_map_) {
      this->update_constants_array_from_map();
    }
  }

#ifdef USE_CUDA
  CUDAPtr&& release_constant_blob() {
    return std::move(constant_blob_);
  }
#endif

  std::shared_ptr<std::vector<ConstantHandle>> get_constants_array() {
    return constants_;
  }

  uint8_t* constant_ptr(
      size_t constant_offset,
      size_t bytes_read,
      size_t data_size,
      bool skip_copy) {
#ifdef USE_CUDA
    auto* constants_ptr = static_cast<uint8_t*>(constant_blob_.get());
    uint8_t* internal_ptr = constants_ptr + constant_offset;
    // Copy data to GPU memory
    // TODO: Handle shared storage case.
    if (!skip_copy) {
      AOTI_RUNTIME_DEVICE_CHECK(cudaMemcpy(
          internal_ptr,
          _binary_constants_bin_start + bytes_read,
          data_size,
          cudaMemcpyHostToDevice));
    }
    return internal_ptr;
#else // !USE_CUDA
    // get pointer to constant which is packed in model during compile time.
    AOTI_RUNTIME_CHECK(!skip_copy, "pure cpu mode doesn't support skip copy");
    return const_cast<uint8_t*>(_binary_constants_bin_start) + bytes_read;
#endif // USE_CUDA
  }

  void compute_cuda_constant_blob(
      size_t& blob_size,
      std::vector<size_t>& constants_internal_offset) {
#ifdef USE_CUDA
    size_t num_constants = this->num_constants();
    // Compute required blob size with 64-alignment if on GPU.
    blob_size = 0;
    for (size_t i = 0; i < num_constants; i++) {
      size_t data_size = this->constant_data_size(i);
      if (data_size % AOTI_CONST_GPU_ALIGNMENT) {
        data_size = AOTI_CONST_GPU_ALIGNMENT +
            (data_size / AOTI_CONST_GPU_ALIGNMENT) * AOTI_CONST_GPU_ALIGNMENT;
      }
      constants_internal_offset[i] = blob_size;
      blob_size += data_size;
    }
#endif // USE_CUDA
  }

  size_t num_inputs() const {
    return inputs_info_.size();
  }

  size_t num_outputs() const {
    return outputs_info_.size();
  }

  size_t num_constants() const {
    return constants_info_.size();
  }

  const char* input_name(int64_t idx) const {
    return inputs_info_.at(idx).name;
  }

  const char* output_name(int64_t idx) const {
    return outputs_info_.at(idx).name;
  }

  const char* constant_name(int64_t idx) const {
    return constants_info_.at(idx).name;
  }

  size_t constant_ndim(int64_t idx) {
    return constants_info_.at(idx).shape.size();
  }

  const int64_t* constant_shape(int64_t idx) const {
    return constants_info_.at(idx).shape.data();
  }

  const int64_t* constant_stride(int64_t idx) const {
    return constants_info_.at(idx).stride.data();
  }

  int32_t constant_dtype(int64_t idx) const {
    return constants_info_.at(idx).dtype;
  }

  size_t constant_offset(int64_t idx) const {
    return constants_info_.at(idx).offset;
  }

  size_t constant_data_size(int64_t idx) const {
    return constants_info_.at(idx).data_size;
  }

  const char* constant_original_fqn(int64_t idx) const {
    return constants_info_.at(idx).original_fqn;
  }

  bool constant_from_folded(int64_t idx) const {
    return constants_info_.at(idx).from_folded;
  }

  const char* get_in_spec() const {
    return in_spec_.c_str();
  }

  const char* get_out_spec() const {
    return out_spec_.c_str();
  }

  void update_constants_array_from_map() {
    if (!constants_map_) {
      throw std::runtime_error{
          "constants_map_ was not ready when constants_ is trying to be constructed from it!"};
    }
    if (!constants_) {
      constants_ =
          std::make_shared<std::vector<ConstantHandle>>(constants_info_.size());
    } else {
      constants_->resize(constants_info_.size());
    }
    int idx = 0;
    for (const auto& info : constants_info_) {
      const auto it = constants_map_->find(info.name);
      if (it != constants_map_->end()) {
        constants_->at(idx) = ConstantHandle(it->second);
      }
      idx++;
    }
  }

  void update_constants_map(
      std::shared_ptr<ConstantMap> constants_map,
      bool remap_constants_array = true) {
    constants_map_ = std::move(constants_map);
    if (remap_constants_array) {
      update_constants_array_from_map();
    }
  }

  // This function allows us to update the constants_ that is used to look up
  // the corresponding constant tensor during runtime.
  void update_constants_array(
      std::shared_ptr<std::vector<ConstantHandle>> constants_array) {
    constants_ = std::move(constants_array);
  }

  /// Returns true if the model is complete.
  bool is_finished() {
#ifdef USE_CUDA
    if (!run_finished_) {
      throw std::runtime_error{"Model CUDA event was not initialized"};
    }

    auto event_status = cudaEventQuery(*run_finished_);
    if (event_status == cudaSuccess) {
      return true;
    } else if (event_status == cudaErrorNotReady) {
      return false;
    }

    throw std::runtime_error(
        std::string("The model did not finish successfully. Error: ") +
        cudaGetErrorString(cudaGetLastError()));
#else // !USE_CUDA
    return run_finished_;
#endif // USE_CUDA
  }

  /// Synchronizes completion event.
  void wait_for_completion() {
#ifdef USE_CUDA
    if (!run_finished_) {
      throw std::runtime_error{"Model event was not initialized"};
    }

    AOTI_RUNTIME_DEVICE_CHECK(cudaEventSynchronize(*run_finished_));
#endif // USE_CUDA
  }

 protected:
  struct ParamInfo {
    const char* name = nullptr;
  };

  struct ConstInfo {
    const char* name = nullptr;
    std::vector<int64_t> shape;
    std::vector<int64_t> stride;
    int32_t dtype;
    int64_t offset;
    size_t data_size;
    const char* original_fqn = nullptr;
    bool from_folded;
  };

  std::vector<ParamInfo> inputs_info_;
  std::vector<ParamInfo> outputs_info_;
  std::vector<ConstInfo> constants_info_;
  std::string in_spec_;
  std::string out_spec_;

  std::shared_ptr<ConstantMap> constants_map_;
  std::shared_ptr<std::vector<ConstantHandle>> constants_;

#ifdef USE_CUDA
  // Holds the blob storage for constants' at::Tensor for CUDA.
  CUDAPtr constant_blob_;
#endif // USE_CUDA

  // A directory with CUDA binary files, e.g. compiled kernels, etc.
  const std::optional<std::string> cubin_dir_;

  // Record if the model finishes an inference run so that its owning
  // AOTModelContainer can re-use this instance.
#ifdef USE_CUDA
  std::optional<cudaEvent_t> run_finished_;
#else // !USE_CUDA
  bool run_finished_;
#endif

  // Generated model uses this device index to create CUDA guards.
  int32_t device_type_;
  int32_t device_idx_;
};

// Codegen-ed classes can derive from this to keep pointers to loaded kernels.
class AOTInductorModelKernelsBase {
 public:
  virtual ~AOTInductorModelKernelsBase() = default;
};

class AOTInductorModel : public AOTInductorModelBase<AOTInductorModel> {
 public:
  AOTInductorModel(
      std::shared_ptr<ConstantMap> constants_map,
      std::shared_ptr<std::vector<ConstantHandle>> constants_array,
      const std::string& device_str,
      std::optional<std::string> cubin_dir);

  std::unordered_map<std::string, AtenTensorHandle> const_run_impl(
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor,
      bool initialization = false);

  void _const_run_impl(
      std::vector<AtenTensorHandle>& output_handles,
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor);

  void run_impl(
      AtenTensorHandle*
          input_handles, // array of input AtenTensorHandle; handles
                         // are stolen; the array itself is borrowed
      AtenTensorHandle*
          output_handles, // array for writing output AtenTensorHandle; handles
                          // will be stolen by the caller; the array itself is
                          // borrowed
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor);

  template <typename Inputs, typename Outputs>
  Outputs run_impl_minimal_arrayref_interface(
      const Inputs& inputs,
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor);

  static std::unique_ptr<AOTInductorModel> Create(
      std::shared_ptr<ConstantMap> constants_map,
      std::shared_ptr<std::vector<ConstantHandle>> constants_array,
      const std::string& device_str,
      std::optional<std::string> cubin_dir) {
    return std::make_unique<AOTInductorModel>(
        std::move(constants_map),
        std::move(constants_array),
        device_str,
        cubin_dir);
  }

 private:
  std::unique_ptr<AOTInductorModelKernelsBase> kernels_;
};

#ifdef USE_CUDA
class AOTICudaStreamGuard {
 public:
  AOTICudaStreamGuard(cudaStream_t stream, int32_t device_index) {
    CUDAStreamGuardHandle ptr;
    AOTI_TORCH_ERROR_CODE_CHECK(
        aoti_torch_create_cuda_stream_guard(stream, device_index, &ptr));
    guard_ =
        std::unique_ptr<void, std::function<void(void*)>>(ptr, [](void* ptr) {
          AOTI_TORCH_ERROR_CODE_CHECK(aoti_torch_delete_cuda_stream_guard(
              reinterpret_cast<CUDAStreamGuardHandle>(ptr)));
        });
  }

 private:
  std::unique_ptr<void, std::function<void(void*)>> guard_;
};
#endif // USE_CUDA

} // namespace aot_inductor
} // namespace torch
