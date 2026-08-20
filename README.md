# xmojo

`xmojo` is an experimental interactive Mojo environment built on Modular's ORC execution engine.

`xmojo` is the xeus-based Jupyter kernel executable. Jupyter launches it with a connection file; it keeps one `InteractiveSession` alive for the lifetime of the kernel and publishes compiler diagnostics and exact stdout/stderr streams.

The terminal frontend is `mojoorc`:

```console
$ mojoorc
Mojo ORC REPL
Expressions are delimited by a blank line. :quit exits.

1> def answer() -> Int:
..   return 42
..
2> print(answer())
..
42
```

It also accepts a single expression or a file:

```bash
mojoorc -e 'print("hello")'
mojoorc example.mojo
```

Build and install both commands locally with:

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

No packaged Mojo compiler or locally patched Modular checkout is used. `xmojo`
intentionally depends on private Modular C++ targets and tracks their changes
at exact tested revisions.

```bash
./bazelw build @xmojo//:mojoorc @xmojo//:xmojo
./bazelw test @xmojo//:session_test @xmojo//:interpreter_test @xmojo//:kernel_test
```

The kernel test uses `conkernelclient>=0.0.20` from the active Python
environment and launches `xmojo` directly, without installing a kernelspec.
The other dependencies are built directly from the sibling worktrees.

## Current scope

The current compiler and kernel PoC:

- parses executable statements and top-level declarations in one cell;
- resolves later cells against explicitly committed declaration history;
- keeps a cell's top-level `var`s alive for later cells, which read and assign
  them in place;
- commits no history from a cell that fails parsing or compilation;
- maps diagnostics from generated wrappers back to the submitted source;
- executes uniquely named cell entry points in one ORC JITDylib;
- streams CPU `print()` output through per-session stdout/stderr callbacks;
- uses a 128 KiB formatting buffer for each active CPU print call;
- keeps simultaneous sessions isolated;
- displays the final value expression as a Jupyter `execute_result`;
- publishes explicit textual MIME bundles with `display()` and Mojo repr
  traits;
- completes and inspects names using Mojo's compiler APIs;
- classifies complete, incomplete, and lexically invalid input;
- serves signed Jupyter messages through xeus-zmq; and
- maps successful execution and Mojo failures to matching shell and IOPub
  replies.

It does not use Modular's LLDB-oriented REPL parser entry point, REPL context,
or persistent-variable materializer. Typed expression history, binary
rich-display buffers, display metadata, direct file-descriptor writes,
interruption, and GPU execution are deliberately outside this PoC.

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
