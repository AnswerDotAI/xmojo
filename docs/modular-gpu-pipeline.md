# Modular's accelerator pipeline and xmojo's native GPU architecture

This document traces the accelerator path visible in the open Modular source
and uses it to place xmojo's compiler and runtime work on the same
architectural seams. It is based on Modular commit
`33cd4694b19649bec7f5acac88b0430371805dc6`. The IREE runtime investigation is
against IREE commit `7778c15918526ecbb4b9606e2cfaa8c177854cd5`.

The open repository does not contain the concrete NVIDIA, AMD, or Metal target
implementations or the accelerator part of AsyncRT. Conclusions about those
pieces are marked as **inference**. Everything else below is directly visible
in the cited source.

## Result

The shape to follow is:

```text
Mojo source API
    |
    | selects a !kgen.target and creates kgen.compile_offload
    v
KGEN offload elaboration
    |
    | slices and specializes a target module
    v
TargetLowering                 TargetTraits
    |                               |
    | KGEN/POP -> LLVM dialect      | metadata and target identity
    v                               |
LLVM IR ----------------------------+
    |
    | optimization, device libraries, final code generation
    v
TargetBackend
    |
    | emits target artifact bytes
    v
host-side CompiledFunctionInfo
    |
    | runtime loads bytes, encodes arguments, launches work
    v
accelerator runtime
```

xmojo's registration of target traits, lowering, and backend implementations is
therefore on the intended compiler extension boundary. A target backend should
emit a complete target artifact; it should not own a device, allocate buffers,
or launch work.

The initial runtime boundary is an xmojo-owned C++ API implemented by
IREE's base runtime, common HAL, and selected HAL drivers:

```text
xmojo Device / Buffer / Executable / Event
    |
    | private adapter; no IREE types in the Mojo API
    v
IREE base + HAL
    |
    +-- Metal
    +-- Vulkan
    +-- CUDA
    `-- HIP or AMDGPU
