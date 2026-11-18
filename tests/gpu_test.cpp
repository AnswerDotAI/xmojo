//===----------------------------------------------------------------------===//
// Copyright (c) 2026, xmojo contributors.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
//===----------------------------------------------------------------------===//

#include "xmojo/InteractiveSession.h"
#include "gtest/gtest.h"

using xmojo::InteractiveSession;

namespace {

std::string diagnosticsText(const xmojo::ExecutionResult &result) {
  std::string text;
  for (const auto &diagnostic : result.diagnostics)
    text += diagnostic.message;
  if (result.runtimeError)
    text += result.runtimeError->message;
  return text;
}

constexpr const char *kernelStory = R"mojo(
from max.gpu import global_idx
from max.gpu.host import DeviceContext

def increment(output: Pointer[Float32, MutAnyOrigin], size: Int32):
    var i = global_idx.x
    if i < Int(size):
        output[unsafe_offset=i] += 1

with DeviceContext() as context:
    for iteration in range(4):
        var buffer = context.enqueue_create_buffer[DType.float32](16)
        buffer.enqueue_fill(Float32(iteration))
        context.enqueue_function[increment](
            buffer, Int32(16), grid_dim=1, block_dim=16
        )
        context.synchronize()
        with buffer.map_to_host() as values:
            for i in range(16):
                if values[i] != Float32(iteration + 1):
                    raise Error("wrong GPU result at ", i, ": ", values[i])
)mojo";

TEST(NativeGPU, RunsOrdinaryMAXKernelInIndependentSessions) {
  for (int iteration = 0; iteration != 2; ++iteration) {
    auto session = InteractiveSession::create();
    ASSERT_FALSE(session.isError()) << session.getError();
    auto result = (*session)->execute(kernelStory);
    ASSERT_FALSE(result.isError()) << result.getError();
    ASSERT_TRUE(result->succeeded) << diagnosticsText(*result);
  }
}

} // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
