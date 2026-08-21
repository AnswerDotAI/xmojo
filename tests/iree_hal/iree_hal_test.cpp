#include "iree/async/util/proactor_pool.h"
#include "iree/async/frontier_tracker.h"
#include "iree/base/internal/flatcc/building.h"
#include "iree/base/internal/flatcc/parsing.h"
#include "iree/base/status_cc.h"
#include "iree/hal/api.h"
#include "iree/hal/device_group.h"
#include "iree/hal/drivers/metal/api.h"
#include "iree/schemas/metal_executable_def_builder.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

extern "C" bool xmojo_metal_has_default_device(void);
extern "C" size_t xmojo_metal_device_count(void);

void check(iree_status_t status) {
  if (iree_status_is_ok(status))
    return;
  std::string message = iree::Status::ToString(status);
  iree_status_free(status);
  throw std::runtime_error(message);
}

void expect(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

template <typename T, auto release> struct Releaser {
  void operator()(T *value) const {
    if (value)
      release(value);
  }
};

template <typename T, auto release>
using Ref = std::unique_ptr<T, Releaser<T, release>>;

using Driver = Ref<iree_hal_driver_t, iree_hal_driver_release>;
using FrontierTracker =
    Ref<iree_async_frontier_tracker_t, iree_async_frontier_tracker_release>;
using Device = Ref<iree_hal_device_t, iree_hal_device_release>;
using DeviceGroup =
    Ref<iree_hal_device_group_t, iree_hal_device_group_release>;
using Buffer = Ref<iree_hal_buffer_t, iree_hal_buffer_release>;
using Executable = Ref<iree_hal_executable_t, iree_hal_executable_release>;
using ExecutableCache =
    Ref<iree_hal_executable_cache_t, iree_hal_executable_cache_release>;
using ProactorPool =
    Ref<iree_async_proactor_pool_t, iree_async_proactor_pool_release>;
using Semaphore = Ref<iree_hal_semaphore_t, iree_hal_semaphore_release>;

template <typename T, auto release, typename Create>
Ref<T, release> createRef(Create &&create) {
  T *value = nullptr;
  check(create(&value));
  return Ref<T, release>(value);
}

struct Runtime {
  Driver driver;
  FrontierTracker frontierTracker;
  Device device;
  DeviceGroup deviceGroup;
  ExecutableCache executableCache;
};

Runtime createRuntime() {
  auto pool = createRef<iree_async_proactor_pool_t,
                        iree_async_proactor_pool_release>([](auto **out) {
    return iree_async_proactor_pool_create(
        1, nullptr, iree_async_proactor_pool_options_default(),
        iree_allocator_system(), out);
  });

  iree_hal_metal_device_params_t metalParams;
  iree_hal_metal_device_params_initialize(&metalParams);
  auto driver = createRef<iree_hal_driver_t, iree_hal_driver_release>(
      [&](auto **out) {
        return iree_hal_metal_driver_create(iree_make_cstring_view("metal"),
                                            &metalParams,
                                            iree_allocator_system(), out);
      });

  auto createParams = iree_hal_device_create_params_default();
  createParams.proactor_pool = pool.get();
  auto device = createRef<iree_hal_device_t, iree_hal_device_release>(
      [&](auto **out) {
        return iree_hal_driver_create_default_device(
            driver.get(), &createParams, iree_allocator_system(), out);
      });
  auto frontierTracker =
      createRef<iree_async_frontier_tracker_t,
                iree_async_frontier_tracker_release>([&](auto **out) {
        return iree_async_frontier_tracker_create(
            iree_async_frontier_tracker_options_default(),
            iree_allocator_system(), out);
      });
  auto deviceGroup =
      createRef<iree_hal_device_group_t, iree_hal_device_group_release>(
          [&](auto **out) {
            return iree_hal_device_group_create_from_device(
                device.get(), frontierTracker.get(), iree_allocator_system(),
                out);
          });
  auto executableCache =
      createRef<iree_hal_executable_cache_t,
                iree_hal_executable_cache_release>([&](auto **out) {
        return iree_hal_executable_cache_create(
            device.get(), iree_make_cstring_view("xmojo"), out);
      });
  return {std::move(driver), std::move(frontierTracker), std::move(device),
          std::move(deviceGroup), std::move(executableCache)};
}

class FlatbufferBuilder {
public:
  FlatbufferBuilder() {
    expect(flatcc_builder_init(&builder_) == 0,
           "failed to initialize FlatCC builder");
  }
  ~FlatbufferBuilder() { flatcc_builder_clear(&builder_); }

  flatbuffers_builder_t *get() { return &builder_; }

private:
  flatbuffers_builder_t builder_;
};

std::vector<uint8_t> makeMetalExecutable(std::string_view source,
                                         std::string_view entryPoint) {
  FlatbufferBuilder owner;
  auto *builder = owner.get();
  expect(!flatbuffers_failed(
             iree_hal_metal_ExecutableDef_start_as_root(builder)),
         "failed to start Metal executable container");

  auto sourceString =
      flatbuffers_string_create(builder, source.data(), source.size());
  expect(sourceString != 0, "failed to store MSL source");
  auto sourceDef = iree_hal_metal_MSLSourceDef_create(
      builder, 196608, sourceString); // MTLLanguageVersion3_0
  expect(sourceDef != 0, "failed to describe MSL source");
  expect(!flatbuffers_failed(iree_hal_metal_LibraryDef_start(builder)) &&
             !flatbuffers_failed(
                 iree_hal_metal_LibraryDef_source_add(builder, sourceDef)),
         "failed to start Metal library");
  auto library = iree_hal_metal_LibraryDef_end(builder);
  expect(library != 0, "failed to describe Metal library");
  auto libraries =
      iree_hal_metal_LibraryDef_vec_create(builder, &library, 1);
  expect(libraries != 0, "failed to store Metal library");

  auto entryPointString = flatbuffers_string_create(
      builder, entryPoint.data(), entryPoint.size());
  auto publicNameString = flatbuffers_string_create(
      builder, entryPoint.data(), entryPoint.size());
  expect(entryPointString != 0 && publicNameString != 0,
         "failed to store Metal entry point");
  iree_hal_metal_ThreadgroupSize_t threadgroupSize = {64, 1, 1};
  iree_hal_metal_BindingBits_enum_t mutableBinding = 0;
  auto bindingFlags = iree_hal_metal_BindingBits_vec_create(
      builder, &mutableBinding, 1);
  expect(bindingFlags != 0, "failed to store Metal binding layout");
  expect(!flatbuffers_failed(iree_hal_metal_PipelineDef_start(builder)) &&
             !flatbuffers_failed(iree_hal_metal_PipelineDef_library_ordinal_add(
                 builder, 0)) &&
             !flatbuffers_failed(iree_hal_metal_PipelineDef_entry_point_add(
                 builder, entryPointString)) &&
             !flatbuffers_failed(iree_hal_metal_PipelineDef_name_add(
                 builder, publicNameString)) &&
             !flatbuffers_failed(iree_hal_metal_PipelineDef_threadgroup_size_add(
                 builder, &threadgroupSize)) &&
             !flatbuffers_failed(iree_hal_metal_PipelineDef_constant_count_add(
                 builder, 1)) &&
             !flatbuffers_failed(iree_hal_metal_PipelineDef_binding_flags_add(
                 builder, bindingFlags)),
         "failed to start Metal pipeline");
  auto pipeline = iree_hal_metal_PipelineDef_end(builder);
  expect(pipeline != 0, "failed to describe Metal pipeline");
  auto pipelines =
      iree_hal_metal_PipelineDef_vec_create(builder, &pipeline, 1);
  expect(pipelines != 0, "failed to store Metal pipeline");

  expect(!flatbuffers_failed(
             iree_hal_metal_ExecutableDef_pipelines_add(builder, pipelines)) &&
             !flatbuffers_failed(iree_hal_metal_ExecutableDef_libraries_add(
                 builder, libraries)) &&
             iree_hal_metal_ExecutableDef_end_as_root(builder) != 0,
         "failed to finish Metal executable container");

  size_t flatbufferSize = 0;
  void *flatbuffer =
      flatcc_builder_finalize_aligned_buffer(builder, &flatbufferSize);
  expect(flatbuffer != nullptr && flatbufferSize != 0,
         "failed to finalize Metal executable container");
  std::unique_ptr<void, decltype(&flatcc_builder_aligned_free)> data(
      flatbuffer, flatcc_builder_aligned_free);

  iree_flatbuffer_file_header_t header = {};
  std::memcpy(&header.magic,
              iree_hal_metal_ExecutableDef_file_identifier,
              sizeof(header.magic));
  header.content_size = flatbufferSize;
  std::vector<uint8_t> result(sizeof(header) + flatbufferSize);
  std::memcpy(result.data(), &header, sizeof(header));
  std::memcpy(result.data() + sizeof(header), flatbuffer, flatbufferSize);
  return result;
}

Executable loadExecutable(Runtime &runtime, std::string_view source,
                          std::string_view entryPoint) {
  auto artifact = makeMetalExecutable(source, entryPoint);
  iree_hal_executable_params_t params;
  iree_hal_executable_params_initialize(&params);
  params.executable_format = iree_make_cstring_view("metal-msl-fb");
  params.executable_data =
      iree_make_const_byte_span(artifact.data(), artifact.size());
  return createRef<iree_hal_executable_t, iree_hal_executable_release>(
      [&](auto **out) {
        return iree_hal_executable_cache_prepare_executable(
            runtime.executableCache.get(), &params, out);
      });
}

Buffer createBuffer(Runtime &runtime, std::span<const uint32_t> values) {
  iree_hal_buffer_params_t params = {};
  params.type =
      IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL | IREE_HAL_MEMORY_TYPE_HOST_VISIBLE;
  params.usage =
      IREE_HAL_BUFFER_USAGE_DISPATCH_STORAGE | IREE_HAL_BUFFER_USAGE_MAPPING;
  auto buffer = createRef<iree_hal_buffer_t, iree_hal_buffer_release>(
      [&](auto **out) {
        return iree_hal_allocator_allocate_buffer(
            iree_hal_device_allocator(runtime.device.get()), params,
            values.size_bytes(), out);
      });
  check(iree_hal_buffer_map_write(buffer.get(), 0, values.data(),
                                  values.size_bytes()));
  return buffer;
}

std::vector<uint32_t> readBuffer(Buffer &buffer, size_t size) {
  std::vector<uint32_t> values(size);
  check(iree_hal_buffer_map_read(buffer.get(), 0, values.data(),
                                 values.size() * sizeof(values.front())));
  return values;
}

class Timeline {
public:
  explicit Timeline(iree_hal_device_t *device)
      : semaphore_(createRef<iree_hal_semaphore_t,
                             iree_hal_semaphore_release>([&](auto **out) {
          return iree_hal_semaphore_create(
              device, IREE_HAL_QUEUE_AFFINITY_ANY, 0,
              IREE_HAL_SEMAPHORE_FLAG_NONE, out);
        })) {}

  void dispatch(iree_hal_device_t *device, iree_hal_executable_t *executable,
                std::string_view functionName, iree_hal_buffer_t *buffer,
                uint32_t constant) {
    iree_hal_executable_function_t function;
    check(iree_hal_executable_lookup_function_by_name(
        executable,
        iree_make_string_view(functionName.data(), functionName.size()),
        &function));
    auto binding = iree_hal_make_buffer_ref(
        buffer, 0, iree_hal_buffer_byte_length(buffer));
    iree_hal_buffer_ref_list_t bindings = {1, &binding};
    auto constants = iree_make_const_byte_span(&constant, sizeof(constant));
    ++value_;
    iree_hal_semaphore_t *signalSemaphore = semaphore_.get();
    iree_hal_semaphore_list_t signals = {1, &signalSemaphore, &value_};
    check(iree_hal_device_queue_dispatch(
        device, IREE_HAL_QUEUE_AFFINITY_ANY,
        iree_hal_semaphore_list_empty(), signals, executable, function,
        iree_hal_make_static_dispatch_config(1, 1, 1), constants, bindings,
        IREE_HAL_DISPATCH_FLAG_NONE));
    check(iree_hal_semaphore_wait(semaphore_.get(), value_,
                                  iree_infinite_timeout(),
                                  IREE_ASYNC_WAIT_FLAG_NONE));
  }

private:
  Semaphore semaphore_;
  uint64_t value_ = 0;
};

constexpr std::string_view addSource = R"msl(
#include <metal_stdlib>
using namespace metal;

struct Bindings {
  device uint *values [[id(0)]];
};

kernel void add(constant Bindings &bindings [[buffer(0)]],
                constant uint &amount [[buffer(3)]],
                uint index [[thread_position_in_grid]]) {
  bindings.values[index] += amount;
}
)msl";