```

xmojo will not use the IREE compiler, VM, VMFB execution, model runtime, or
tensor machinery. ORC remains responsible for host execution and persistent
notebook state. IREE HAL is responsible only for devices, memory, executables,
queues, dispatch, and synchronization.

There is not a target-neutral GPU IR underneath today's `std.gpu`.
Many public GPU operations select NVVM, AMDGCN, or AIR intrinsics directly in
Mojo. Supporting useful portable Mojo kernels consequently needs four pieces
rather than only a runtime driver:

1. target descriptions for the selected artifact families;
2. a portable Mojo-facing kernel subset;
3. KGEN/LLVM lowering and artifact emission;
4. an xmojo launch ABI implemented over IREE HAL.

The first compiler route uses SPIR-V: Vulkan can consume packaged SPIR-V
directly, while Metal will use SPIRV-Cross to produce MSL before packaging it
for IREE's Metal driver. The compiler-only PoC now registers an independent
SPIR-V traits/lowering/backend set and emits a typed Vulkan compute interface
from ordinary Mojo functions. Later NVPTX and AMDGPU targets can emit
PTX/cubin and HSACO for the CUDA and HIP/AMDGPU drivers. These targets share
the source-level contract and offload machinery while providing different
lowerings and artifact formats.

## 1. The target is a compile-time value

Mojo represents a compilation target as `!kgen.target`. A target contains at
least a triple, architecture, features, data layout, index and SIMD widths, and
a `stdlib_plugin` name. `CompilationTarget` exposes those fields to Mojo
compile-time code.

The built-in GPU descriptions use these target families:

| API | Triple | Stdlib plugin | Typical artifact |
| --- | --- | --- | --- |
| CUDA | `nvptx64-nvidia-cuda` | `cuda` | PTX or device object |
| HIP | `amdgcn-amd-amdhsa` | `hip` | AMDGPU object |
| Metal | `air64-apple-macosx` | `metal` | AIR/Metal object |

The individual GPU models in `std.gpu.host.info` select an architecture and
data layout within one of those families. `get_gpu_target()` returns the
corresponding `!kgen.target`; it does not return a runtime device.

The `stdlib_plugin` field is independent of the C++ compiler backend. It
selects a Mojo `PluginHooks` implementation at compile time. The current hooks
cover operations such as specialized math, stack allocation, named address
spaces, printing, and assertions. CUDA, HIP, and Metal plugin types are in the
open stdlib, although they currently leave the published hooks at their
defaults.

This gives a new backend two related identities:

- a C++ target selected by the triple;
- a Mojo stdlib policy selected by `stdlib_plugin`.

Each xmojo accelerator target should define its own plugin identity rather than
relying on an omitted plugin field resolving to xmojo's host-oriented
`default` plugin.

### A limitation in the current target model

The C++ `TargetTraits::isGPU()` query is extensible through the target
registry. Mojo's `std.sys.is_gpu()` is not: it is the disjunction of the
hard-coded NVIDIA, AMD, and Apple triple checks. The public GPU primitives use
that Mojo query and the same vendor checks.

A registered external accelerator backend is therefore a GPU to KGEN but not to stdlib
compile-time code. Adding an xmojo stdlib plugin does not itself change that.
For independent xmojo development, the smallest clean route is a portable
surface in `xmojo.gpu`; making `std.gpu` recognize externally supplied GPU
targets is a separate upstream API problem.

## 2. `std.gpu` is a portable API, not a neutral compiler dialect

The public names are deliberately common across vendors. For example,
`thread_idx`, `block_idx`, `block_dim`, `grid_dim`, and `global_idx` have one
Mojo spelling. Their implementations select target LLVM intrinsics at compile
time:

| Mojo operation | NVIDIA | AMD | Apple |
| --- | --- | --- | --- |
| `thread_idx` | `llvm.nvvm.read.ptx.sreg.tid.*` | `llvm.amdgcn.workitem.id.*` | `llvm.air.thread_position_in_threadgroup.*` |
| `block_idx` | `llvm.nvvm.read.ptx.sreg.ctaid.*` | `llvm.amdgcn.workgroup.id.*` | `llvm.air.threadgroup_position_in_grid.*` |
| `block_dim` | NVVM `ntid` registers | implicit-argument loads | AIR `threads_per_threadgroup` |
| `barrier()` | `nvvm.barrier` MLIR op | AMDGCN fences and barrier | `llvm.air.wg.barrier` |

`global_idx` itself is portable arithmetic over the vendor-specific thread,
block, and block-size operations. Warp operations, atomics, memory utilities,
and much of `max.gpu` contain more vendor-specific branches and intrinsics.

This design has two consequences:

1. KGEN does not receive a generic `gpu.thread_id` operation for all of these
   calls and choose a backend later. The target has already affected the Mojo
   specialization.
2. Reusing the existing names for xmojo's portable surface is reasonable, but
   their implementation must gain an external-target case or be provided by
   an xmojo package.

LLVM's SPIR-V backend already provides suitable primitives, including
`llvm.spv.thread.id`, `llvm.spv.thread.id.in.group`, `llvm.spv.group.id`,
`llvm.spv.workgroup.size`, and group memory barriers. A first xmojo surface can
map the common index and barrier operations to those intrinsics without
inventing a new MLIR dialect. That is an implementation choice for the source
API, not part of `TargetBackend`.

The initial portable contract should stay small and honest: invocation IDs,
workgroup IDs and dimensions, storage-buffer access, scalar arithmetic,
branches and loops, and a workgroup barrier when its memory semantics are
defined. Vendor-only warp and matrix operations should fail at compile time
rather than receive approximate behavior.

## 3. `compile_info` enters the offload pipeline

`std.compile.compile_info[func, target=..., emission_kind=...]()` creates a
`kgen.compile_offload` operation. The operation records:

- the target;
- assembly, LLVM, bitcode, or object emission;
- compile and link option strings;
- the function to compile.

During elaboration, KGEN resolves the function and all parameter bindings. It
groups offloads by target and emission options, assigns each kernel an integer
ID, and slices the exported function plus transitive dependencies into a
standalone module. That module is re-elaborated for the offload target, so
compile-time target checks in `std.gpu` select the device implementation.

Captured values are handled explicitly. KGEN records capture sizes and creates
a host-side function that populates capture storage. The source contains a
warning that this mechanism is tightly coupled to the existing GPU module, so
it should not be treated as the stable foundation of a portable GPU ABI.

After code generation, the original host `kgen.compile_offload` is replaced by
a structure containing:

- emitted bytes;
- a content-derived module name;
- the capture count;
- a pointer to capture sizes.

`CompiledFunctionInfo` adds the function linkage name and the emission-kind
label. The result is ordinary host-side data. This is the key boundary between
compiler and runtime.

## 4. The three target extension interfaces

Each concrete target self-registers through a static initializer. Lookup is by
LLVM triple. KGEN resolves an implementation once when an `ObjectCompiler` is
created and reports an unsupported-target error rather than silently falling
back to the host.

Only the host implementations are present in the open tree. The interfaces and
their call sites nevertheless expose the intended division of work.

### `TargetTraits`: identity and cheap facts

`TargetTraits` deliberately lives below the code-generation layer. It provides:

- triple matching and a short name;
- whether the target is a GPU;
- default CPU/architecture information;
- emitted file extensions;
- whether offload object files exist;
- stack address-space constraints;
- code-generation triple normalization;
- bitcode version and supported accelerator metadata.

The `isBaseTarget()` distinction controls whether the target requires MAX to
be installed. Its comments describe this as a gate for “MAX-only” targets, not
as a host-versus-accelerator distinction. An independently supplied
accelerator target being a base target is therefore a reasonable
interpretation: it does not depend on MAX.

### `TargetLowering`: target semantics and kernel ABI

`TargetLowering` owns target policy while the program is still MLIR. Its hooks
cover:

- target-specific POP-to-LLVM rewrite patterns;
- module-scoped lowering patterns;
- late passes over LLVM-dialect MLIR;
- convergent-operation recognition;
- marking and identifying exported kernels;
- kernel argument memory and indirection rules;
- target verification;
- mapping argument and function metadata;
- target-specific dtype legalization;
- finalization of converted LLVM-dialect functions.

The late-pass hook runs after generic lowering and canonicalization but before
translation to LLVM IR. This is the final point where a backend can perform a
module-level transformation while retaining MLIR type information.

The typed ABI must be decided before translation to opaque LLVM pointers, but
the adapter itself need not be built there. The current xmojo PoC uses
`TargetBackend::prepareModuleForLowering` to validate the KGEN signature and
preserved argument annotations, then records a compact versioned manifest as
function metadata. At LLVM IR, the backend uses that manifest to build the
parameterless shader entry point and resource accesses. This keeps type
reconstruction out of LLVM while using LLVM's first-class SPIR-V resource
intrinsics for the final interface.

### `TargetBackend`: LLVM policy and emitted artifacts

`TargetBackend` operates at the LLVM/code-generation layer. It controls:

- splitting and whether code generation is interprocedural;
- whether missing kernel IDs are errors for the offload target;
- shared-memory address spaces;
- target-machine option adjustment and finalization;
- LLVM bitcode serialization;
- the complete optimization pipeline or additions to the standard pipeline;
- sanitizer support;
- device/runtime bitcode libraries;
- final kernel code-generation attributes;
- assembly, object, and archive emission.

The base code calls these hooks in roughly this order:

1. `prepareModuleForLowering` on KGEN/POP MLIR;
2. the generic lowering pipeline plus `TargetLowering` hooks;
3. translation to LLVM IR;
4. target runtime/device-library linking;
5. `attachCodegenAttributes` on the identified kernel entry;
6. target-machine adjustment and creation;
7. LLVM optimization, either generic or backend-owned;
8. `emitAssembly` or `emitObject`;
9. optional archive combination.

The interface comments name NVPTX and Metal examples: NVPTX augments the
standard pipeline and requires a single module in some paths, while Metal owns
an AIR legalization pipeline and overrides bitcode emission. Those comments
are direct evidence for the shape of the closed implementations even though
their source is absent.

## 5. Artifact loading and launch are separate

MAX's open Mojo host API compiles a `DeviceFunction` with `compile_info`, then
calls the runtime boundary:

```text
AsyncRT_DeviceContext_loadFunction(
    context,
    module_name,
    function_name,
    artifact_bytes,
    artifact_length,
    ...)
