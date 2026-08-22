# Development notes

## Architecture

All xmojo-authored code lives in this repository. The sibling Modular checkout
is a revision-pinned source dependency. Its compiler sources remain unmodified;
the root module has one local configuration line enabling LLVM's SPIR-V target:

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

The experimental native accelerator runtime is a separate `libxmojo_gpu`
static library with an IREE-free C++ interface in `include/xmojo/gpu`. Its
Metal implementation links only IREE base, async, common HAL, FlatCC, and the
Metal driver from a sibling IREE checkout. It does not link the IREE compiler,
VM, LLVM, or MLIR and is not yet connected to the installed Mojo API.

The public boundary is direct C++. There is no subprocess protocol or public C
ABI. Generated code will still use ordinary CompilerRT ABI entry points when
runtime host services are added.

Modular remains Bazel's main repository because its dependency overrides are
root-module configuration. The local `bazelw` injects this worktree as the
`@xmojo` repository, so both repositories remain independently editable without
duplicating Modular's build configuration. It passes
`--check_visibility=false` because consuming unstable private compiler targets
is intentional. The dependency list remains explicit in `BUILD.bazel`.

## Development layout

Development uses editable sibling worktrees rather than vendored or patched
dependencies:

```text
git/
├── modular/
├── iree/
├── xmojo/
├── nlohmann-json/
├── xeus/
├── xeus-zmq/
├── libzmq/
└── cppzmq/
```

`XMOJO_DEPS_ROOT` selects the directory containing these worktrees and defaults
to xmojo's parent directory. `XMOJO_MODULAR_ROOT` can select a Modular checkout
outside that directory. Wheel dependency revisions are recorded in
`bazel/versions.bzl`. The IREE revision used by the experimental runtime is
recorded in `docs/modular-gpu-pipeline.md` until that library becomes a packaged
xmojo dependency.

### New checkout

Install `uv`, CMake, and Ninja. macOS development also needs Xcode. On
Ubuntu/Debian Linux, the native prerequisites are:

```bash
sudo apt-get install cmake ninja-build pkg-config
```

Source development supports Apple Silicon macOS and Linux on ARM64 or x86-64.
Published wheels remain macOS-only. The experimental IREE runtime story is
currently Metal-only; the interactive engine and compiler-only SPIR-V story
build and run on both operating systems.

Clone xmojo and its editable dependencies into one directory:

```bash
git clone --depth 1 https://github.com/AnswerDotAI/xmojo.git
git clone --depth 1 https://github.com/modular/modular.git
git clone --depth 1 https://github.com/iree-org/iree.git
git clone --depth 1 https://github.com/nlohmann/json.git nlohmann-json
git clone --depth 1 https://github.com/jupyter-xeus/xeus.git
git clone --depth 1 https://github.com/jupyter-xeus/xeus-zmq.git
git clone --depth 1 https://github.com/zeromq/libzmq.git
git clone --depth 1 https://github.com/zeromq/cppzmq.git
git -C iree submodule update --init --depth 1 third_party/flatcc
```

For a reproducible release build, check out the revisions in
`bazel/versions.bzl` and the IREE revision in `docs/modular-gpu-pipeline.md`.
Normal development intentionally uses the current local worktrees.

The open Modular build enables only its default LLVM targets. xmojo's SPIR-V
backend needs one local root-module configuration line. Add it between the
existing `use_extension` and `use_repo` calls in `modular/MODULE.bazel`:

```starlark
llvm_configure = use_extension("//bazel/public-patches:llvm_project.bzl", "llvm_configure")
llvm_configure.configure(extra_targets = ["SPIRV"])
use_repo(llvm_configure, "llvm-project")
```

This is the only local Modular edit. It selects code already present in
Modular's pinned LLVM checkout; xmojo contains the target implementation.
Keeping it in the root module means every xmojo build uses the same LLVM
configuration and Bazel cache key.

Set up the editable Python project and its native command with:

```bash
uv sync
uv run xmojo --version
```

The setuptools editable build asks Bazel for a development build of the native
assets and stages them under the ignored `python/xmojo/_native` directory.
Python edits remain immediately visible; rerun `uv sync` after native, Mojo, or
build-configuration changes.

The main development stories are:

```bash
./bazelw test @xmojo//:session_test @xmojo//:interpreter_test \
  @xmojo//:cli_test @xmojo//:kernel_test @xmojo//:spirv_target_test
tests/iree_hal/test
```

The first command builds the source compiler, stdlib overlay, frontends, and
compiler-only typed SPIR-V story. Its Python tests use the `python` on `PATH`
and require `conkernelclient`. In the AnswerAI development workspace, activate
the workspace environment before running them:

```bash
source ~/aai-ws/.venv/bin/activate
```

The second command independently builds the runtime-only IREE Metal story and
is only run on macOS.

CMake automatically materializes Modular's BoringSSL headers and stages the
xeus libraries when Bazel first needs them. On Linux it compiles those
libraries with Modular's pinned Clang and Ubuntu 22.04 sysroot, matching the ABI
used by the final Bazel link. An xmojo-owned UUID implementation uses the same
BoringSSL randomness, so the host distribution's `libuuid` cannot leak across
that boundary.

