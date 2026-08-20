# xmojo

`xmojo` is an experimental interactive Mojo environment built on Modular's
ORC execution engine. The compiler integration, execution session, tests, and
frontends all live in this repository. An unmodified sibling Modular checkout
provides the compiler and runtime implementation.

The first frontend is `mojoorc`, a terminal REPL:

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

## Development layout

The build intentionally uses editable sibling git worktrees:

```text
git/
├── modular/
├── xmojo/
├── xeus/
├── xeus-zmq/
├── libzmq/
└── cppzmq/
```

No packaged Mojo compiler or locally patched Modular checkout is used. `xmojo`
intentionally depends on private Modular C++ targets and tracks their changes
at exact tested revisions.

```bash
./bazelw build @xmojo//:mojoorc
./bazelw test @xmojo//:session_test
```

## Current scope

The current compiler PoC:

- parses executable statements and top-level declarations in one cell;
- resolves later cells against explicitly committed declaration history;
- commits no history from a cell that fails parsing or compilation;
- maps diagnostics from generated wrappers back to the submitted source;
- executes uniquely named cell entry points in one ORC JITDylib;
- streams CPU `print()` output through per-session stdout/stderr callbacks;
- uses a 128 KiB formatting buffer for each active CPU print call; and
- keeps simultaneous sessions isolated.

It does not use Modular's LLDB-oriented REPL parser entry point, REPL context,
or persistent-variable materializer. Persistent local values, typed expression
results, direct file-descriptor writes, interruption, GPU execution, and the
xeus frontend are deliberately outside this PoC.

## License

Apache License v2.0 with LLVM Exceptions. See `LICENSE`.
