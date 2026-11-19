# Development notes

## Architecture

All xmojo-authored code lives in this repository. The sibling Modular checkout
is an unmodified, revision-pinned source dependency.

```text
InteractiveSession
├── InteractiveParser
│   ├── cell source partitioning and location mapping
│   ├── final-expression classification and display rewriting
│   ├── chained declaration resolution
│   ├── typed persistent-variable binding
│   ├── explicit transactional history
│   └── completion, inspection, and completeness tooling
├── ORC object compilation and execution
├── xmojo Mojo package and scoped output/display callbacks
└── unified xmojo REPL, compiler-driver, and xeus-zmq kernel frontend
```

GPU compilation and execution use Modular's native compiler targets and MAX
runtime. xmojo owns the interactive session and frontend integration; Modular
owns device discovery, specialization, code generation, allocation, and launch.
The public session boundary is direct C++. Generated code uses the native
CompilerRT and MAX runtime entry points.

Modular remains Bazel's main repository because its dependency overrides are
root-module configuration. The local `bazelw` injects this worktree as the
`@xmojo` repository, so both repositories remain independently editable without
duplicating Modular's build configuration. It passes
`--check_visibility=false` because consuming unstable private compiler targets
is intentional. The dependency list remains explicit in `BUILD.bazel`.

## Development layout

Development uses editable sibling worktrees for Modular, nlohmann-json, xeus,
xeus-zmq, libzmq, and cppzmq. `XMOJO_DEPS_ROOT` selects their parent directory
and defaults to xmojo's parent. `XMOJO_MODULAR_ROOT` can select a separate Modular
checkout. No dependency needs an xmojo patch.

Install `uv`, CMake, and Ninja. macOS development needs Xcode and GPU execution
needs its Metal toolchain. Ubuntu/Debian development needs `cmake`, `ninja-build`,
and `pkg-config`. Native builds target Apple Silicon macOS and Linux on ARM64
or x86-64; GPU execution additionally requires hardware supported by Modular.

Create missing pinned dependencies and set up the editable project with:

```bash
tools/setup_source_deps.sh
uv sync
uv run xmojo --version
```

The setup helper leaves existing worktrees untouched. The editable build stages
native assets under the ignored `python/xmojo/_native` directory. Python edits
remain visible; rerun `uv sync` after native, Mojo, or build configuration changes.

Run the core stories and, on a GPU host, the hardware stories:

```bash
./bazelw test @xmojo//:session_test @xmojo//:interpreter_test \
  @xmojo//:cli_test @xmojo//:kernel_test --test_output=errors
./bazelw test @xmojo//:gpu_test @xmojo//:gpu_kernel_test --test_output=errors
```

Python frontend tests use `python` on `PATH` and need `conkernelclient`. The GPU
targets are tagged `manual` and `requires-gpu`; run them explicitly rather than
counting their exclusion from a wildcard run as GPU verification.

Codex must run `./bazelw` outside its sandbox; the wrapper fails immediately
when `CODEX_SANDBOX` is set. Configure
`prefix_rule(pattern=["./bazelw"], decision="allow")`, then use
`sandbox_permissions="require_escalated"`. Invoke the wrapper directly rather
than through `bash`. This prevents a sandboxed Bazel server from being reused
by later commands.

CMake materializes Modular's BoringSSL headers and stages the xeus libraries
when Bazel first needs them. On Linux it uses Modular's pinned Clang and Ubuntu
22.04 sysroot to match the final Bazel link. On macOS it uses the SDK belonging
to the selected Xcode. xmojo's UUID implementation uses BoringSSL randomness,
so the host distribution's `libuuid` cannot leak across that boundary.

## Interactive compiler PoC

Cell compilation uses xmojo's own module construction and persistent-value ABI.
Completion and inspection use Modular's `MojoREPLContext` as a tooling helper;
it does not execute cells or materialize their variables. The parser borrows
three language-independent ideas from Modular's REPL, with source attribution:

- partition cell source into module declarations and executable statements;
- map generated-wrapper diagnostics back to the submitted cell; and
- import declarations from the last committed interactive module.

Parsing produces an uncommitted `InteractiveParser::Cell`. The session clones,
lowers, slices, object-compiles, and adds that cell before committing its module
as visible history and executing its entry point. Parse and compilation
failures therefore cannot contribute names to later cells. An uncaught runtime
`Error` does not roll back declarations already resident in ORC, mutations, or
other side effects. Sessions share Modular's process-wide runtime context,
while each owns its parser, MLIR context, ORC engine, and committed history.