```

The accelerator implementation behind this boundary is not in the open
repository. The surrounding code still establishes the responsibilities:

- compiler: emit bytes and a linkage name;
- runtime: turn those bytes into a device function for one context;
- launch layer: encode host arguments, retain device buffers, and enqueue a
  grid/block launch;
- synchronization layer: expose completion and errors.

`DevicePassable` and `DeviceTypeEncoder` make host-to-device argument encoding
a first-class Mojo concern. The default encoder writes device representations;
the Metal encoder additionally retains the buffer handles referred to by
device pointers. Layout queries use the target's data layout rather than the
host layout.

This is a useful model for xmojo even without MAX:

- an xmojo compiler target emits a self-contained driver artifact;
- an xmojo Mojo runtime API validates and encodes the kernel arguments;
- a C++ runtime adapter creates IREE HAL buffers and executables, submits queue
  dispatches, and represents completion with semaphores.

IREE HAL is an execution abstraction, not a compiler abstraction. It does not
turn vendor-specialized Mojo into portable kernels and it does not accept one
universal executable format. xmojo must still form the typed kernel ABI, lower
portable primitives, emit target code, and package the metadata expected by
the chosen driver.

## 6. What can be inferred about Modular's closed targets

The following points are **inferences** supported by open interfaces and
comments, rather than observations of the missing source:

1. Each accelerator family supplies concrete traits, lowering, and backend
   implementations and links them into the compiler so their static
   registrations run.
2. NVPTX, AMDGPU, and Metal use distinct target triples and distinct backend
   pipelines. They are not modes of one vendor-switching backend.
3. The target lowering establishes each device calling convention and argument
   ABI before LLVM emission. The runtime knows the matching artifact and launch
   conventions.
4. Device runtime libraries are linked by the backend, while the host runtime
   dynamically loads the completed artifact.
5. Metal may normalize from the public `air64` target to a different LLVM
   code-generation target and owns an AIR-specific LLVM pipeline and bitcode
   writer. This is suggested explicitly by interface comments and
   `isMetalTriple`, but the actual implementation is closed.

The design lesson is not to reproduce guessed class names or internal
packaging. It is to preserve the same boundaries: one target implementation
per execution model, a byte-artifact boundary, and a separate runtime driver.

## 7. IREE HAL as the runtime implementation

### Selected subset

The intended dependency is:

```text
IREE base runtime
IREE async support required by device creation
IREE common HAL
selected HAL driver and its utilities
FlatCC parsing/building for driver executable containers
```

The following are explicitly outside the process:

```text
IREE compiler and its LLVM/MLIR revision
IREE VM and VMFB execution
IREE high-level runtime/session API
IREE tensor and model machinery
```

This avoids an LLVM/MLIR collision with the exact Modular revision linked into
xmojo. IREE's build confirms the separation: `IREE_BUILD_COMPILER=OFF` reports
that LLVM and MLIR are not added.

Using IREE's VM would introduce a second host execution system where ORC and
Mojo already provide control flow and persistent state. Linking the IREE
compiler would introduce its separate LLVM/MLIR revision. If a future artifact
needs IREE compiler-side serialization, an out-of-process tool is safer than
putting both compiler stacks in xmojo; it is not needed for the initial test.
The experimental `iree-hal-streaming` project is useful evidence for dynamic
loading of raw PTX, HSACO, and SPIR-V, but it is currently focused on other
drivers and does not provide the production Metal route required here.

### Local runtime-only build result

At IREE commit `7778c15918526ecbb4b9606e2cfaa8c177854cd5`, a shallow clone plus
only the `third_party/flatcc` submodule is sufficient to configure and build
the Metal HAL library. The tested configuration disables the compiler, tests,
benchmarks, samples, Python bindings, all default drivers, local executable
loaders, plugins, `cpuinfo`, and tracing; it enables only Metal.

The resulting build compiles IREE base, async, IO, common HAL, FlatCC parsing
and building, the Metal driver, and the Metal driver's utility libraries. It
also builds FlatCC and IREE data-embedding tools used during the build; those
tools are not linked into the result. It does not compile IREE's LLVM
submodule.

The narrative probe is in `tests/iree_hal`. Its CMake file names only four
direct IREE targets: the proactor pool, FlatCC building support, the Metal
driver, and the generated Metal executable schema. After extraction, the
xmojo-authored static archive is 41,016 bytes and the complete narrative test
executable is 412,528 bytes on arm64 macOS. Its only dynamic dependencies are
macOS system libraries plus Foundation, Metal, and libc++; there is no IREE
shared library, compiler, VM, LLVM, or MLIR dependency.

Current IREE Metal intentionally targets Metal 3. A macOS 12 deployment failed
on unguarded `MTLGPUFamilyMetal3` and `MTLLanguageVersion3_0` uses; macOS 13
compiled successfully. xmojo only needs to support current OS releases, so
this is not a constraint and we should not patch IREE for older systems.

### Direct HAL model

The runtime can create the Metal driver and default device without a registry,
or use driver registration when implementing backend discovery. Device
creation currently requires an IREE async proactor pool. Before submitting
work, even one device must be assigned to a single-device
`iree_hal_device_group_t` backed by an `iree_async_frontier_tracker_t`. Group
creation assigns the device's topology and completion axis. Dispatch without
this step currently dereferences a null frontier tracker rather than returning
an error, so the adapter must make an incompletely initialized device
unrepresentable.

Once initialized, the device directly supplies:

- allocation through its HAL allocator;
- executable cache creation and executable preparation;
- direct `iree_hal_device_queue_dispatch` submission;
- semaphore-based dependencies and completion;
- queued transfers and host-visible mapping; and
- device and driver enumeration.

Simple xmojo launches need no IREE VM and no persistent IREE command graph.
`queue_dispatch` accepts the executable function, workgroup configuration,
32-bit constant bytes, an ordered buffer-reference list, wait semaphores, and
signal semaphores.

### Metal executable container

The Metal driver does not consume raw SPIR-V. It consumes an IREE executable
container whose payload contains:

- one or more MSL sources or metallibs;
- named compute pipelines;
- a fixed default threadgroup size;
- ordered binding flags;
- the number of 32-bit dispatch constants; and
- optional debug and source information.

At the investigated revision, the FlatBuffer root has the `MTL1` identifier
and is prefixed by IREE's 64-byte executable header carrying the magic,
version, and content size. The driver compiles embedded MSL with Metal 3 or
loads an embedded metallib. Its current argument ABI uses an MSL argument
buffer at buffer index 0 and dispatch constants at buffer index 3. These are
IREE implementation details that belong in the adapter, not xmojo's public
artifact model.

The current IREE tree is internally inconsistent about the format name: the
device query advertises `metal-msl-fb`, while the Metal no-op executable cache
checks `MTLE`, and format inference writes `PTXE`. The narrative probe confirms
that `iree_hal_executable_cache_prepare_executable` accepts an `MTL1` container
labelled `metal-msl-fb`; the prepare path currently loads the container without
consulting the cache's `can_prepare_format` result. This looks like active
format work or a defect on current main. Because xmojo will pin an exact IREE
revision, internal format evolution is manageable, but the adapter must contain
it in one place.

### Runtime narrative probe result

The first two checkpoints pass on an M5 Max running macOS 26.6. One runtime,
device, executable cache, timeline semaphore, and 64-element device buffer are
created. An embedded MSL `add` kernel is loaded and dispatched twice with
different constants. Exact readback confirms that the buffer contains the
cumulative result. The executable is then destroyed, an MSL `multiply` kernel
is loaded, and another dispatch confirms that the same buffer retained its
state. The complete test takes about 0.5 seconds, including Metal's runtime MSL
compilation.

The probe also found a Metal enumeration issue on this system. A direct initial
call to `MTLCopyAllDevices()` returns an empty array even though
`MTLCreateSystemDefaultDevice()` succeeds. Calling the latter first makes the
subsequent enumeration used by IREE succeed. The probe records this explicitly
rather than hiding it in the xmojo API. Before adopting the runtime adapter we
should either fix IREE's Metal enumeration to fall back to the system default
device or establish that this is an Apple API constraint IREE expects clients
to handle.

### Runtime abstraction checkpoint

The probe has been extracted into the static `libxmojo_gpu` library. Its public
header contains no IREE or Metal types. The implemented surface is deliberately
small:

```cpp
auto available = enumerateDevices();
auto device = Device::open("auto");       // also metal or metal:0
auto buffer = device.createBuffer(bytes);
auto executable = device.load(artifact);
auto event = executable.dispatch(bindings, constants, workgroups);
event.wait();
```

`KernelArtifact` carries the code format and bytes, entry-point name, fixed
workgroup size, ordered binding access, and exact push-constant byte size. The
first implemented format is embedded MSL. Driver-specific FlatBuffer
construction, conversion from byte size to IREE's 32-bit constant count,
Metal's argument-buffer indices, IREE handles, device-group setup, and the
current Metal enumeration initialization are confined to `Runtime.mm`.

`Device`, `Buffer`, `Executable`, and `Event` are small shared handles. Buffers
and executables retain their device. An event retains the device timeline and
payload value, but does not own the submitted buffers or executable: IREE's
Metal queue retains the command buffer and all referenced resources until its
completion handler runs. Discarding an event therefore remains asynchronous.
A per-device submission mutex keeps timeline allocation and queue submission in
the same order for concurrent callers; waiting on one payload also covers all
earlier work on that device timeline.

Bindings are byte-range `BufferView`s rather than whole allocations, which
supports tensor views and future suballocation without changing the launch
contract. Push constants cross the runtime boundary as opaque bytes whose size
must exactly match the artifact and be a multiple of four; the future typed
compiler ABI owns scalar layout and packing.

The current implementation is honestly Metal-specific. It allocates
host-visible device-local buffers and packages one MSL entry point per
executable. These constraints are sufficient for the current story without
claiming a finished cross-driver memory policy. The public types already place
them behind the device and artifact boundaries where later Vulkan and
discrete-device implementations can differ.

## 8. Recommended xmojo architecture

### Public Mojo surface

The user-facing API should describe accelerators, not its runtime implementation:

```mojo
from xmojo.gpu import Device, devices, compile