constexpr std::string_view multiplySource = R"msl(
#include <metal_stdlib>
using namespace metal;

struct Bindings {
  device uint *values [[id(0)]];
};

kernel void multiply(constant Bindings &bindings [[buffer(0)]],
                     constant uint &amount [[buffer(3)]],
                     uint index [[thread_position_in_grid]]) {
  bindings.values[index] *= amount;
}
)msl";

} // namespace

int main() {
  expect(xmojo_metal_has_default_device(),
         "Metal does not expose a system default device");
  expect(xmojo_metal_device_count() != 0,
         "Metal exposes a default device, but MTLCopyAllDevices returned none");
  auto runtime = createRuntime();
  std::array<uint32_t, 64> initial;
  for (size_t i = 0; i < initial.size(); ++i)
    initial[i] = static_cast<uint32_t>(i);
  auto buffer = createBuffer(runtime, initial);
  Timeline timeline(runtime.device.get());

  auto executable = loadExecutable(runtime, addSource, "add");
  timeline.dispatch(runtime.device.get(), executable.get(), "add", buffer.get(),
                    1);
  timeline.dispatch(runtime.device.get(), executable.get(), "add", buffer.get(),
                    2);
  auto added = readBuffer(buffer, initial.size());
  for (size_t i = 0; i < initial.size(); ++i)
    expect(added[i] == initial[i] + 3,
           "repeated dispatch did not preserve buffer state");

  executable.reset();
  executable = loadExecutable(runtime, multiplySource, "multiply");
  timeline.dispatch(runtime.device.get(), executable.get(), "multiply",
                    buffer.get(), 2);
  auto multiplied = readBuffer(buffer, initial.size());
  for (size_t i = 0; i < initial.size(); ++i)
    expect(multiplied[i] == (initial[i] + 3) * 2,
           "replacing the executable disturbed persistent device state");
}
