# ===----------------------------------------------------------------------=== #
# Copyright (c) 2026, xmojo contributors.
#
# Licensed under the Apache License v2.0 with LLVM Exceptions:
# ===----------------------------------------------------------------------=== #

from std.collections import OptionalReg
from std.collections.string.string_span import _get_kgen_string
from std.ffi import CStringSlice, c_int, c_size_t, external_call
from std.io.file_descriptor import FileDescriptor

from ._trait import PluginHooks


def _emit_print[O: Origin](
    cstr: CStringSlice[O], file_value: FileDescriptor
):
    external_call["xmojo_emit_print", NoneType](
        cstr.ptr(), c_size_t(len(cstr)), c_int(file_value.value)
    )


struct XMojoPlugin(PluginHooks):
    """Default host plugin that routes `print()` through an xmojo session."""

    comptime name: __mlir_type.`!kgen.string` = _get_kgen_string["default"]()
    comptime print_emit_fn = OptionalReg(_emit_print)