print(devices())
# [Device("metal:0"), ...]

var device = Device.best_available()
var output = device.buffer[Float32](256)
var kernel = compile[increment](device)
var completion = kernel.enqueue(output, grid_dim=4)
completion.wait()
```

Backend and device are separate concepts. Expected selectors include:

```mojo
Device("metal")
Device("metal:0")
Device("vulkan:0")
Device("cuda:1")
Device("hip:0")
```

Introspection should include available backends, device counts, and explicit
availability checks. `Device.best_available()` follows a documented preference
order. A notebook may set a persistent default device, and the kernel launcher
may eventually accept `--device=auto` or an explicit selector. Discovery must
not eagerly initialize every runtime.

Buffers belong to one device. Passing a buffer to a kernel for another device
is an error rather than an implicit copy. Compilation follows the selected
device and may create a new target variant lazily.

### C++ runtime boundary

xmojo owns a small modern C++ API:

```cpp
auto devices = enumerateDevices();
auto device = Device::open("metal:0");
auto buffer = device.createBuffer(size);
auto executable = device.load(artifact);
auto event = executable.dispatch(bindings, constants, workgroups);
```

IREE handles and formats stay private to the implementation. The first
experiment should statically link the runtime subset because that is simplest
and there is no LLVM collision to isolate. A private shared library is
justified only by a measured packaging or symbol-management benefit; it does
not provide isolation automatically.

Platform builds should include only useful drivers:

```text
macOS          Metal
Linux generic  Vulkan
Linux NVIDIA   Vulkan, later CUDA
Linux AMD      Vulkan, later HIP or AMDGPU
```

### Compiler targets

The portable Mojo source contract should cover only the common semantics we can
lower correctly: invocation and workgroup IDs, typed storage buffers, scalar
arithmetic, branches and loops, and a well-defined workgroup barrier.
Unsupported warp, matrix, texture, and vendor operations should fail at compile
time.

The first two artifact targets can share most lowering while having distinct
identities and packaging:

```text
portable Mojo kernel
    |
    +-- Vulkan SPIR-V target
    |      -> logical SPIR-V
    |      -> Vulkan IREE executable container
    |      -> IREE Vulkan HAL
    |
    `-- Metal-via-SPIR-V target
           -> logical SPIR-V
           -> SPIRV-Cross
           -> MSL
           -> Metal IREE executable container
           -> IREE Metal HAL
```

