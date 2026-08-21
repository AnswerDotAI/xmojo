// Copyright (c) 2026, xmojo contributors.
// Licensed under the Apache License v2.0 with LLVM Exceptions.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xmojo::gpu {

enum class Backend { Metal };
enum class ArtifactFormat { MetalMsl };
enum class BindingAccess { ReadOnly, ReadWrite };

struct Extent3D {
  uint32_t x = 1;
  uint32_t y = 1;
  uint32_t z = 1;
};

struct DeviceInfo {
  Backend backend;
  size_t ordinal;
  std::string name;
  std::string path;

  std::string selector() const;
};

struct KernelArtifact {
  ArtifactFormat format;
  std::vector<uint8_t> code;
  std::string entryPoint;
  Extent3D workgroupSize;
  std::vector<BindingAccess> bindings;
  size_t pushConstantSize = 0;
};

class Device;
class Executable;
struct BufferView;

class Buffer {
public:
  Buffer() = default;

  size_t size() const;
  BufferView view(size_t offset = 0, size_t length = std::dynamic_extent) const;
  void write(std::span<const std::byte> source, size_t offset = 0) const;
  void read(std::span<std::byte> destination, size_t offset = 0) const;
  explicit operator bool() const { return bool(impl_); }

private:
  struct Impl;
  explicit Buffer(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
  std::shared_ptr<Impl> impl_;

  friend class Device;
  friend class Executable;
};

struct BufferView {
  Buffer buffer;
  size_t offset;
  size_t length;
};

class Event {
public:
  Event() = default;

  // Waits for this dispatch and all earlier work on the same device timeline.
  void wait() const;
  explicit operator bool() const { return bool(impl_); }

private:
  struct Impl;
  explicit Event(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
  std::shared_ptr<Impl> impl_;

  friend class Executable;
};

class Executable {
public:
  Executable() = default;

  // Enqueues work and returns immediately. IREE retains submitted resources
  // until completion, independently of the lifetime of the returned event.
  Event dispatch(std::span<const BufferView> bindings,
                 std::span<const std::byte> constants,
                 Extent3D workgroups = {}) const;
  explicit operator bool() const { return bool(impl_); }

private:
  struct Impl;
  explicit Executable(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
  std::shared_ptr<Impl> impl_;

  friend class Device;
};

class Device {
public:
  Device() = default;

  // Opens "auto", "metal", or an explicit "metal:<ordinal>" selector.
  static Device open(std::string_view selector = "auto");
  const DeviceInfo &info() const;
  Buffer createBuffer(size_t size) const;
  Executable load(const KernelArtifact &artifact) const;
  explicit operator bool() const { return bool(impl_); }

private:
  struct Impl;
  explicit Device(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
  std::shared_ptr<Impl> impl_;

  friend class Buffer;
  friend class Event;
  friend class Executable;
};

std::vector<DeviceInfo> enumerateDevices();

} // namespace xmojo::gpu