Successful top-level `var` declarations are moved at the cell epilogue into
individually allocated `OwnedPointer` storage. The parser retains each resolved
MLIR type and gives later cell wrappers mutable references through an opaque
slot array; declarations emitted outside those wrappers cannot implicitly
capture the references. Existing names cannot be redeclared or change type.
Each specialization registers a C-ABI destructor with the session, which
destroys values in reverse declaration order before ORC is torn down.
Resolved persistent types may contain only static origins. The parser rejects
local and untracked borrowed values before compilation because the opaque slot
ABI cannot preserve or validate their source lifetimes; users must persist an
owned copy instead.

Variable effects are sequential rather than transactional. Existing mutations
completed before an uncaught `Error` remain visible, while variables declared
by that raising cell stay ordinary locals and unwind because the persistence
epilogue was not reached. Compilation failures execute nothing and therefore
change no variable state.

The cell wrapper asks Mojo's parser whether the final top-level simple
statement is a value expression. If so, source-map-aware rewriting wraps that
expression in `__xmojo_display(...)`; assignments, declarations, and control
flow are left alone. This preserves Mojo's syntax and type checking rather than
duplicating either in xmojo. The helper has a `None` overload, so a call such as
`display(value)` emits its explicit display without producing a second result.

Each session owns one ORC JITDylib. A cell adds a new archive with a uniquely
named no-argument entry point. Reachable definitions are emitted with external
linkage so later cells can use specializations generated by earlier cells.
Splitting cells into separate JITDylibs does not work: Mojo normally gives those
helper symbols private linkage because a one-shot compilation has no later
consumer.

Each session also owns a temporary object-cache directory. Modular's
`ObjectCompiler` cache hits bypass the per-compiler function bookkeeping used
to exclude definitions already resident in ORC, so sharing a cache between
sessions can produce duplicate symbols and wrong results. The directory is an
RAII `TempDir`, destroyed after the execution engine and object compiler.

The frontend targets use Modular's `mojo_test_environment` rule directly. An
xmojo-owned source overlay replaces the default stdlib plugin while retaining
Modular's accelerator plugins, then precompiles the resulting `std`. The
environment computes its runfiles import path and stages the runtime libraries
without requiring an installed Mojo toolchain.

## Python packaging and wheel layout

`pyproject.toml` is authoritative for Python package metadata, dependencies,
the console command, and the Jupyter data installation. Setuptools provides the
PEP 517 and PEP 660 implementation. The small `build_native` command in
`setup.py` is a regular setuptools build subcommand: editable builds stage a
development asset set beside the Python sources, while `uv build --wheel`
writes an optimized asset set into the wheel build directory. Both paths use
Bazel's `wheel_assets` target and the same Python launcher.

xmojo publishes platform wheels only; it does not publish a source
distribution. The wheel tags are `macosx_12_0_arm64`,
`manylinux_2_35_aarch64`, and `manylinux_2_35_x86_64`.

Each platform wheel packages the native frontend, its shared-library dependencies, the ordinary and interactive-overlay `std` packages, `max.mojoc`, and `xmojo.mojoc`. It includes Modular's license and third-party notices.

The Python launcher sets `MODULAR_MOJO_MAX_PACKAGE_ROOT` to the installed `_native` directory and replaces itself with the native frontend. Modular resolves CompilerRT and standalone linker flags from that package root. The frontend in `bin/` finds shared libraries in `lib/` through relative rpaths. The launcher does not change `LD_LIBRARY_PATH` or `DYLD_LIBRARY_PATH`.

REPL and kernel sessions use the stdlib overlay. `build` and `precompile` select the ordinary stdlib for standalone I/O.

The wheel has no separate Mojo or MAX Python package dependency. Hardware
runtime requirements, such as the Metal toolchain or GPU drivers, still apply.
Development assets retain Bazel's default configuration. Release wheels use `-c opt`.

Run the CLI and Jupyter stories against an installed wheel:

```bash
source bazel/versions.bzl
python tests/wheel_test.py path/to/wheel.whl tests/cli_test.py tests/kernel_test.py "$XMOJO_VERSION"
```

`bazel/versions.bzl` is authoritative for the source revisions. Updating Modular
requires rebuilding all compiler, runtime, stdlib, and MAX artifacts together.
`tools/build_wheel.sh` rejects wrong revisions or dirty dependency worktrees.
`XMOJO_ALLOW_DIRTY=1` skips only the xmojo cleanliness check for local packaging
development and must not be used for a published build.

GitHub Actions builds all three platform wheels. On Linux, `auditwheel show` reports manylinux compatibility. Release tags publish through the existing trusted-publishing workflow. Do not bump the package version during development.

When `BBUDDY_API_KEY` is set, `bazelw` selects xmojo's BuildBuddy configuration.
Otherwise it uses Modular's read-only public action cache with build-event
uploads and remote cache writes disabled.