Later sibling targets can extend capability without changing the notebook API:

```text
NVPTX  -> PTX or cubin -> IREE CUDA HAL
AMDGPU -> HSACO        -> IREE HIP or AMDGPU HAL
Metal  -> native artifact, if an appropriate compiler route becomes available
```

Target preparation must form the resource and scalar ABI while typed MLIR
information remains available. An LLVM-level wrapper may then realize that
already-defined ABI. `attachCodegenAttributes()` should perform only genuine
final LLVM marking. Workgroup size is an executable property; dispatch
specifies workgroup counts. If a backend supports runtime specialization, that
can be represented explicitly rather than pretending all backends accept an
arbitrary launch-time block size.

### xmojo-owned artifact contract

Compiler code should not construct driver-specific FlatBuffers throughout the
lowering pipeline. It should first produce an xmojo-owned description:

```text
KernelArtifact
    target and code format
    code bytes
    entry points
        public name and encoded device name
        fixed workgroup size
        ordered bindings and access modes
        constant/scalar layout
        required features
```

One small adapter translates that description into the pinned IREE driver's
container. Cached kernels use the exact source and compiler revision, target,
device capabilities, compile options, and artifact-contract version. A cache
may hold several variants; one stock HAL executable should not be assumed to be
a cross-driver fat binary.

