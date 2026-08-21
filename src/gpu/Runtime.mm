// Copyright (c) 2026, xmojo contributors.
// Licensed under the Apache License v2.0 with LLVM Exceptions.

#include "xmojo/gpu/Runtime.h"

#include "iree/async/frontier_tracker.h"
#include "iree/async/util/proactor_pool.h"
#include "iree/base/internal/flatcc/building.h"
#include "iree/base/internal/flatcc/parsing.h"
#include "iree/base/status_cc.h"
#include "iree/hal/api.h"
#include "iree/hal/device_group.h"
#include "iree/hal/drivers/metal/api.h"
#include "iree/schemas/metal_executable_def_builder.h"

#import <Metal/Metal.h>

#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace xmojo::gpu {
namespace {

void check(iree_status_t status) {
  if (iree_status_is_ok(status))
    return;
  std::string message = iree::Status::ToString(status);
  iree_status_free(status);
  throw std::runtime_error(message);
}

void require(bool condition, std::string_view message) {
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

using DriverRef = Ref<iree_hal_driver_t, iree_hal_driver_release>;
using FrontierTrackerRef =
    Ref<iree_async_frontier_tracker_t, iree_async_frontier_tracker_release>;
using DeviceRef = Ref<iree_hal_device_t, iree_hal_device_release>;
using DeviceGroupRef =
    Ref<iree_hal_device_group_t, iree_hal_device_group_release>;
using BufferRef = Ref<iree_hal_buffer_t, iree_hal_buffer_release>;
using ExecutableRef = Ref<iree_hal_executable_t, iree_hal_executable_release>;
using ExecutableCacheRef =
    Ref<iree_hal_executable_cache_t, iree_hal_executable_cache_release>;
using ProactorPoolRef =
    Ref<iree_async_proactor_pool_t, iree_async_proactor_pool_release>;
using SemaphoreRef = Ref<iree_hal_semaphore_t, iree_hal_semaphore_release>;

template <typename T, auto release, typename Create>
Ref<T, release> createRef(Create &&create) {
  T *value = nullptr;
  check(create(&value));
  return Ref<T, release>(value);
}

std::string copyString(iree_string_view_t value) {
  return std::string(value.data, value.size);
}

void initializeMetal() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  [device release];
}

DriverRef createMetalDriver() {
  initializeMetal();
  iree_hal_metal_device_params_t params;
  iree_hal_metal_device_params_initialize(&params);
  return createRef<iree_hal_driver_t, iree_hal_driver_release>([&](auto **out) {
    return iree_hal_metal_driver_create(iree_make_cstring_view("metal"),
                                        &params, iree_allocator_system(), out);
  });
}

std::vector<DeviceInfo> enumerateMetalDevices(iree_hal_driver_t *driver) {
  iree_host_size_t count = 0;
  iree_hal_device_info_t *infos = nullptr;
  check(iree_hal_driver_query_available_devices(driver, iree_allocator_system(),
                                                &count, &infos));
  std::unique_ptr<iree_hal_device_info_t, decltype([](auto *value) {
                    iree_allocator_free(iree_allocator_system(), value);
                  })>
      owner(infos);

  std::vector<DeviceInfo> result;
  result.reserve(count);
  for (iree_host_size_t ordinal = 0; ordinal < count; ++ordinal) {
    result.push_back({Backend::Metal, ordinal, copyString(infos[ordinal].name),
                      copyString(infos[ordinal].path)});
  }
  return result;
}

size_t parseMetalOrdinal(std::string_view selector) {
  if (selector == "auto" || selector == "metal")
    return 0;
  constexpr std::string_view prefix = "metal:";
  require(selector.starts_with(prefix),
          "device selector must be 'auto', 'metal', or 'metal:<ordinal>'");
  selector.remove_prefix(prefix.size());
  require(!selector.empty(), "Metal device selector is missing an ordinal");
  size_t ordinal = 0;
  auto [end, error] = std::from_chars(
      selector.data(), selector.data() + selector.size(), ordinal);
  require(error == std::errc() && end == selector.data() + selector.size(),
          "Metal device ordinal must be a non-negative integer");
  return ordinal;
}

class FlatbufferBuilder {
public:
  FlatbufferBuilder() {
    require(flatcc_builder_init(&builder_) == 0,
            "failed to initialize FlatCC builder");
  }
  ~FlatbufferBuilder() { flatcc_builder_clear(&builder_); }