On macOS, the wrapper passes `xcode-select`'s developer directory to Bazel's
target and exec action environments. It also passes the developer directory and
Xcode build number as repository inputs, so Bazel refreshes its generated Xcode
configuration when either changes and equivalent machines share configuration
and remote-cache keys. Bazel's repository rule discovers installed Xcodes
through LaunchServices, which Codex's macOS Seatbelt sandbox does not expose;
this is why sandboxed Codex invocations are rejected rather than worked around.

## Native GPU compilation

`InteractiveSession` uses Modular's normal offload pipeline and MAX runtime.
Ordinary `max.gpu.host.DeviceContext` calls compile and launch kernels in-process;
there is no second compiler, declaration-source replay, custom GPU ABI, or
xmojo-specific accelerator backend. The default target accelerator comes from
Modular's device discovery, with an optional explicit session/CLI override.

The standard library overlay replaces `_plugin/_impl.mojo` and selects
`XMojoPlugin` only for host compilation. Accelerator compilation keeps the
native target's plugin. Shared compiler and runtime dependencies retain
Modular's registry ownership, and ORC resolves runtime symbols through the
process symbol generator.

GPU contexts and buffers follow the existing persistent-value rules, including
reverse destruction before the ORC engine is destroyed. Compilation failures
execute nothing. Device work remains asynchronous until the program explicitly
synchronizes it.

See the [GPU integration notes](docs/modular-gpu-pipeline.md) for platform requirements and what the hardware tests cover.

## Jupyter frontend

`MojoInterpreter` is a thin translation layer between xeus requests and
`InteractiveSession`; it owns no compiler state. The `xmojo` executable adds
xeus-zmq's sockets and Jupyter connection-file handling. Its end-to-end test
launches that executable directly by argv through `conkernelclient`, without
installing or discovering a kernelspec. `conkernelclient` provides correlated
shell replies, deterministic readiness, IOPub draining, and process cleanup.
The test checks persistent cells, tooling requests, automatic and explicit
display, stdout, stderr, and errors over the wire.

The xeus, xeus-zmq, libzmq, cppzmq, and nlohmann-json sibling worktrees remain
unchanged. CMake stages their static libraries under the active Bazel
output-user root, and the wrapper injects that install tree as the logical
`@xmojo_deps` repository. This gives concurrent wrappers independent CMake
build and install trees while retaining Bazel disk-cache reuse for identical
consumer actions. xeus-zmq normally uses OpenSSL algorithms which Modular's
BoringSSL does not provide. Rather than linking two crypto implementations, the
build substitutes an xmojo-owned authentication implementation supporting the
Jupyter defaults: `none` and `hmac-sha256`. Other schemes fail explicitly.

## Design constraints

- Keep the entire Modular checkout unmodified; use its native GPU targets.
- Track Modular private APIs directly; update xmojo rather than adding version
  compatibility code.
- Use ORC exclusively; do not route execution through LLDB or MCJIT.
- Do not adopt `MojoParserREPLListener`, `RefFromPointerREPLOp`, or LLDB's
  persistent-variable ABI.
- Add output routing through explicit runtime callbacks, never file-descriptor
  interception.
- Represent persistent values with session-owned storage based on Mojo data
  layout; do not expose LLDB's materializer as an API.
- Failed compilation must not mutate visible session state.
- Keep the session API smaller than its current implementations require; add
  interruption only with its real backend.
- Local sibling worktrees are authoritative during development. CI may check
  out recorded known-good commits for reproduction.

By default the wrapper finds Modular and the CMake dependencies under
`XMOJO_DEPS_ROOT`, which defaults to xmojo's parent directory.
`XMOJO_MODULAR_ROOT` overrides just the Modular checkout. `bazelw` and
`bazelw2` use separate output-user roots. Bazel then derives a distinct output
base for each selected Modular worktree, giving concurrent sessions and release
trees independent servers, analysis caches, and staged xeus dependencies. Both
inherit Modular's shared disk cache, so identical actions reuse compiled
outputs.
`XMOJO_BAZEL_OUTPUT_USER_ROOT` and `XMOJO_BAZELW2_OUTPUT_USER_ROOT` override the
two defaults.

The wrappers reject caller overrides of their action environments, injected
repositories, toolchain, visibility policy, and output-user root.
These infrastructure settings must remain identical across
sessions for reliable disk-cache reuse. Ordinary Bazel configurations remain
available: fastbuild/debug and `-c opt` use independent cached outputs, while
named configurations such as `--config=asan` may be selected explicitly.

## Dependency revisions

`bazel/versions.bzl` records the exact source revisions. Modular is pinned to
`bbc13a719b6c139ede763f1434a4342ebfd843ac`, corresponding to Mojo
`1.3.0.dev2026111705` and MAX `26.8.0.dev2026111705`. All compiler, stdlib,
MAX package, and runtime artifacts are built together from that checkout.

When updating Modular, replace the recorded revision and adopt the new APIs directly. Run the core tests, exercise the GPU backends you claim to support, and test the installed wheel before releasing.