### Asynchronous notebook execution

Each ORC session can retain a selected device, persistent buffers, loaded
executables, and outstanding events independently of ordinary persistent Mojo
values. An `Event` maps naturally to an IREE semaphore and payload value.
Waiting in Mojo can initially be explicit; later the xeus kernel can integrate
completion without blocking its message-processing thread.

The runtime must retain all resources used by a dispatch until its completion
event is satisfied. Destroying or replacing a compiled kernel must not destroy
the device or unrelated buffers. Runtime and validation failures become Mojo
errors associated with the cell that submitted or awaited the operation.

## 9. Concrete checkpoints

1. **Runtime-only Metal narrative test — complete.** One driver and device load
   known MSL, retain a buffer across repeated dispatches, wait with a timeline
   semaphore, and produce exact readback.
2. **Independent lifetimes — complete.** Replacing the executable retains the
   same device and buffer and the next dispatch sees the existing device state.
3. **Integration measurement — complete for Metal.** The direct build targets,
   linked footprint, dynamic dependencies, initialization requirements, and
   current IREE rough edges are recorded above.
4. **xmojo runtime types and discovery — complete for Metal.** The IREE-free
   `Device`, `Buffer`, `Executable`, `Event`, and `KernelArtifact` abstractions
   implement the narrative, including `auto`, `metal`, and `metal:0`
   selection.