The native accelerator narrative is currently built independently with CMake:

```bash
tests/iree_hal/test
```

It discovers and selects Metal devices through `auto`, `metal`, and
`metal:<ordinal>` selectors, then verifies asynchronous dispatch, persistent
buffer views, byte-packed push constants, executable replacement, and exact
readback through `libxmojo_gpu`. See `docs/modular-gpu-pipeline.md` for the
compiler/runtime boundary and the next integration checkpoints.

The compiler-only accelerator narrative is `@xmojo//:spirv_target_test`. It
registers xmojo-owned `TargetTraits`, `TargetLowering`, and `TargetBackend`
implementations, compiles a typed Mojo function through `compile_info`, and
checks its Vulkan storage-buffer and push-constant interface in textual and
binary SPIR-V. The internal target value is in `xmojo.spirv`; its raw argument
annotations are temporary compiler scaffolding rather than a public GPU API.

## Interactive compiler PoC

`InteractiveParser` does not call `parseREPLExpression` and does not create an
LLDB-style REPL context. It borrows three language-independent ideas from
Modular's `ParserDriverREPL.cpp`, with attribution in the source:

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
distribution. The current wheel tag is `macosx_12_0_arm64`.

The platform wheel packages the native frontend, the three runtime dylibs it
needs, ordinary and interactive-overlay `std` packages, and base and
Modular-GPU variants of the small `xmojo` package. The base variant contains
the notebook display API but
deliberately excludes the MAX-dependent GPU module. Tiny Python console entry
points locate these resources relative to `site-packages`, set the explicit
CompilerRT and import paths, then replace themselves with the native process.
REPL and kernel sessions use the overlay so output reaches their host callbacks;
`build` and `precompile` switch to the ordinary stdlib so generated artifacts
retain normal standalone I/O behavior.
Bazel's much larger compiler-tool runfiles closure is not needed because
compilation and ORC linking happen in-process. Apart from macOS system libraries
and frameworks, the installed commands therefore have no runtime dependency on
Bazel outputs, a source checkout, or an installed Mojo SDK.

The base wheel has no Mojo or MAX package dependency. Its optional `modular-gpu`
extra depends on the matching `max-core` nightly. When explicitly requested,
the launcher locates `max-core` through
Python package metadata, asks its `gpu-query` for the target accelerator, adds
its Mojo package directory, and passes the absolute AsyncRT Mojo bindings,
official compiler, and Modular-root paths into the native frontend. Local
development builds retain Bazel's debug configuration and are intentionally
large; published wheels use `-c opt`.

The current source/package mapping is deliberately one-to-one:

```text
Modular source  33cd4694b19649bec7f5acac88b0430371805dc6
Mojo package    1.1.0.dev2026082005
MAX package     26.6.0.dev2026082005
Mojo compiler   Mojo 1.1.0.dev2026082005 (c72288dd)
```

`bazel/versions.bzl` is authoritative for this mapping and the recorded sibling
dependency revisions. Updating Modular means updating it, the Pixi lock, the
official compiler assertion, and the optional MAX requirement together. The
wheel carries its source-built CompilerRT and compiled stdlib overlay as part of
xmojo's exact native ABI and output-hook implementation.

`tools/build_wheel.sh` rejects the wrong revision or a dirty worktree for every
source dependency, while allowing and checking Modular's single documented
SPIR-V configuration line. It checks the duplicated Python and Pixi pins and
invokes `uv build --wheel`. It defaults to `bazelw`; set
`XMOJO_BAZEL_WRAPPER=./bazelw2` when that is the server allocated to the current
session. `XMOJO_ALLOW_DIRTY=1` skips only the xmojo cleanliness check for local
packaging development; it must not be used for a published build.

On macOS, the wrapper passes `xcode-select`'s developer directory explicitly to
Bazel's target and exec action environments. This avoids Bazel's
LaunchServices-based `xcode-locator`, which can reject an otherwise working
Xcode installation on macOS 26, without modifying Bazel or the selected Xcode.
Keeping this setting in every wrapper invocation also preserves consistent
action keys between development and release builds.

## Official GPU compilation

The source-built compiler handles each cell's MAX host code and ORC linking,
but it cannot emit Apple's closed `air64` backend. `xmojo.gpu.compile[func]`
therefore calls back into `InteractiveSession`, which invokes the exactly
matched official Mojo compiler on the session's accumulated declaration
source. A generated temporary shared library uses `std.compile.compile_info`
to expose the device object and linkage name; xmojo copies those bytes and
unloads the temporary library.

The Mojo API loads that object through the cell's real `DeviceContext` and
returns `CompiledKernel[func, ...]`. It retains the original function value for
source-name reflection and separately infers its declared argument types. Each
`enqueue` checks the number and device conversion of its arguments at compile
time, then reuses MAX's Metal encoder or the generic device encoder. The first
version supports top-level, nonparameterized, noncapturing functions with an
ordinary identifier.

