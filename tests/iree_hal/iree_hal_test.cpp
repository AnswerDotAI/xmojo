// Copyright (c) 2026, xmojo contributors.
// Licensed under the Apache License v2.0 with LLVM Exceptions.

#include "xmojo/gpu/Runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace xmojo::gpu;

namespace {

void expect(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

KernelArtifact mslArtifact(std::string_view source, std::string entryPoint) {
  return {ArtifactFormat::MetalMsl,
          std::vector<uint8_t>(source.begin(), source.end()),
          std::move(entryPoint),
          {64, 1, 1},
          {BindingAccess::ReadWrite},
          sizeof(uint32_t)};
}

std::vector<uint32_t> readValues(const Buffer &buffer, size_t count) {
  std::vector<uint32_t> result(count);
  buffer.read(std::as_writable_bytes(std::span(result)));
  return result;
}

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

void runStory() {
  auto available = enumerateDevices();
  expect(!available.empty(), "IREE did not discover a Metal device");
  expect(available.front().selector() == "metal:0",
         "the first Metal device has an unexpected selector");
  expect(Device::open("auto").info().selector() == "metal:0",
         "auto did not select the first available backend");
  expect(Device::open("metal").info().selector() == "metal:0",
         "the backend selector did not select its first device");

  auto device = Device::open("metal:0");
  constexpr size_t elementCount = 64;
  std::array<uint32_t, elementCount + 2> initial;
  for (size_t i = 0; i < initial.size(); ++i)
    initial[i] = static_cast<uint32_t>(i);
  auto buffer = device.createBuffer(sizeof(initial));
  buffer.write(std::as_bytes(std::span(initial)));
  std::array bindings = {
      buffer.view(sizeof(uint32_t), elementCount * sizeof(uint32_t))};

  auto executable = device.load(mslArtifact(addSource, "add"));
  std::array<uint32_t, 1> one = {1};
  std::array<uint32_t, 1> two = {2};
  executable.dispatch(bindings, std::as_bytes(std::span(one)));
  auto addedEvent =
      executable.dispatch(bindings, std::as_bytes(std::span(two)));
  executable = {};
  addedEvent.wait();
  auto added = readValues(buffer, initial.size());
  expect(added.front() == initial.front() && added.back() == initial.back(),
         "dispatch modified values outside its buffer view");
  for (size_t i = 1; i <= elementCount; ++i)
    expect(added[i] == initial[i] + 3,
           "repeated dispatch did not preserve buffer state");

  executable = device.load(mslArtifact(multiplySource, "multiply"));
  auto multipliedEvent =
      executable.dispatch(bindings, std::as_bytes(std::span(two)));
  multipliedEvent.wait();
  auto multiplied = readValues(buffer, initial.size());
  expect(multiplied.front() == initial.front() &&
             multiplied.back() == initial.back(),
         "replacement executable modified values outside its buffer view");
  for (size_t i = 1; i <= elementCount; ++i)
    expect(multiplied[i] == (initial[i] + 3) * 2,
           "replacing the executable disturbed persistent device state");
}

} // namespace

int main() {
  try {
    runStory();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