  flatbuffers_builder_t *get() { return &builder_; }

private:
  flatbuffers_builder_t builder_;
};

std::vector<uint8_t> packageMetalExecutable(const KernelArtifact &artifact) {
  require(artifact.format == ArtifactFormat::MetalMsl,
          "Metal devices require a Metal MSL artifact");
  require(!artifact.code.empty(), "kernel artifact has no code");
  require(!artifact.entryPoint.empty(), "kernel artifact has no entry point");
  require(artifact.workgroupSize.x && artifact.workgroupSize.y &&
              artifact.workgroupSize.z,
          "kernel workgroup dimensions must be nonzero");
  require(artifact.pushConstantSize % sizeof(uint32_t) == 0,
          "kernel constant size must be a multiple of four bytes");
  size_t constantCount = artifact.pushConstantSize / sizeof(uint32_t);
  require(constantCount <= std::numeric_limits<uint32_t>::max(),
          "kernel constant layout is too large");

  FlatbufferBuilder owner;
  auto *builder = owner.get();
  require(
      !flatbuffers_failed(iree_hal_metal_ExecutableDef_start_as_root(builder)),
      "failed to start Metal executable container");

  auto source = flatbuffers_string_create(
      builder, reinterpret_cast<const char *>(artifact.code.data()),
      artifact.code.size());
  require(source != 0, "failed to store MSL source");
  auto sourceDef = iree_hal_metal_MSLSourceDef_create(
      builder, 0, source); // zero selects the latest supported MSL version
  require(sourceDef != 0, "failed to describe MSL source");
  require(!flatbuffers_failed(iree_hal_metal_LibraryDef_start(builder)) &&
              !flatbuffers_failed(
                  iree_hal_metal_LibraryDef_source_add(builder, sourceDef)),
          "failed to start Metal library");
  auto library = iree_hal_metal_LibraryDef_end(builder);
  require(library != 0, "failed to describe Metal library");
  auto libraries = iree_hal_metal_LibraryDef_vec_create(builder, &library, 1);
  require(libraries != 0, "failed to store Metal library");

  auto entryPoint = flatbuffers_string_create(
      builder, artifact.entryPoint.data(), artifact.entryPoint.size());
  auto publicName = flatbuffers_string_create(
      builder, artifact.entryPoint.data(), artifact.entryPoint.size());
  require(entryPoint != 0 && publicName != 0,
          "failed to store Metal entry point");
  iree_hal_metal_ThreadgroupSize_t threadgroupSize = {artifact.workgroupSize.x,
                                                      artifact.workgroupSize.y,
                                                      artifact.workgroupSize.z};
  std::vector<iree_hal_metal_BindingBits_enum_t> bindingFlags;
  bindingFlags.reserve(artifact.bindings.size());
  for (auto access : artifact.bindings) {
    switch (access) {
    case BindingAccess::ReadOnly:
      bindingFlags.push_back(iree_hal_metal_BindingBits_IMMUTABLE);
      break;
    case BindingAccess::ReadWrite:
      bindingFlags.push_back(0);
      break;
    default:
      throw std::runtime_error("kernel artifact has an unknown binding access");
    }
  }
  auto bindings = iree_hal_metal_BindingBits_vec_create(
      builder, bindingFlags.data(), bindingFlags.size());
  require(bindings != 0 || bindingFlags.empty(),
          "failed to store Metal binding layout");
  require(
      !flatbuffers_failed(iree_hal_metal_PipelineDef_start(builder)) &&
          !flatbuffers_failed(
              iree_hal_metal_PipelineDef_library_ordinal_add(builder, 0)) &&
          !flatbuffers_failed(iree_hal_metal_PipelineDef_entry_point_add(
              builder, entryPoint)) &&
          !flatbuffers_failed(
              iree_hal_metal_PipelineDef_name_add(builder, publicName)) &&
          !flatbuffers_failed(iree_hal_metal_PipelineDef_threadgroup_size_add(
              builder, &threadgroupSize)) &&
          !flatbuffers_failed(iree_hal_metal_PipelineDef_constant_count_add(
              builder, static_cast<uint32_t>(constantCount))) &&
          !flatbuffers_failed(
              iree_hal_metal_PipelineDef_binding_flags_add(builder, bindings)),
      "failed to start Metal pipeline");
  auto pipeline = iree_hal_metal_PipelineDef_end(builder);
  require(pipeline != 0, "failed to describe Metal pipeline");
  auto pipelines = iree_hal_metal_PipelineDef_vec_create(builder, &pipeline, 1);
  require(pipelines != 0, "failed to store Metal pipeline");

  require(!flatbuffers_failed(
              iree_hal_metal_ExecutableDef_pipelines_add(builder, pipelines)) &&
              !flatbuffers_failed(iree_hal_metal_ExecutableDef_libraries_add(
                  builder, libraries)) &&
              iree_hal_metal_ExecutableDef_end_as_root(builder) != 0,
          "failed to finish Metal executable container");

  size_t flatbufferSize = 0;
  void *flatbuffer =
      flatcc_builder_finalize_aligned_buffer(builder, &flatbufferSize);
  require(flatbuffer && flatbufferSize,
          "failed to finalize Metal executable container");
  std::unique_ptr<void, decltype(&flatcc_builder_aligned_free)> data(
      flatbuffer, flatcc_builder_aligned_free);

  iree_flatbuffer_file_header_t header = {};
  std::memcpy(&header.magic, iree_hal_metal_ExecutableDef_file_identifier,
              sizeof(header.magic));
  header.content_size = flatbufferSize;
  std::vector<uint8_t> result(sizeof(header) + flatbufferSize);
  std::memcpy(result.data(), &header, sizeof(header));
  std::memcpy(result.data() + sizeof(header), flatbuffer, flatbufferSize);
  return result;
}

} // namespace