5. **Typed SPIR-V ABI formation — compiler PoC complete.** xmojo analyzes the
   specialized KGEN signature before pointer erasure, carries a versioned ABI
   manifest through LLVM metadata, and creates a parameterless Vulkan compute
   wrapper with ordered read-only/read-write storage buffers and 32-bit scalar
   push constants. The initial contract accepts `Float32`, `Int32`, and
   `UInt32`, uses a fixed `1 x 1 x 1` workgroup, rejects unannotated pointers
   and unsupported signatures, and emits both textual and binary SPIR-V. The
   narrative test checks the SPIR-V magic, compute entry point, descriptor
   bindings, `NonWritable` decoration, push-constant interface, and rejection
   diagnostic. The raw `@__llvm_arg_metadata` annotations are internal
   scaffolding; the portable Mojo buffer API will generate them.
6. **Connect Metal compilation.** Translate emitted logical SPIR-V with
   standalone SPIRV-Cross, package MSL through the adapter, and run the same
   persistent-buffer story from Mojo source.
7. **Add Vulkan.** Package the corresponding SPIR-V and run the same public API
   on an AMD or NVIDIA Vulkan device.
8. **Broaden semantics deliberately.** Add workgroup memory and barriers, then
   atomics and selected math, with an executable narrative test for each
   semantic group.

