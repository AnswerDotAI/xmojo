# Copyright (c) 2026, xmojo contributors.
# Licensed under the Apache License v2.0 with LLVM Exceptions.

# Each entry is a separate interactive cell. The parameterized kernel and the
# device allocation must survive the boundary without replaying earlier cells.
CELLS = (
    '''from max.gpu import global_idx
from max.gpu.host import DeviceContext

def increment[amount: Int](output: Pointer[Float32, MutAnyOrigin], size: Int32):
    var i = global_idx.x
    if i < Int(size):
        output[unsafe_offset=i] += Float32(amount)
''',
    '''var context = DeviceContext()
var buffer = context.enqueue_create_buffer[DType.float32](16)
buffer.enqueue_fill(0)
print("allocated")
''',
    '''context.enqueue_function[increment[2]](
    buffer, Int32(16), grid_dim=1, block_dim=16
)
context.synchronize()
''',
    '''with buffer.map_to_host() as values:
    for i in range(16):
        if values[i] != 2:
            raise Error("wrong GPU result at ", i, ": ", values[i])
print("GPU result verified")
''',
)
