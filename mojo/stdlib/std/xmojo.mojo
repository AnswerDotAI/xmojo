# ===----------------------------------------------------------------------=== #
# Copyright (c) 2026, xmojo contributors.
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# ===----------------------------------------------------------------------=== #
"""Rich display APIs for the xmojo interactive environment."""

from std.collections import List
from std.ffi import c_int, c_size_t, external_call
from std.memory import MutOpaquePointer, OwnedPointer, Pointer


trait HTMLRepr:
    """A value with an HTML representation."""

    def _repr_html_(self) -> String:
        ...


trait MarkdownRepr:
    """A value with a Markdown representation."""

    def _repr_markdown_(self) -> String:
        ...


trait SVGRepr:
    """A value with an SVG representation."""

    def _repr_svg_(self) -> String:
        ...


trait LaTeXRepr:
    """A value with a LaTeX representation."""

    def _repr_latex_(self) -> String:
        ...


@fieldwise_init
struct MIMEData(Copyable, Movable):
    """One textual value in a MIME bundle."""

    var mime_type: String
    var data: String


struct MIMEBundle(Movable):
    """A collection of textual representations of one value."""

    var data: List[MIMEData]

    def __init__(out self):
        self.data = List[MIMEData]()

    def add(mut self, var mime_type: String, var data: String):
        """Add a representation to the bundle."""
        self.data.append(MIMEData(mime_type^, data^))


trait MIMEBundleRepr:
    """A value that supplies its complete MIME bundle."""

    def _repr_mimebundle_(self) -> MIMEBundle:
        ...


def _begin(kind: Int):
    external_call["xmojo_display_begin", NoneType](c_int(kind))


def _add(mime_type: StringLiteral, data: String):
    external_call["xmojo_display_add", NoneType](
        mime_type.ptr(),
        c_size_t(mime_type.byte_length()),
        data.as_bytes().unsafe_ptr(),
        c_size_t(data.byte_length()),
    )


def _add(mime_type: String, data: String):
    external_call["xmojo_display_add", NoneType](
        mime_type.as_bytes().unsafe_ptr(),
        c_size_t(mime_type.byte_length()),
        data.as_bytes().unsafe_ptr(),
        c_size_t(data.byte_length()),
    )


def _end():
    external_call["xmojo_display_end", NoneType]()


def _render[T: AnyType](value: T, kind: Int):
    _begin(kind)
    comptime if conforms_to(T, MIMEBundleRepr):
        var bundle = value._repr_mimebundle_()
        for item in bundle.data:
            _add(item.mime_type, item.data)
    else:
        comptime if conforms_to(T, HTMLRepr):
            _add("text/html", value._repr_html_())
        comptime if conforms_to(T, MarkdownRepr):
            _add("text/markdown", value._repr_markdown_())
        comptime if conforms_to(T, SVGRepr):
            _add("image/svg+xml", value._repr_svg_())
        comptime if conforms_to(T, LaTeXRepr):
            _add("text/latex", value._repr_latex_())
        comptime if conforms_to(T, Writable):
            _add("text/plain", repr(value))
        else:
            _add("text/plain", String("<", reflect[T].name(), ">"))
    _end()


def display[T: AnyType](value: T):
    """Publish `value` as an explicit rich display."""
    _render(value, 0)


def __xmojo_display[T: AnyType](value: T):
    """Publish an automatically captured final expression."""
    _render(value, 1)


def __xmojo_display(value: None):
    """Suppress the result of an expression returning `None`."""
    pass


def __xmojo_error(error: Error):
    """Route an uncaught cell error to its interactive session."""
    var message = String(error)
    var stack_trace = error.get_stack_trace()
    if stack_trace:
        var trace = String(stack_trace.value())
        external_call["xmojo_emit_error", NoneType](
            message.as_bytes().unsafe_ptr(),
            c_size_t(message.byte_length()),
            trace.as_bytes().unsafe_ptr(),
            c_size_t(trace.byte_length()),
        )
    else:
        external_call["xmojo_emit_error", NoneType](
            message.as_bytes().unsafe_ptr(),
            c_size_t(message.byte_length()),
            "".ptr(),
            c_size_t(0),
        )


comptime __xmojo_slot_array = Pointer[
    MutOpaquePointer[MutUntrackedOrigin], MutUntrackedOrigin
]
"""One session-owned pointer per persistent interactive variable."""

comptime _xmojo_destroy_fn = def(MutOpaquePointer[MutUntrackedOrigin]) thin abi(
    "C"
) -> None


def _destroy_persistent[
    T: Movable & Deinitable
](storage: MutOpaquePointer[MutUntrackedOrigin],) abi("C"):
    _ = OwnedPointer[T](unsafe_from_opaque_pointer=storage)


def __xmojo_persist[
    T: Movable & Deinitable
](slots: __xmojo_slot_array, index: Int, var value: T):
    """Move a completed cell's top-level variable into session storage."""
    var owned = OwnedPointer(value^)
    var storage = owned^.unsafe_take_allocation().unsafe_leak()
    slots[unsafe_offset=index] = storage.unsafe_bitcast[NoneType]()
    var destroy: _xmojo_destroy_fn = _destroy_persistent[T]
    external_call["xmojo_register_persistent", NoneType](
        c_size_t(index), destroy
    )
