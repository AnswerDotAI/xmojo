//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
//===----------------------------------------------------------------------===//

#include "xmojo/InteractiveSession.h"

#include "gtest/gtest.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using xmojo::ExecutionResult;
using xmojo::InteractiveSession;
using xmojo::SessionOptions;

namespace {

std::string executablePath;
std::string gpuCachePath;
std::string gpuRuntimePath;

constexpr const char *kernelStory = R"mojo(
from std.gpu import global_idx
from max.gpu.host import DeviceContext
from xmojo.gpu import compile

def increment(
  output: Pointer[Float32, MutAnyOrigin],
  size: Int32,
):
  var i = global_idx.x
  if i < Int(size):
    output[unsafe_offset=i] += 1

with DeviceContext() as context:
  var kernel = compile[increment](context)
  for iteration in range(4):
    var buffer = context.enqueue_create_buffer[DType.float32](16)
    buffer.enqueue_fill(Float32(iteration))
    kernel.enqueue(
      buffer,
      Int32(16),
      grid_dim=1,
      block_dim=16,
    )
    context.synchronize()

    with buffer.map_to_host() as values:
      for i in range(16):
        if values[i] != Float32(iteration + 1):
          raise Error("wrong GPU result at ", i, ": ", values[i])
)mojo";

SessionOptions gpuOptions(const std::string &runtimePath) {
  SessionOptions options;
  options.targetAccelerator = "apple-m5-metal4";
  options.gpuRuntimeLibrary = runtimePath;
  options.gpuCacheDirectory = gpuCachePath;
  return options;
}

std::vector<std::filesystem::path> cacheEntries() {
  std::vector<std::filesystem::path> result;
  if (!std::filesystem::exists(gpuCachePath))
    return result;
  for (const auto &entry : std::filesystem::directory_iterator(gpuCachePath))
    if (entry.path().extension() == ".artifact")
      result.push_back(entry.path());
  return result;
}

std::string diagnosticsText(const ExecutionResult &result) {
  std::string text;
  for (const auto &diagnostic : result.diagnostics)
    text += diagnostic.message;
  return text;
}

} // namespace

TEST(ModularGPUTest, CompilesAndLaunchesACellKernel) {
  auto sessionOr = InteractiveSession::create(gpuOptions(gpuRuntimePath));
  ASSERT_FALSE(sessionOr.isError()) << sessionOr.getError();
  auto session = sessionOr.takeValue();

  auto resultOr = session->execute(kernelStory);

  ASSERT_FALSE(resultOr.isError()) << resultOr.getError();
  ASSERT_TRUE(resultOr->succeeded)
      << (resultOr->runtimeError ? resultOr->runtimeError->message : "")
      << diagnosticsText(*resultOr);

  auto entries = cacheEntries();
  ASSERT_EQ(entries.size(), 1u);
  auto cachedWriteTime = std::filesystem::last_write_time(entries.front());

  auto cachedOr = session->execute(R"mojo(
with DeviceContext() as context:
  var kernel = compile[increment](context)
)mojo");
  ASSERT_FALSE(cachedOr.isError()) << cachedOr.getError();
  ASSERT_TRUE(cachedOr->succeeded) << diagnosticsText(*cachedOr);
  entries = cacheEntries();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(std::filesystem::last_write_time(entries.front()), cachedWriteTime);

  auto invalidCallOr = session->execute(R"mojo(
with DeviceContext() as context:
  var buffer = context.enqueue_create_buffer[DType.float32](16)
  var kernel = compile[increment](context)
  kernel.enqueue(
    buffer,
    Float64(16),
    grid_dim=1,
    block_dim=16,
  )
)mojo");
  ASSERT_FALSE(invalidCallOr.isError()) << invalidCallOr.getError();
  ASSERT_FALSE(invalidCallOr->succeeded);
  EXPECT_NE(diagnosticsText(*invalidCallOr).find("does not match"),
            std::string::npos)
      << diagnosticsText(*invalidCallOr);

  auto reusedOr = InteractiveSession::create(gpuOptions(gpuRuntimePath));
  ASSERT_FALSE(reusedOr.isError()) << reusedOr.getError();
  auto reusedResultOr = (*reusedOr)->execute(kernelStory);
  ASSERT_FALSE(reusedResultOr.isError()) << reusedResultOr.getError();
  ASSERT_TRUE(reusedResultOr->succeeded) << diagnosticsText(*reusedResultOr);
  entries = cacheEntries();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(std::filesystem::last_write_time(entries.front()), cachedWriteTime);

  auto changedOr = InteractiveSession::create(gpuOptions(gpuRuntimePath));
  ASSERT_FALSE(changedOr.isError()) << changedOr.getError();
  auto changedResultOr = (*changedOr)->execute(R"mojo(
from std.gpu import global_idx
from max.gpu.host import DeviceContext
from xmojo.gpu import compile

def increment(
  output: Pointer[Float32, MutAnyOrigin],
  size: Int32,
):
  var i = global_idx.x
  if i < Int(size):
    output[unsafe_offset=i] += 2

with DeviceContext() as context:
  var kernel = compile[increment](context)
)mojo");
  ASSERT_FALSE(changedResultOr.isError()) << changedResultOr.getError();
  ASSERT_TRUE(changedResultOr->succeeded) << diagnosticsText(*changedResultOr);
  EXPECT_EQ(cacheEntries().size(), 2u);

  auto conflictingOr = InteractiveSession::create(gpuOptions(executablePath));
  ASSERT_TRUE(conflictingOr.isError());
  EXPECT_NE(std::string(conflictingOr.getError()).find("already loaded"),
            std::string::npos)
      << conflictingOr.getError();
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  executablePath = argv[0];
  gpuRuntimePath = argv[1];
  const char *temporaryDirectory = std::getenv("TEST_TMPDIR");
  if (!temporaryDirectory)
    return 2;
  gpuCachePath =
      (std::filesystem::path(temporaryDirectory) / "gpu-cache").string();
  for (int index = 1; index + 1 < argc; ++index)
    argv[index] = argv[index + 1];
  --argc;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
