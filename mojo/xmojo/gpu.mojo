# ===----------------------------------------------------------------------=== #
# Copyright (c) 2026, xmojo contributors.
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# ===----------------------------------------------------------------------=== #
"""Checked loading and dispatch for externally compiled Mojo GPU kernels."""

from std.builtin.device_passable import DevicePassable
from std.collections import Array, List, Span
from std.ffi import c_int, c_size_t, external_call
from std.math import align_up
from std.reflection import call_location, reflect, reflect_fn
from std.sys import size_of
from std.sys.info import _TargetType

from max.gpu.host import DeviceContext, Dim, LaunchAttribute
from max.gpu.host._device_context_metal import call_with_pack_checked_metal
from max.gpu.host.device_context import (
    DefaultDeviceTypeEncoder,
    DeviceExternalFunction,
    _checked_call,
)


struct CompiledKernel[
    func_type: TrivialRegisterPassable,
    //,
    func: func_type,
    declared_arg_types: TypeList[Trait=AnyType, ...],
    *,
    target: _TargetType = DeviceContext.default_device_info.target(),
]:
    """A checked handle to an externally compiled GPU function.

    The original Mojo function remains a compile-time parameter, allowing the
    launch method to validate and convert host arguments using the same
    `DevicePassable` contract as MAX's in-process compiled functions.
    """

    var _function: DeviceExternalFunction
    var _context: DeviceContext

    def __init__(
        out self,
        context: DeviceContext,
        *,
        var function_name: String,
        var object: String,
    ) raises:
        self._function = context.load_function[Self.func](
            function_name=function_name^,
            asm=object^,
        )
        self._context = context

    @staticmethod
    def _validate_arguments[
        *Ts: DevicePassable,
        num_args: Int,
    ]() -> Tuple[Int, Array[Int, num_args]]:
        comptime assert (
            Self.declared_arg_types.length == num_args
        ), "wrong number of arguments to enqueue"

        var offset = 0
        var translated_offsets = Array[Int, num_args](uninitialized=True)
        var translated_count = 0
        comptime for i in range(num_args):
            comptime declared = Self.declared_arg_types[i]
            comptime actual = Ts[i]
            comptime matches = actual._is_convertible_to_device_type[declared]()
            comptime declared_name = (
                String(declared.get_type_name()) if conforms_to(
                    declared, DevicePassable
                ) else String(reflect[declared].name())
            )
            comptime assert matches, String(
                "argument #",
                i,
                " of type '",
                actual.get_type_name(),
                "' does not match the kernel argument type '",
                declared_name,
                "'",
            )

            var aligned_size = align_up(
                size_of[actual.device_type, target=Self.target](), 8
            )
            if aligned_size:
                translated_offsets[i] = offset
                translated_count += 1
                offset += aligned_size
            else:
                translated_offsets[i] = -1
        return translated_count, translated_offsets^

    @always_inline
    def enqueue[
        *Ts: DevicePassable,
    ](
        self,
        *args: *Ts,
        grid_dim: Dim,
        block_dim: Dim,
        shared_mem_bytes: Int = 0,
    ) raises:
        """Enqueues the kernel after checking and encoding its arguments."""
        comptime num_args = Ts.length
        var validated = Self._validate_arguments[*Ts, num_args=num_args]()
        var translated_count = validated[0]
        var translated_offsets = validated[1].copy()

        @__parameter
        def argument_storage_size() -> Int:
            var result = 8
            comptime for i in range(num_args):
                result += align_up(
                    size_of[Ts[i].device_type, target=Self.target](), 8
                )
            return result

        comptime storage_size = argument_storage_size()
        var translated = Array[Byte, storage_size](uninitialized=True)
        var start = Int(translated.unsafe_ptr())
        var extra_align = align_up(start, 8) - start
        var addresses = Array[OpaquePointer[MutAnyOrigin], num_args](
            uninitialized=True
        )
        var attributes = List[LaunchAttribute]()

        if self._context.api() == "metal":
            var no_capture_sizes = Array[UInt64, 1](fill=0)
            call_with_pack_checked_metal[
                Self.func,
                *Ts,
                num_passed_args=num_args,
                num_captures_static=0,
            ](
                self._context,
                *args,
                func_handle=self._function._handle,
                device_context=self._context,
                capture_sizes=no_capture_sizes.unsafe_ptr()
                .as_imm()
                .unsafe_origin_cast[ImmUntrackedOrigin](),
                num_captures=0,
                num_translated_args=translated_count,
                translated_arg_offsets=translated_offsets,
                extra_align=extra_align,
                translated_args_ptr=translated.unsafe_ptr().unsafe_origin_cast[
                    MutAnyOrigin
                ](),
                dense_args_addrs=addresses.unsafe_ptr().unsafe_origin_cast[
                    MutUntrackedOrigin
                ](),
                grid_dim=grid_dim,
                block_dim=block_dim,
                shared_mem_bytes=shared_mem_bytes,
                attributes_ptr=attributes.unsafe_ptr().unsafe_origin_cast[
                    MutAnyOrigin
                ](),
                num_attributes=0,
                location=call_location(),
            )
            return

        var encoder = DefaultDeviceTypeEncoder()
        var translated_index = 0
        comptime for i in range(num_args):
            var translated_offset = translated_offsets[i]
            if translated_offset >= 0:
                var address = Pointer(
                    to=translated.unsafe_ptr()[
                        unsafe_offset=translated_offset + extra_align
                    ]
                ).unsafe_bitcast[NoneType]()
                args[i]._to_device_type(encoder, address)
                addresses[translated_index] = address.as_unsafe_any_origin()
                translated_index += 1

        _checked_call[Self.func](
            self._context.enqueue(
                self._function._handle,
                grid_dim,
                block_dim,
                shared_mem_bytes,
                attributes.unsafe_ptr().unsafe_origin_cast[MutAnyOrigin](),
                0,
                addresses.unsafe_ptr().as_unsafe_any_origin(),
                UInt32(translated_count),
                Optional[Pointer[UInt64, MutUntrackedOrigin]](),
            ),
            device_context=self._context,
            location=call_location(),
        )


def _callback_string[name: StaticString]() -> String:
    var size: c_size_t = 0
    var data = external_call[name, Pointer[Byte, ImmutAnyOrigin]](
        Pointer(to=size)
    )
    return String(unsafe_from_utf8=Span(unsafe_ptr=data, length=Int(size)))


def compile[
    func_type: TrivialRegisterPassable,
    declared_arg_types: TypeList[Trait=AnyType, ...],
    //,
    func: func_type,
    _signature: def(* args: * declared_arg_types) thin -> None = func,
](context: DeviceContext) raises -> CompiledKernel[func, declared_arg_types]:
    """Compiles a top-level, noncapturing function for the session's GPU."""
    # Keep `func` uncoerced so function reflection retains its source name;
    # this defaulted parameter separately infers and checks its signature.
    comptime name = reflect_fn[func].display_name()
    var status = external_call["xmojo_compile_gpu", c_int](
        name.as_bytes().unsafe_ptr(),
        c_size_t(name.byte_length()),
    )
    if status:
        raise Error(_callback_string["xmojo_gpu_error"]())
    return CompiledKernel[func, declared_arg_types](
        context,
        function_name=_callback_string["xmojo_gpu_function_name"](),
        object=_callback_string["xmojo_gpu_object_data"](),
    )