struct Device::Impl {
  DeviceInfo info;
  ProactorPoolRef proactorPool;
  DriverRef driver;
  FrontierTrackerRef frontierTracker;
  DeviceRef device;
  DeviceGroupRef deviceGroup;
  ExecutableCacheRef executableCache;
  SemaphoreRef timeline;
  uint64_t nextTimelineValue = 0;
  std::mutex dispatchMutex;
};

struct Buffer::Impl {
  std::shared_ptr<Device::Impl> device;
  BufferRef buffer;
};

struct Executable::Impl {
  std::shared_ptr<Device::Impl> device;
  ExecutableRef executable;
  iree_hal_executable_function_t function;
  size_t bindingCount;
  size_t constantSize;
};

struct Event::Impl {
  std::shared_ptr<Device::Impl> device;
  uint64_t timelineValue;
};

std::string DeviceInfo::selector() const {
  require(backend == Backend::Metal, "unknown accelerator backend");
  return "metal:" + std::to_string(ordinal);
}

std::vector<DeviceInfo> enumerateDevices() {
  auto driver = createMetalDriver();
  return enumerateMetalDevices(driver.get());
}

Device Device::open(std::string_view selector) {
  size_t ordinal = parseMetalOrdinal(selector);
  auto driver = createMetalDriver();
  auto devices = enumerateMetalDevices(driver.get());
  require(ordinal < devices.size(), "requested Metal device is not available");

  auto proactorPool =
      createRef<iree_async_proactor_pool_t, iree_async_proactor_pool_release>(
          [](auto **out) {
            return iree_async_proactor_pool_create(
                1, nullptr, iree_async_proactor_pool_options_default(),
                iree_allocator_system(), out);
          });
  auto createParams = iree_hal_device_create_params_default();
  createParams.proactor_pool = proactorPool.get();
  auto device =
      createRef<iree_hal_device_t, iree_hal_device_release>([&](auto **out) {
        return iree_hal_driver_create_device_by_ordinal(
            driver.get(), ordinal, 0, nullptr, &createParams,
            iree_allocator_system(), out);
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
      createRef<iree_hal_executable_cache_t, iree_hal_executable_cache_release>(
          [&](auto **out) {
            return iree_hal_executable_cache_create(
                device.get(), iree_make_cstring_view("xmojo"), out);
          });
  auto timeline = createRef<iree_hal_semaphore_t, iree_hal_semaphore_release>(
      [&](auto **out) {
        return iree_hal_semaphore_create(device.get(),
                                         IREE_HAL_QUEUE_AFFINITY_ANY, 0,
                                         IREE_HAL_SEMAPHORE_FLAG_NONE, out);
      });

  auto impl = std::make_shared<Impl>();
  impl->info = std::move(devices[ordinal]);
  impl->proactorPool = std::move(proactorPool);
  impl->driver = std::move(driver);
  impl->frontierTracker = std::move(frontierTracker);
  impl->device = std::move(device);
  impl->deviceGroup = std::move(deviceGroup);
  impl->executableCache = std::move(executableCache);
  impl->timeline = std::move(timeline);
  return Device(std::move(impl));
}

const DeviceInfo &Device::info() const {
  require(bool(impl_), "device is empty");
  return impl_->info;
}

Buffer Device::createBuffer(size_t size) const {
  require(bool(impl_), "device is empty");
  iree_hal_buffer_params_t params = {};
  params.type =
      IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL | IREE_HAL_MEMORY_TYPE_HOST_VISIBLE;
  params.usage =
      IREE_HAL_BUFFER_USAGE_DISPATCH_STORAGE | IREE_HAL_BUFFER_USAGE_MAPPING;
  auto buffer =
      createRef<iree_hal_buffer_t, iree_hal_buffer_release>([&](auto **out) {
        return iree_hal_allocator_allocate_buffer(
            iree_hal_device_allocator(impl_->device.get()), params, size, out);
      });
  return Buffer(
      std::make_shared<Buffer::Impl>(Buffer::Impl{impl_, std::move(buffer)}));
}

Executable Device::load(const KernelArtifact &artifact) const {
  require(bool(impl_), "device is empty");
  auto packaged = packageMetalExecutable(artifact);
  iree_hal_executable_params_t params;
  iree_hal_executable_params_initialize(&params);
  params.executable_format = iree_make_cstring_view("metal-msl-fb");
  params.executable_data =
      iree_make_const_byte_span(packaged.data(), packaged.size());
  auto executable =
      createRef<iree_hal_executable_t, iree_hal_executable_release>(
          [&](auto **out) {
            return iree_hal_executable_cache_prepare_executable(
                impl_->executableCache.get(), &params, out);
          });
  iree_hal_executable_function_t function;
  check(iree_hal_executable_lookup_function_by_name(
      executable.get(),
      iree_make_string_view(artifact.entryPoint.data(),
                            artifact.entryPoint.size()),
      &function));
  return Executable(std::make_shared<Executable::Impl>(
      Executable::Impl{impl_, std::move(executable), function,
                       artifact.bindings.size(), artifact.pushConstantSize}));
}

size_t Buffer::size() const {
  require(bool(impl_), "buffer is empty");
  return iree_hal_buffer_byte_length(impl_->buffer.get());
}

BufferView Buffer::view(size_t offset, size_t length) const {
  require(bool(impl_), "buffer is empty");
  require(offset <= size(), "buffer view offset is out of bounds");
  if (length == std::dynamic_extent)
    length = size() - offset;
  require(length <= size() - offset, "buffer view length is out of bounds");
  return {*this, offset, length};
}

void Buffer::write(std::span<const std::byte> source, size_t offset) const {
  require(bool(impl_), "buffer is empty");
  require(offset <= size() && source.size() <= size() - offset,
          "buffer write is out of bounds");
  check(iree_hal_buffer_map_write(impl_->buffer.get(), offset, source.data(),
                                  source.size()));
}

void Buffer::read(std::span<std::byte> destination, size_t offset) const {
  require(bool(impl_), "buffer is empty");
  require(offset <= size() && destination.size() <= size() - offset,
          "buffer read is out of bounds");
  check(iree_hal_buffer_map_read(impl_->buffer.get(), offset,
                                 destination.data(), destination.size()));
}

Event Executable::dispatch(std::span<const BufferView> bindings,
                           std::span<const std::byte> constants,
                           Extent3D workgroups) const {
  require(bool(impl_), "executable is empty");
  require(bindings.size() == impl_->bindingCount,
          "dispatch binding count does not match the artifact");
  require(constants.size() == impl_->constantSize,
          "dispatch constant size does not match the artifact");
  require(workgroups.x && workgroups.y && workgroups.z,
          "dispatch workgroup dimensions must be nonzero");

  std::vector<iree_hal_buffer_ref_t> bufferRefs;
  bufferRefs.reserve(bindings.size());
  for (const auto &binding : bindings) {
    require(bool(binding.buffer.impl_), "dispatch binding is empty");
    require(binding.offset <= binding.buffer.size() &&
                binding.length <= binding.buffer.size() - binding.offset,
            "dispatch binding range is out of bounds");
    require(binding.buffer.impl_->device == impl_->device,
            "dispatch binding belongs to another device");
    bufferRefs.push_back(iree_hal_make_buffer_ref(
        binding.buffer.impl_->buffer.get(), binding.offset, binding.length));
  }
  iree_hal_buffer_ref_list_t bindingList = {bufferRefs.size(),
                                            bufferRefs.data()};
  auto constantBytes =
      iree_make_const_byte_span(constants.data(), constants.size());
  uint64_t timelineValue;
  {
    std::lock_guard lock(impl_->device->dispatchMutex);
    timelineValue = ++impl_->device->nextTimelineValue;
    iree_hal_semaphore_t *semaphore = impl_->device->timeline.get();
    iree_hal_semaphore_list_t signals = {1, &semaphore, &timelineValue};
    check(iree_hal_device_queue_dispatch(
        impl_->device->device.get(), IREE_HAL_QUEUE_AFFINITY_ANY,
        iree_hal_semaphore_list_empty(), signals, impl_->executable.get(),
        impl_->function,
        iree_hal_make_static_dispatch_config(workgroups.x, workgroups.y,
                                             workgroups.z),
        constantBytes, bindingList, IREE_HAL_DISPATCH_FLAG_NONE));
  }
  return Event(
      std::make_shared<Event::Impl>(Event::Impl{impl_->device, timelineValue}));
}

void Event::wait() const {
  require(bool(impl_), "event is empty");
  check(iree_hal_semaphore_wait(impl_->device->timeline.get(),
                                impl_->timelineValue, iree_infinite_timeout(),
                                IREE_ASYNC_WAIT_FLAG_NONE));
}

} // namespace xmojo::gpu
