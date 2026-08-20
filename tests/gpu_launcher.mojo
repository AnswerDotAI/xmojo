# ===----------------------------------------------------------------------=== #
# Copyright (c) 2026, xmojo contributors.
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# ===----------------------------------------------------------------------=== #

from std.ffi import c_int
from std.gpu import global_idx
from std.runtime import initialize_runtime

from max.gpu.host import DeviceBuffer, DeviceContext


def increment(
    output: Pointer[Float32, MutAnyOrigin],
    size: Int32,
):
    var i = global_idx.x
    if i < Int(size):
        output[unsafe_offset=i] += 1


@export("xmojo_test_launch")
def launch(
    context: Pointer[DeviceContext, MutAnyOrigin],
    output: Pointer[DeviceBuffer[DType.float32], MutAnyOrigin],
    size: c_int,
    grid: c_int,
    block: c_int,
) abi("C") -> c_int:
    initialize_runtime()
    try:
        context[].enqueue_function[increment](
            output[],
            Int32(size),
            grid_dim=Int(grid),
            block_dim=Int(block),
        )
        context[].synchronize()
        return 0
    except:
        return 1
