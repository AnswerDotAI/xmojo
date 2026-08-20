# ===----------------------------------------------------------------------=== #
# Copyright (c) 2026, xmojo contributors.
#
# Licensed under the Apache License v2.0 with LLVM Exceptions:
# ===----------------------------------------------------------------------=== #

from ._trait import PluginHooks
from .selector import PluginSelector
from .cuda import CUDAPlugin
from .hip import HIPPlugin
from .metal import MetalPlugin
from .xmojo import XMojoPlugin

comptime STD_PLUGINS = PluginSelector[
    XMojoPlugin, MetalPlugin, CUDAPlugin, HIPPlugin
]
