"""Internal target definition for xmojo's portable SPIR-V compiler."""

comptime _target = __mlir_attr[
    `#kgen.target<triple = "spirv64-unknown-vulkan1.3-compute", `,
    `arch = "", features = "", `,
    `data_layout = "e-p:64:64-i64:64-v16:16-v24:32-v32:32-v48:64-`,
    `v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G1", `,
    `simd_bit_width = 128, index_bit_width = 64`,
    `> : !kgen.target`,
]
