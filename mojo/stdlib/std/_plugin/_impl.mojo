# ===----------------------------------------------------------------------=== #
# Copyright (c) 2026, xmojo contributors.
#
# Licensed under the Apache License v2.0 with LLVM Exceptions:
# ===----------------------------------------------------------------------=== #

from std.sys.info import CompilationTarget
from ._trait import PluginHooks
from .xmojo import XMojoPlugin

# Preserve the target's device hooks during native GPU offload compilation.
# Only host print emissions are routed to the interactive session.
comptime CurrentPlugin: PluginHooks = (
    CompilationTarget.current().stdlib_plugin
    if CompilationTarget.current().is_accelerator()
    else XMojoPlugin
)
