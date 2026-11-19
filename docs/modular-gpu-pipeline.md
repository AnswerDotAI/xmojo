# Native Modular GPU integration

xmojo embeds the compiler and runtime from the unmodified Modular revision in
`bazel/versions.bzl`. GPU code uses `max.gpu` and `max.gpu.host`, including
`DeviceContext.enqueue_function` and `DeviceContext.compile_function`.

## Compilation and execution

`InteractiveParser` builds each cell in a persistent compiler context and resolves
references to previously committed declarations. `InteractiveSession` clones that
cell and runs Modular's normal `KGENCompiler::runKGENPipeline`. MAX's device
compilation requests enter the native offload pipeline during that compilation.
Modular owns target selection, device specialization, kernel argument layout,
backend code generation, and the device artifact format.

After lowering, xmojo slices the reachable host module, emits an archive, adds it
to the session's ORC execution engine, and executes the cell entry point. MAX's
runtime loads and launches the embedded device code. xmojo does not replay source
through a second compiler or implement a separate GPU launch ABI.

The default accelerator comes from
`Driver::Device::getAcceleratorArchOrEmpty()`, the same device discovery used by
Modular's driver. An explicit `SessionOptions::targetAccelerator` or
`--target-accelerator` overrides it.

## Native linking

`BUILD.bazel` links Modular's target registrations and compiler libraries through
their upstream targets. The frontend's shared dependencies include CompilerRT,
Driver Compilation, Driver Runtime, and AsyncRT Mojo bindings. They retain the
upstream shared owners of process-wide registries. ORC resolves the generated
host code's runtime references through the process symbol generator.

Both the source runfiles environment and the wheel carry `std.mojoc`,
`max.mojoc`, and `xmojo.mojoc`, plus the runtime shared libraries. The wheel
staging rule follows the frontend's shared-library dependency graph, including
transitive libraries, without packaging unrelated compiler-tool runfiles.

The Python launcher sets the paths to those packaged resources. It does not
query or invoke another Mojo installation. Standalone `build` and `precompile`
commands select the ordinary stdlib; interactive sessions select the stdlib
overlay that routes host output to xmojo callbacks.

## Output and session lifetime

The stdlib overlay replaces `_plugin/_impl.mojo`. Host code selects
`XMojoPlugin`; accelerator code keeps `CompilationTarget.current().stdlib_plugin`.
This preserves Modular's device-specific behavior while forwarding host output
to the REPL or Jupyter IOPub stream. xmojo does not intercept file descriptors.

Owned device contexts and buffers use xmojo's existing typed persistent storage.
They can be created in one cell and used in later cells. Values are destroyed in
reverse declaration order while the session's ORC code is still alive. Borrowed
values whose origins cannot outlive a cell remain invalid persistent values.

A compilation failure executes no code and commits no declarations. Runtime
errors retain already committed declarations and completed side effects. An
explicit `context.synchronize()` provides the boundary for checking completion
and observing asynchronous GPU work; submitting a cell alone is not a device
synchronization operation.

## GPU platform requirements

Metal requires the [Xcode Metal toolchain](../README.md#gpu-support). An accelerator target in the compiler does not guarantee that the device and driver are available.

Linux GPU execution is unverified in xmojo. The pinned Modular backend requires these components outside the wheel:

- NVIDIA: a CUDA driver and either `libNVPTX.so` (`MODULAR_NVPTX_LIBRARY_PATH`) or a compatible `ptxas` (`MODULAR_NVPTX_COMPILER_PATH`).
- AMD: a HIP runtime and a compatible `lld` (`MODULAR_MOJO_MAX_LLD_PATH`).

## Verification

See [Development layout](../DEV.md#development-layout) for the Bazel commands and test dependencies.

Run the two hardware tests on each backend you claim to support:

- `gpu_test` compiles and launches a Mojo kernel through `InteractiveSession`. It checks readback after repeated launches in independent sessions.
- `gpu_kernel_test` launches a parameterized kernel with device allocations carried across Jupyter cells. It checks that a cell with a compile error cannot mutate the buffer or prevent later use.

To exercise the GPU Jupyter story using the installed wheel, pass `tests/gpu_kernel_test.py` in place of `tests/kernel_test.py` to the [wheel test](../DEV.md#python-packaging-and-wheel-layout). The CPU wheel tests do not verify GPU execution.

## Initial Metal verification

On 2026-11-17, the pinned source build passed all seven session, interpreter,
CLI, Jupyter, and GPU test targets on an Apple M1 Pro running macOS 27.0.1.
The staged distribution assets also built successfully. A fresh editable
installation containing only xmojo passed the CLI, Jupyter, GPU REPL, and GPU
Jupyter stories with its bundled native libraries. Standalone compiler tests
used the SDK from the selected Xcode, as the macOS Bazel wrapper now supplies.

NVIDIA and AMD hardware, Linux builds, and optimized release wheels were not
verified in that run.

## Integration boundaries

| Concern | Owner |
| --- | --- |
| Cell parsing, persistent values, diagnostics, and display | xmojo `InteractiveParser` and `InteractiveSession` |
| Device functions, buffers, launch, and synchronization | Modular `max/mojo/gpu` |
| Target traits, lowering, offload, and object compilation | Modular `Mojo` compiler libraries |
| Device discovery and runtime services | Modular `MLRT/Driver` and `AsyncRT` |
| REPL and Jupyter transport | xmojo frontends |