This order resolves runtime packaging and typed compilation separately before
joining them. Failure of the IREE runtime or artifact-container experiments is
the checkpoint at which the runtime choice should be reconsidered.

## Source map

All Modular paths below are relative to the repository root at the commit named
at the top of this document.

| Concern | Source |
| --- | --- |
| Target value and target queries | `mojo/stdlib/std/sys/info.mojo` |
| GPU target descriptions | `mojo/stdlib/std/gpu/host/info.mojo` |
| Stdlib plugin selection | `mojo/stdlib/std/_plugin/_impl.mojo`, `_overlay.mojo`, `_trait.mojo` |
| GPU IDs and dimensions | `mojo/stdlib/std/gpu/primitives/id.mojo` |
| GPU barriers | `max/mojo/max/gpu/sync/sync.mojo` |
| Device argument contract | `mojo/stdlib/std/builtin/device_passable.mojo` |
| `compile_info` entry point | `mojo/stdlib/std/compile/compile.mojo` |
| Offload operation definition | `KGEN/include/KGEN/KGENDialect/KGENOps.td` |
| Offload grouping and rewriting | `KGEN/lib/Elaborator/Elaborator.cpp` |
| Offload compilation and embedding | `KGEN/lib/Compiler/KGENCompiler.cpp` |
| Target metadata interface | `KGEN/lib/Target/TargetTraits.h` |
| MLIR target interface | `KGEN/lib/Target/TargetLowering.h` |
| LLVM/codegen target interface | `KGEN/include/KGEN/Compiler/Target/TargetBackend.h` |
| Target dispatch and object emission | `KGEN/lib/Compiler/ObjectCompiler/ObjectCompiler.cpp` |
| MAX load and launch boundary | `max/mojo/max/gpu/host/device_context.mojo` |

The relevant LLVM SPIR-V evidence is in `llvm/IR/IntrinsicsSPIRV.td`,
`llvm/lib/Target/SPIRV`, `llvm/docs/SPIRVUsage.md`, and the
`llvm/test/CodeGen/SPIRV` resource and builtin tests.

IREE paths below are relative to the IREE revision named at the top of this
document:

| Concern | Source |
| --- | --- |
| Common device and direct queue APIs | `runtime/src/iree/hal/device.h` |
| Driver and device enumeration | `runtime/src/iree/hal/driver.h` |
| Executable cache and preparation | `runtime/src/iree/hal/executable_cache.h` |
| Executable functions and reflection | `runtime/src/iree/hal/executable.h` |
| Metal public driver construction | `runtime/src/iree/hal/drivers/metal/api.h` |
| Single-device topology and completion tracking | `runtime/src/iree/hal/device_group.h`, `runtime/src/iree/async/frontier_tracker.h` |
| Metal device and direct dispatch | `runtime/src/iree/hal/drivers/metal/metal_device.m`, `direct_command_buffer.m` |
| Metal executable loading | `runtime/src/iree/hal/drivers/metal/executable.m` |
| Metal executable schema | `runtime/src/iree/schemas/metal_executable_def.fbs` |
| Executable file prefix | `runtime/src/iree/base/internal/flatcc/parsing.h`, `runtime/src/iree/hal/utils/executable_header.c` |
| IREE's compiler-side Metal serializer, for reference only | `compiler/plugins/target/MetalSPIRV/MetalSPIRVTarget.cpp` |
| Runtime-only submodule check | `build_tools/scripts/git/check_submodule_init.py`, `runtime_submodules.txt` |
