# xmojo

`xmojo` is an experimental interactive Mojo environment built on Modular's
ORC execution engine. The compiler integration, execution session, tests, and
frontends all live in this repository. An unmodified sibling Modular checkout
provides the compiler and runtime implementation.

The terminal frontend is `mojoorc`:

```console
$ ./bazelw run @xmojo//:mojoorc
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
./bazelw run @xmojo//:mojoorc -- -e 'print("hello")'
./bazelw run @xmojo//:mojoorc -- example.mojo
```

`xmojo` is the xeus-based Jupyter kernel executable. Jupyter launches it with
a connection file; it keeps one `InteractiveSession` alive for the lifetime of
the kernel and publishes compiler diagnostics and exact stdout/stderr streams.

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

The kernel test uses the current `conkernelclient` checkout from the active
Python environment, including its direct-argv `run_kernel` support. The other
dependencies are built directly from the sibling worktrees.

## Current scope

The current compiler and kernel PoC:

- parses executable statements and top-level declarations in one cell;
- resolves later cells against explicitly committed declaration history;
- commits no history from a cell that fails parsing or compilation;
- maps diagnostics from generated wrappers back to the submitted source;
- executes uniquely named cell entry points in one ORC JITDylib;
- streams CPU `print()` output through per-session stdout/stderr callbacks;
- uses a 128 KiB formatting buffer for each active CPU print call;
- keeps simultaneous sessions isolated;
- serves signed Jupyter messages through xeus-zmq; and
- maps successful execution and Mojo failures to matching shell and IOPub
  replies.

It does not use Modular's LLDB-oriented REPL parser entry point, REPL context,
or persistent-variable materializer. Persistent local values, typed expression
results, rich display, completion, inspection, direct file-descriptor writes,
interruption, and GPU execution are deliberately outside this PoC.

## License

Apache License v2.0 with LLVM Exceptions. See `LICENSE`.
