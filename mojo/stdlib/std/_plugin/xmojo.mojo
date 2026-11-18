# ===----------------------------------------------------------------------=== #
# Copyright (c) 2026, xmojo contributors.
#
# Licensed under the Apache License v2.0 with LLVM Exceptions:
# ===----------------------------------------------------------------------=== #

from std.collections import OptionalReg
from std.ffi import CStringSpan, c_size_t, external_call
from std.io.file_descriptor import RawFileDescriptor

from ._trait import PluginHooks, TargetDeviceKind


def _emit_print[O: Origin](
    cstr: CStringSpan[O], file_value: RawFileDescriptor
):
    external_call["xmojo_emit_print", NoneType](
        cstr.ptr(), c_size_t(len(cstr)), file_value._raw()
    )


struct XMojoPlugin(PluginHooks):
    """Default host plugin that routes `print()` through an xmojo session."""

    comptime device_kind = TargetDeviceKind.cpu
    comptime print_emit_fn = OptionalReg(_emit_print)
