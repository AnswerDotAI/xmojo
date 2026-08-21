# xmojo

`xmojo` is an experimental interactive Mojo environment built on Modular's ORC execution engine.

## Install

The current wheel is an Apple Silicon nightly built against the exact matching
Mojo nightly. Build and install it from the selected Modular worktree with:

```bash
export XMOJO_MODULAR_ROOT=/path/to/modular-nightly
XMOJO_BAZEL_WRAPPER=./bazelw2 ./tools/build_wheel.sh
uv pip install --prerelease=allow \
  "$XMOJO_MODULAR_ROOT"/bazel-bin/external/+local_repository+xmojo/xmojo-*.whl
```

This installs the `xmojo` command and a Jupyter kernelspec. The command also
works directly without kernelspec
discovery. The installed wheel is self-contained: it does not use the Modular
checkout, Bazel, or a separately configured Mojo installation. The exact
Mojo compiler, runtime, and stdlib are built into xmojo's wheel.

The planned published form is the same installation reduced to
`uv pip install xmojo`; the `modular-gpu` extra adds the exactly matched
`max-core` package for official Modular compilation and runtime support. It is
not required for the WebGPU path:

```bash
uv pip install --prerelease=allow \
  --extra-index-url https://whl.modular.com/nightly/simple/ \
  'xmojo[modular-gpu]'
```

With no subcommand, `xmojo` is an ORC-based terminal REPL:

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

The same command evaluates an expression or file, invokes Modular's Mojo build
and precompile drivers, or runs the xeus-based Jupyter kernel:

```bash
xmojo -e 'print("hello")'
xmojo example.mojo
xmojo build example.mojo
xmojo precompile mypackage -o mypackage.mojoc
xmojo kernel -f connection.json
```

## Using Modular GPU

After installing the `modular-gpu` extra, start a GPU-enabled terminal session
with:

```bash
xmojo --modular-gpu
```

The launcher asks `max-core` to detect the local accelerator and supplies the
matching official compiler, MAX imports, and runtime automatically. The current
Apple Silicon build also requires Xcode's Metal toolchain, installed once with
`xcodebuild -downloadComponent MetalToolchain`.

The default Jupyter kernelspec remains CPU-only. Install a separate GPU
kernelspec with:

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

Select **Mojo (xmojo, Modular GPU)** in Jupyter, then compile and launch an
ordinary top-level Mojo function:

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

On a cache miss, `compile[...]` runs the official compiler synchronously and
stores the resulting device object in the platform cache directory
(`~/Library/Caches/xmojo/gpu` on macOS). Set `XMOJO_GPU_CACHE_DIR` to override
it. Kernels must be top-level, nonparameterized, noncapturing functions with an
ordinary name; launch argument count and device types are checked during cell
compilation.

Build and install the command locally with:

```bash
./tools/install_cli.sh
```

This installs Bazel-generated, runfiles-aware launchers in `~/.local/bin`.
Pass another directory as the first argument to install elsewhere. Rerun the
script after `bazel clean` or changing Bazel's output base.

## Development layout

The build intentionally uses editable sibling git worktrees:

```text
git/
├── modular/
├── xmojo/
├── nlohmann-json/
├── xeus/
├── xeus-zmq/
├── libzmq/
└── cppzmq/
```

The normal xmojo build uses no packaged Mojo compiler or locally patched
Modular checkout. `xmojo` intentionally depends on private Modular C++ targets
and tracks their changes at exact tested revisions. The Metal interop test uses
a separately pinned official compiler because Modular's open-source compiler
does not contain the accelerator backend.

```bash
./bazelw build @xmojo//:xmojo
./bazelw test @xmojo//:session_test @xmojo//:interpreter_test @xmojo//:cli_test @xmojo//:kernel_test
./bazelw test -c opt @xmojo//:wheel_test
./bazelw2 test @xmojo//:gpu_shared_library_test
```

The GPU test requires Pixi and Xcode's optional Metal toolchain. Install the
latter once with `xcodebuild -downloadComponent MetalToolchain`; `bazelw2`
installs xmojo's locked official Mojo/MAX environment automatically.

The kernel test uses `conkernelclient>=0.0.20` from the active Python
environment and launches `xmojo` directly, without installing a kernelspec.
The wheel test installs into a clean environment and reruns both the CLI and
kernel stories through the installed command. The other dependencies are built
directly from the sibling worktrees.

## Current scope

The current compiler and kernel PoC:

- parses executable statements and top-level declarations in one cell;
- resolves later cells against explicitly committed declaration history;
- commits no history from a cell that fails parsing or compilation;
- retains compiled declarations from a cell whose executable statements raise;
- keeps successful top-level `var` declarations in typed, session-owned
  storage so later cells can read and mutate them;
- reports uncaught Mojo errors, including an available stack trace, as failed
  execution rather than printed output;
- maps diagnostics from generated wrappers back to the submitted source;
- executes uniquely named cell entry points in one ORC JITDylib;
- streams CPU `print()` output through per-session stdout/stderr callbacks;
- uses a 128 KiB formatting buffer for each active CPU print call;
- keeps simultaneous sessions isolated;
- isolates each session's compiler object cache;
- displays the final value expression as a Jupyter `execute_result`;
- publishes explicit textual MIME bundles with `display()` and Mojo repr
  traits;
- completes and inspects names using Mojo's compiler APIs;
- classifies complete, incomplete, and lexically invalid input;
- compiles top-level noncapturing functions with the official Mojo accelerator
  backend and launches them through a checked MAX device handle;
- serves signed Jupyter messages through xeus-zmq; and
- maps successful execution and Mojo failures to matching shell and IOPub
  replies.

It does not use Modular's LLDB-oriented REPL parser entry point, REPL context,
or persistent-variable materializer. A persistent variable keeps its original
type, cannot be redeclared, and is visible only to later cell statements—not
implicitly inside function bodies. Mutations completed before a runtime error
remain; new variables from the raising cell do not. Persistent values must own
their data or refer only to static storage; borrowed views must first be copied
into an owned value. Typed expression history,
binary rich-display buffers, display metadata, direct file-descriptor writes,
and interruption remain outside this PoC. Modular GPU support requires the
`modular-gpu` extra and an explicitly configured GPU session, as described
above.

Notebook code can opt into rich display without Python-style runtime
reflection:

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

`MarkdownRepr`, `SVGRepr`, `LaTeXRepr`, and `MIMEBundleRepr` provide the other
supported representations. A `Writable` value also gets a `text/plain`
representation; other values receive a type-name fallback.

## License

Apache License v2.0 with LLVM Exceptions. See `LICENSE`.
