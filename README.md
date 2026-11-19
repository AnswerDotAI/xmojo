# xmojo

`xmojo` is an interactive Mojo environment built on Modular's ORC execution
engine. One command provides a terminal REPL, native compilation, package
precompilation, and a Jupyter kernel with persistent state and rich display.

xmojo is created by Jeremy Howard from Answer.AI. It is not a Modular project.

## Install

xmojo supports Apple Silicon on macOS 12 or newer, and ARM64 or x86-64 Linux
systems with glibc 2.35 or newer:

```bash
uv pip install xmojo
```

The wheel is self-contained. It includes the Mojo compiler, runtime, and
standard library used by xmojo, so it does not require a Modular checkout,
Bazel, or a separate Mojo installation. It also installs a Jupyter kernelspec.

Each xmojo release targets one exact Mojo nightly. Its numeric release component
ends with that nightly's timestamp; a `.postN` suffix denotes packaging or
documentation updates for the same build.

## Commands

```bash
xmojo                                      # interactive ORC REPL
xmojo -e 'print("hello")'                  # evaluate one cell
xmojo example.mojo                         # execute a Mojo file
xmojo build hello.mojo -o hello            # build a native executable
xmojo precompile mypackage -o mypackage.mojoc  # precompile a package
xmojo kernel -f connection.json            # run as a Jupyter kernel
xmojo --version
```

## Jupyter notebooks

Install xmojo in the environment used by Jupyter, start your preferred Jupyter
frontend, and select **Mojo (xmojo)**:

```bash
uv pip install xmojo jupyterlab
jupyter lab
```

The kernel provides:

- persistent functions, types, and typed top-level variables;
- exact streaming of `stdout` and `stderr`;
- automatic display of a cell's final expression;
- explicit rich display with textual MIME bundles;
- compiler-backed completion, inspection, and input completeness;
- source-mapped compiler diagnostics and Mojo runtime stack traces; and
- continued use of the session after ordinary compilation or runtime errors.

Compilation failures execute nothing and add no names. Mutations completed
before a runtime error remain visible, while new variables from the raising
cell are not persisted.

### Rich display

`display()` publishes explicit output. Mojo repr traits provide
compiler-checked HTML, Markdown, SVG, LaTeX, or complete MIME-bundle
representations:

```mojo
from xmojo import HTMLRepr, display

@fieldwise_init
struct HTML(HTMLRepr):
    var source: String

    def _repr_html_(self) -> String:
        return self.source

display(HTML("<b>explicit display</b>"))
HTML("<b>automatic final-expression result</b>")
```

The other traits are `MarkdownRepr`, `SVGRepr`, `LaTeXRepr`, and
`MIMEBundleRepr`. A `Writable` value also receives a `text/plain`
representation; other values receive a type-name fallback.

### Limitations

- Interrupting a running cell is not yet supported.
- Rich display currently supports textual MIME data, without binary buffers, display metadata, or transient display IDs.
- Persistent variables cannot be redeclared or implicitly captured by functions defined in other cells.
- GPU support is opt-in and currently targets top-level noncapturing kernels.

## GPU support

The REPL and Jupyter kernel use Modular's native GPU compiler and runtime.
Use the normal `max.gpu` APIs in either frontend; no GPU-specific launch flag,
separate Mojo compiler, or `max-core` Python installation is needed. The session
detects the local accelerator. `--target-accelerator` overrides that choice.

On Apple Silicon, install Xcode's optional Metal toolchain once:

```bash
xcodebuild -downloadComponent MetalToolchain
```

For example, run this in a notebook cell or paste it into the REPL:

```mojo
from max.gpu import global_idx
from max.gpu.host import DeviceContext

def increment[amount: Int](output: Pointer[Float32, MutAnyOrigin], size: Int32):
    var i = global_idx.x
    if i < Int(size):
        output[unsafe_offset=i] += Float32(amount)

with DeviceContext() as context:
    var buffer = context.enqueue_create_buffer[DType.float32](16)
    buffer.enqueue_fill(0)
    context.enqueue_function[increment[2]](
        buffer, Int32(16), grid_dim=1, block_dim=16
    )
    context.synchronize()
    with buffer.map_to_host() as values:
        print(values[0])  # 2.0
```

Imports, kernel definitions, and owned device contexts and buffers can persist
across cells. GPU compilation and launch use the same in-process pipeline as
ordinary Mojo code. Hardware support and platform prerequisites follow the
pinned Modular revision. See [the GPU integration notes](docs/modular-gpu-pipeline.md)
for the compiler boundary and GPU tests.

## REPL

Run `xmojo` without arguments. A blank line submits each cell:

```console
$ xmojo
Mojo ORC REPL
Expressions are delimited by a blank line. :quit exits.

1> def answer() -> Int:
..   return 42
..
2> print(answer())
..
42
```

Declarations and variables persist between cells:

```mojo
var total = 40
```

```mojo
total += 2
total
```

The final expression produces `Int(42)` in Jupyter. Persistent variables keep
their original type and cannot be redeclared. They must own their data or refer
only to static storage; copy borrowed views into an owned value before
persisting them.

## Build executables

`xmojo build` uses Modular's native Mojo build driver with an ordinary stdlib,
so compiled programs retain normal standalone I/O behavior:

`hello.mojo`:

```mojo
def main():
    print("Hello from Mojo")
```

```bash
xmojo build hello.mojo -o hello
./hello
```

```text
Hello from Mojo
```

### Precompile packages

Precompile a directory containing `__init__.mojo` and other Mojo modules:

```bash
xmojo precompile mypackage -o mypackage.mojoc
```

The result can be supplied to Mojo through its normal import paths.

See [DEV.md](DEV.md) for architecture, source builds, dependency revisions,
testing, and contributor workflow.

## License

Apache License v2.0 with LLVM Exceptions. See [LICENSE](LICENSE).
Bundled Modular components retain their upstream licenses and third-party
notices, included under `xmojo/_native/licenses` in the installed package.
