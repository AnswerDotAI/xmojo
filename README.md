# xmojo

`xmojo` is an interactive Mojo environment built on Modular's ORC execution
engine. One command provides a terminal REPL, native compilation, package
precompilation, and a Jupyter kernel with persistent state and rich display.

## Install

xmojo currently supports Apple Silicon on macOS 12 or newer:

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

### REPL

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

### Build executables

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

## Modular GPU

GPU sessions use the exactly matched official Mojo compiler and MAX runtime.
Install the optional dependency from Modular's nightly package index:

```bash
uv pip install --prerelease=allow \
  --extra-index-url https://whl.modular.com/nightly/simple/ \
  'xmojo[modular-gpu]'
```

On Apple Silicon, install Xcode's optional Metal toolchain once:

```bash
xcodebuild -downloadComponent MetalToolchain
```

Start a GPU-enabled REPL with:

```bash
xmojo --modular-gpu
```

The launcher detects the local accelerator and supplies the matched compiler,
MAX imports, and AsyncRT runtime automatically. Compile and launch an ordinary
top-level Mojo function:

```mojo
from std.gpu import global_idx
from max.gpu.host import DeviceContext
from xmojo.gpu import compile

def increment(output: Pointer[Float32, MutAnyOrigin], size: Int32):
    var i = global_idx.x
    if i < Int(size):
        output[unsafe_offset=i] += 1

with DeviceContext() as context:
    var buffer = context.enqueue_create_buffer[DType.float32](256)
    var kernel = compile[increment](context)
    kernel.enqueue(buffer, Int32(256), grid_dim=1, block_dim=256)
    context.synchronize()
```

`compile[...]` caches device objects under `~/Library/Caches/xmojo/gpu`.
Set `XMOJO_GPU_CACHE_DIR` to choose another directory. Kernels must currently
be top-level, nonparameterized, noncapturing functions with an ordinary name.
Launch argument count and device types are checked while compiling the cell.

The installed Jupyter kernelspec is CPU-only. To add a separate GPU kernel:

```bash
gpu_kernel="$(jupyter --data-dir)/kernels/xmojo-gpu"
mkdir -p "$gpu_kernel"
cat > "$gpu_kernel/kernel.json" <<'JSON'
{
  "argv": ["xmojo", "--modular-gpu", "kernel", "-f", "{connection_file}"],
  "display_name": "Mojo (xmojo, Modular GPU)",
  "language": "mojo"
}
JSON
```

Select **Mojo (xmojo, Modular GPU)** in Jupyter.

## Current limitations

- Published wheels currently support Apple Silicon only.
- Interrupting a running cell is not yet supported.
- Rich display currently supports textual MIME data, without binary buffers,
  display metadata, or transient display IDs.
- Persistent variables cannot be redeclared or implicitly captured by
  functions defined in other cells.
- GPU support is opt-in and currently targets top-level noncapturing kernels.

See [DEV.md](DEV.md) for architecture, source builds, dependency revisions,
testing, and contributor workflow.

## License

Apache License v2.0 with LLVM Exceptions. See [LICENSE](LICENSE).