Official device artifacts are cached by the exact generated source,
accelerator target, official compiler contents, and the contents and resolution
order of every Mojo package on the compiler import paths. Cache entries contain
only the object bytes and linkage name; each `DeviceContext` still loads its own
device handle. A miss runs one synchronous `mojo build`, then publishes the
entry with an atomic rename. Failures and builds whose package fingerprint
changes during compilation are not cached. Corrupt entries are misses. The
default platform cache can be overridden with `XMOJO_GPU_CACHE_DIR` or the
native `--gpu-cache-directory` option.

A Modular GPU process must load its MAX runtime globally before
`Init::getOrCreateContext`. The native session canonicalizes the configured
runtime path under a mutex, permanently retains the first successful load,
reuses that runtime for later sessions, and rejects attempts to mix a different
MAX runtime into the process. The native `xmojo` frontend accepts
`--target-accelerator`, `--gpu-runtime-library`, `--mojo-compiler`, and
`--modular-home`; installed launchers resolve these from the pinned optional
dependency rather than asking notebook users for paths.

On macOS with Pixi and Xcode's Metal toolchain installed
(`xcodebuild -downloadComponent MetalToolchain`):

```bash
./bazelw2 test @xmojo//:gpu_shared_library_test
```

This compiles a kernel submitted as cell source, rejects an incorrectly typed
launch during host compilation, performs repeated Metal dispatch and readback,
and checks reuse and conflict handling for the process-wide MAX runtime.

CPU `print()` calls use the stdlib's `print_emit_fn` hook. Generated code calls
an explicitly registered ORC symbol, which routes stdout and stderr to the
callback active for that synchronous session execution. Calls without a
callback and calls targeting other file descriptors fall back to POSIX
`write`. Each interactive module sets `HEAP_BUFFER_BYTES=131072`, making the
buffer stack storage per active print call rather than permanent process
storage. Direct `FileDescriptor.write_*` calls and GPU output remain outside
the current boundary.

The generated cell wrapper catches an uncaught Mojo `Error` and synchronously
returns its message and optional Mojo stack trace through a registered ORC
symbol. Runtime errors are distinct from compiler diagnostics and leave the
session usable. Native crashes and JIT failures after an archive is added make
the session unusable instead of claiming recovery from potentially inconsistent
state.

The public Mojo package provides `display`, `HTMLRepr`, `MarkdownRepr`,
`SVGRepr`, `LaTeXRepr`, and `MIMEBundleRepr`. Traits make rich representation
explicit and compiler-checked: Mojo currently has no structural “has this
method” reflection. Rendering produces one synchronous `DisplayEvent` through
registered ORC symbols. `display()` becomes Jupyter `display_data`, while the
compiler-inserted helper becomes `execute_result`. Bundles currently contain
textual MIME data only; metadata, transient display IDs, and binary buffers are
not modeled.

Completion and inspection use a second `MojoParserContext` containing replayed
committed cells. This keeps speculative editor requests out of executable ORC
state. Completion calls `codeCompleteREPLExpression`; inspection first collects
the compiler's resolved references and renders `PublicDecl` Markdown. Persistent
variables are synthetic wrapper parameters rather than renderable declarations,
so their fallback remains compiler-driven: roots use their resolved
`MojoASTTypeRef`, and members use an exact documented REPL-completion result.
Persistent root completions expose the same compiler-rendered declaration as
their Jupyter completion signature.
Completeness uses Mojo's lexer for delimiters and lexical errors, then recognizes
trailing suite colons and operators to supply notebook indentation. The session
API uses UTF-8 byte offsets, as Modular's compiler does; `MojoInterpreter`
translates these to and from Jupyter's Unicode-character offsets.

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

- Keep Modular's compiler sources free of xmojo patches; the root module may
  select LLVM's SPIR-V target as documented above.
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
repositories, toolchain, visibility policy, output-user root, and implicit
`build-mojo` config. These infrastructure settings must remain identical across
sessions for reliable disk-cache reuse. Ordinary Bazel configurations remain
available: fastbuild/debug and `-c opt` use independent cached outputs, while
named configurations such as `--config=asan` may be selected explicitly.

## Tested dependency revisions

The current compiler and kernel PoC is tested against:

```text
modular  33cd4694b19649bec7f5acac88b0430371805dc6
nlohmann-json 55f93686c01528224f448c19128836e7df245f72
xeus     69d6d1397c68ba0ac1f6ab766dbeebb8a81e5b03
xeus-zmq 660e6c6ca75badbe55b295cec8c8dd020a5540f0
libzmq   46493370217ac135246617fa2f6ac819d8b61bfc
cppzmq   7f0530688804c2b5b6b0d985773405593fd25ca8
```

When updating Modular, replace the recorded revision rather than supporting
both old and new APIs, then run:

```bash
./bazelw test @xmojo//:session_test @xmojo//:interpreter_test @xmojo//:cli_test @xmojo//:kernel_test @xmojo//:spirv_target_test
./bazelw build @xmojo//:xmojo
uv build --wheel
python tests/wheel_test.py dist/xmojo-*.whl tests/cli_test.py tests/kernel_test.py 0.0.2026082005.post1
```
