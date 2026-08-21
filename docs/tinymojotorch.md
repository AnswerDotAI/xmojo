# TinyMojoTorch: an imperative, notebook-native tensor system

`TinyMojoTorch` is a working description, not necessarily a project or package
name. It describes a direction for a small, fully open tensor system built on
Mojo, MLIR, xmojo, and native accelerator drivers.

The intended result combines four ideas:

- PyTorch's original imperative, code-is-the-model user experience;
- tinygrad's small, inspectable implementation and complete accelerator scope;
- Mojo and MLIR's ability to compile the program directly, without tracing a
  second graph language; and
- an nbdev-like workflow in which notebooks are executable documentation and
  export complete Mojo modules with a larger optimization horizon.

The interactive and exported systems must not be separate implementations. A
notebook cell and an exported function use the same tensor API, compiler IR,
lowering, kernels, and runtime. They differ only in how much of the program the
compiler can see before execution.

The accelerator compiler and runtime design is covered separately in
[Modular's accelerator pipeline and xmojo's native GPU
architecture](modular-gpu-pipeline.md). That document is authoritative for
KGEN target extension points, portable kernel semantics, artifact formats,
IREE HAL integration, and backend checkpoints. This document describes the
tensor programming model and the system above that boundary.

## Historical lessons

### PyTorch: ordinary programs as models

Torch7 showed that a productive tensor and neural-network system could be a
library in a general-purpose, interactive language rather than a declarative
model description. Its CPU and CUDA implementation lived largely in the TH,
THC, THNN, and THCUNN native libraries, with Lua providing the user-facing
environment.

The original PyTorch reused that body of native tensor work, replaced Lua with
Python, and introduced a new automatic-differentiation engine. Its important
innovation was not a particular tensor operator. It was the decision that a
model should be an ordinary program:

- Python control flow was model control flow;
- execution was immediate and easy to inspect;
- the differentiation graph was created by running the program;
- errors appeared at the operation that caused them;
- arbitrary user code could surround tensor operations; and
- accelerator work was enqueued asynchronously, so imperative host execution
  did not imply synchronous device execution.

This was a usability-first response to static graph frameworks. Researchers
could use a debugger, print values, change control flow, and write a new layer
as a normal function. PyTorch treated flexibility as the primary product and
accepted reasonable rather than theoretically maximal performance.

Its early implementation also offers warnings. Per-operation eager execution
materialized intermediates and hid fusion opportunities. A language boundary
between Python and native kernels made framework overhead important. ATen
created a cleaner C++ tensor API, but the system accumulated dispatch layers,
generated operator registration, separate eager and compiled paths, and a
large compatibility surface. Several generations of tracing, scripting, graph
capture, and compilation were needed because Python itself was not a suitable
compiler IR. Production requirements and strong backwards compatibility made
later simplification extremely difficult.

We want the original product insight without recreating the accumulated
architecture:

> Tensor programs should feel like normal programs, but their operations
> should remain visible to a compiler from the beginning.

### tinygrad: the whole stack can remain understandable

Tinygrad began as a very small, eager, micrograd-like tensor and autograd
library. Its present architecture was extracted gradually from working code:

1. lazy operations made fusion possible;
2. `ShapeTracker` made reshape, permutation, expansion, slicing, and related
   movement operations into index transformations instead of copies;
3. UOps began as a low-level instruction sequence emitted by the kernel
   linearizer;
4. UOps became a graph and acquired structural identity and rewriting;
5. symbolic expressions, lazy tensor operations, scheduling, and execution
   were progressively unified around that representation; and
6. small renderers, compilers, allocators, and runtimes made the complete path
   to several accelerators visible in one repository.

The durable lessons are more important than the precise current class layout:

- a small algebra of elementwise, reduction, and movement operations can
  express a surprisingly complete tensor library;
- high-level operations should be compositions rather than new backend
  primitives whenever practical;
- views and symbolic indexing are central compiler semantics, not tensor API
  decoration;
- compilation artifacts, generated kernels, dispatches, and performance
  counters should be easy to inspect;
- backend contracts can be small;
- an explicit custom kernel is a normal escape hatch;
- end-to-end accelerator support need not require millions of lines; and
- line-count pressure is valuable when it exposes unnecessary concepts, but
  harmful when it becomes code golf.

Tinygrad also supplies cautions. Shape and view composition contain real
mathematical difficulty. Scheduling has changed repeatedly. A general graph
rewrite engine can become a compile-time bottleneck. A universal node type is
attractive but can absorb unrelated semantics until its invariants become
hard to explain. We should adopt tinygrad's economy and transparency without
assuming that its one-IR architecture is ideal for a language already built on
MLIR.

### Mojo and MLIR change the compilation problem

PyTorch and tinygrad begin with Python objects. They need tracing, lazy graph
construction, or operator interception before a compiler can see a tensor
program.

Mojo source is compiled into MLIR. Its SSA program is already a typed dataflow
graph containing ordinary control flow, calls, values, and effects. Building a
second user-visible `Graph` whose nodes duplicate the original program would
discard this advantage.

Modular's KGEN architecture deliberately emphasizes explicit parametric kernel
generation rather than the upstream MLIR `linalg`, `affine`, and `scf`
dialects. Important schedules, layouts, target choices, and hardware
operations are expressed in Mojo and specialized before conventional code
generation. MAX then provides a separate graph layer for reasoning across
kernels.

This project explores a different point in the design space. We retain Mojo's
explicit kernel programming for performance-sensitive work, but allow ordinary
tensor expressions to remain compiler-visible long enough for fusion,
bufferization, and kernel outlining. This may use upstream MLIR tensor
dialects, a very small xmojo-owned internal dialect, or transparent inlined
Mojo constructs. The choice should be made by executable experiments. The
invariant is that this is internal compiler representation, not a second
programming model exposed to users.

## Product goal

The user should be able to write straightforward Mojo:

```mojo
fn block(x: Tensor[DType.float16, 2], weight: Tensor[DType.float16, 2]):
    return rms_norm(x @ weight + x)

var output = block(input, weight)
output
```

In a notebook or REPL, this behaves imperatively. The cell runs, values that
escape the cell remain usable in later cells, and the final expression can be
displayed.

When the notebook exports a complete module or executable, the same source is
compiled with notebook boundaries removed. Intermediates that existed only to
cross interactive cells can remain SSA values, functions can be inlined, and
the compiler can fuse and plan memory over the complete compilation region.

The system should be useful without a notebook as an ordinary Mojo library and
compiler. xmojo provides the especially productive interactive and literate
environment, not a required runtime graph executor.

## Goals

1. **An imperative Mojo tensor API.** Tensor code uses normal functions,
   methods, structs, loops, conditions, errors, and debugger-visible values.
2. **One semantic and implementation path.** Interactive, exported, CPU, and
   accelerator execution share tensor definitions and compiler lowering.
3. **A fully open path to accelerators.** MAX and Modular's closed accelerator
   compiler are not part of the architecture.
4. **Compiler-visible tensor semantics.** Operators are not lowered to opaque
   runtime dispatches before fusion and memory planning.
5. **Notebook-native development.** A notebook is executable documentation,
   a test narrative, and source for a real Mojo package or executable.
6. **Explicit high-performance escape hatches.** Users can replace an
   operation or region with a parameterized Mojo kernel without leaving the
   language or framework.
7. **A small comprehensible core.** One capable developer should be able to
   follow an expression through optimization, code generation, allocation,
   dispatch, and readback.
8. **Good-enough general optimization, excellent important kernels.** Normal
   transformations handle composition and glue; a small curated kernel set
   handles operations that require hardware-sensitive schedules.
9. **Transparent behavior.** Generated IR, fusion decisions, buffer plans,
   kernel source or assembly, artifacts, dispatches, and timings can be
   inspected from the notebook.
10. **Native asynchronous execution.** Device work and transfers compose with
    events and do not require a hidden persistent command graph.

## Non-goals

- PyTorch API compatibility or exhaustive operator coverage;
- preserving the allocation or dispatch timing of every interactive cell in
  an exported build;
- implicit device movement or global device placement;
- a user-visible graph-building API;
- adopting MAX Graph, IREE's compiler, IREE VM, VMFB execution, or IREE tensor
  machinery;
- compiling arbitrary Mojo language semantics for a GPU in the first version;
- automatically discovering the best matmul, attention, or tensor-core
  schedule from generic loops;
- matching the fastest framework on every program or device;
- backwards compatibility while the architecture is being discovered; or
- abstraction layers added only for hypothetical future implementations.

## Design principles

### The program is the graph

MLIR represents the program's dataflow. Users should never need to construct a
parallel object graph to make their code optimizable. Internal compiler passes
may use graph algorithms and dialect operations; that does not make the graph
a user-facing language.

### Notebook cells are interaction boundaries, not program semantics

An interactive cell must execute before the user submits the next one. Values
that escape it require runtime storage. An exported compilation has no such
requirement, so cell boundaries must not become optimization barriers.

Explicit observation and effects remain real barriers. Automatic notebook
display is interactive UI behavior and is not inserted into exported source.

### Keep semantics high-level until the optimization horizon is known

This is the wrong lowering for an operator:

```text
Tensor.__mul__ -> immediately load executable -> HAL dispatch
```

Once multiplication is an opaque dispatch, a compiler cannot fuse it with an
addition or activation. Tensor operations should remain as pure, typed
compiler operations until fusion, kernel selection, and bufferization have
run. HAL dispatch is emitted near the bottom of the pipeline.

### Prefer value semantics; make mutation obvious

Pure tensor values allow substitution, fusion, reordering, and memory
elimination. Views should describe alternate indexing of the same logical
value without eagerly copying.

Mutation remains important for optimizers, stateful models, and systems code,
but it must be explicit in both the API and effect model. Aliasing, host access,
random-number state, synchronization, I/O, and exceptions constrain legal
transformations. An in-place-looking convenience must not conceal semantics
the compiler cannot prove.

### Devices and transfers are explicit

Every materialized tensor belongs to a device. Operations involving
incompatible devices fail rather than silently copying. Transfers are visible
operations returning values or events. This keeps performance and failure
behavior understandable and follows PyTorch's successful preference for
simple, explicit device semantics.

### Use progressive lowering, not a universal representation

A source tensor expression, a scheduled kernel, a vector loop, and a device
instruction have different useful invariants. They need not share one node
type. Use the smallest number of IR levels that make transformations clear,
and remove a level when it adds no leverage.

### Let ordinary optimization handle ordinary code

Canonicalization, constant propagation, inlining, view folding, elementwise
fusion, straightforward reductions, vectorization, dead allocation removal,
and buffer reuse should be compiler work rather than hand-written graph
execution logic.

### Write the hard kernels explicitly

GEMM, quantized GEMM, attention, tensor-core pipelines, and some reductions
need schedules and layouts that generic optimization is unlikely to discover.
They should be concise parameterized Mojo kernels with clear contracts. Models
and LLMs can generate candidates; correctness tests and benchmarks decide
which implementations enter the library.

### Optimize for understanding before feature count

New concepts require demonstrated value in an end-to-end story. Tests should
usually show a complete readable narrative rather than fragment behavior into
many fixtures. Generated bindings and platform glue may be large, but the
authored tensor/compiler core should remain small enough to study.

## Programming model

### Tensor values

A first tensor type should distinguish compile-time facts from runtime facts
without encoding the whole shape into the type:

```mojo
Tensor[dtype: DType, rank: Int]
```

The dtype and rank are common specialization axes. Dimensions, strides, and
device identity are generally runtime values. Static dimensions may be
propagated and specialized when known, but making every dimension a type
parameter would cause excessive specialization and compile latency.

At an executing function boundary, a tensor owns or refers to:

- a device allocation;
- shape and stride metadata;
- dtype and layout information;
- the event after which its contents are valid; and
- explicit ownership or borrowing state.

Inside an optimization region, a tensor may remain an abstract SSA value with
no allocated buffer. Bufferization decides which values need storage and where
that storage can be shared or eliminated.

### Operations

The primitive semantic set should initially remain close to tinygrad's proven
decomposition:

- elementwise unary, binary, and selection operations;
- reductions;
- reshape, permute, expand, slice, pad, and related views;
- explicit copy and device transfer;
- gather/scatter only when their semantics are precisely defined; and
- explicit custom-kernel calls.

Matmul, convolution, normalization, activations, losses, and other public APIs
should be compositions where that remains clear and efficient. The compiler
may recognize a composition and select an optimized kernel without making the
optimized form part of the public semantics.

### Layouts and views

Views require a compact symbolic representation of shape, strides, offset,
masks, and transformations. We should reuse suitable MLIR shape, tensor,
memref, and affine-map machinery where it actually simplifies the system, but
not force non-affine layouts into an affine model.

The essential invariants are:

- a view does not allocate or copy;
- composition has one canonical meaning;
- bounds and masks are explicit;
- read and write aliasing is known before mutation;
- device ABI formation retains element and access types; and
- shape/view behavior has exhaustive narrative and property tests.

### Modules and parameters

Models should be ordinary Mojo structs rather than instances of a magical
graph base class. A small trait can provide explicit parameter enumeration,
state loading, and device transfer when those are needed. We should not build a
reflection or registration framework before real models demonstrate the
minimal contract.

### Automatic differentiation

Automatic differentiation is not required for the first accelerator vertical
slice, but the architecture must leave it a clean home.

The preferred long-term model is a transformation over the high-level pure
tensor IR before bufferization:

```text
value_and_grad[loss_function]
    -> differentiated tensor program
    -> joint optimization of forward and backward
    -> bufferization and kernels
```

This avoids a permanent runtime graph representation and allows forward and
backward fusion, saved-value analysis, and checkpointing to become compiler
decisions. A `backward()` convenience can be added later if it preserves the
same semantics. Mutation and general control-flow differentiation should be
added only with explicit, testable rules rather than hidden tape behavior.

### Asynchrony

Accelerator operations produce dependencies even when the user does not
explicitly name them. Runtime tensors retain the completion required before
their data can be consumed. Explicit events allow advanced composition:

```mojo
var done = kernel.enqueue(...)
done.wait()
```

Most tensor code should sequence correctly without manual waits. Host
readback, display, explicit synchronization, and destruction of live resources
must observe the appropriate event. The runtime retains buffers and
executables until dependent dispatches complete.

## Compiler architecture

The planned high-level path is:

```text
Mojo source
    |
    | Tensor operators remain compiler-visible
    v
typed tensor/value IR
    |
    | canonicalize, inline, fold views, differentiate if requested
    v
fused tensor regions and explicit library-kernel calls
    |
    | choose algorithms, tile ordinary regions, outline kernels
    v
host program + target kernel modules
    |
    | bufferize and plan lifetimes
    v
CPU code or accelerator artifacts + HAL dispatches
```

### Initial IR experiment

The first implementation should attempt to use registered upstream MLIR
`tensor`, `linalg`, `arith`, `math`, `scf`, `vector`, and related operations for
the semantics they express naturally. xmojo already owns the compiler context,
so it can register additional dialects and passes.

A small xmojo dialect is justified only for missing concepts such as:

- importing and exporting an owned external device buffer;
- device identity and capability requirements;
- an explicit call to a selected library kernel;
- asynchronous completion; or
- a semantic operation that cannot be represented cleanly upstream.

It must not duplicate upstream operations merely to put an xmojo name on them.
If direct, inlined Mojo produces equally useful IR with less machinery, prefer
it. These choices should be settled with one complete CPU and GPU story rather
than an abstract dialect design exercise.

### Fusion and kernel selection

The initial optimizer should be deliberately unsurprising:

1. inline small/private tensor functions;
2. canonicalize shapes and views;
3. fuse compatible elementwise producers and consumers;
4. fuse simple reductions with their elementwise producers or consumers where
   legality is obvious;
5. recognize a small set of optimized library patterns;
6. make allocation and host-observation boundaries explicit;
7. bufferize; and
8. outline and compile kernels.

Pattern selection should initially be deterministic and explainable. Search,
autotuning, learned cost models, and device-specific alternatives can be added
after stable measurements identify a real need. The generated plan should be
available to notebook inspection.

An explicit optimized kernel is an opaque semantic call at the tensor level,
but not necessarily an optimization barrier around its inputs and outputs. For
example, an elementwise epilogue may be folded into a matmul implementation
that advertises support for that epilogue.

### CPU execution

CPU is the reference backend and the quickest way to validate tensor
semantics. Abstract tensor regions lower to ordinary Mojo loops and SIMD, then
execute through xmojo's existing ORC engine interactively or normal native code
when exported.

CPU execution must not be a separate NumPy-like interpreter with subtly
different rules. It uses the same shape, dtype, view, mutation, and operation
semantics as accelerators. A deliberately simple scalar lowering can serve as
the correctness oracle before vectorized lowering is added.

### Accelerator execution

The accelerator compiler turns outlined target modules into a typed
`KernelArtifact`. The runtime loads artifacts, owns device resources, and
dispatches them. It does not optimize tensor programs.

The target compiler, portable kernel subset, artifact contract, and IREE HAL
adapter are specified in [the accelerator architecture
document](modular-gpu-pipeline.md). This document intentionally does not repeat
their driver-specific formats or implementation checkpoints.

## Interactive execution and module export

### Two optimization horizons

Suppose a notebook contains:

```mojo
var hidden = input @ weight
```

followed by:

```mojo
var output = gelu(hidden + bias)
output
```

Interactively, the first cell executes before the second is known. `hidden`
escapes the cell and therefore needs persistent runtime storage. The second
cell can still fuse addition and GELU. Displaying `output` waits for it and
reads enough data to render its representation.

An exported runner can instead contain:

```mojo
fn notebook_main(input, weight, bias):
    var hidden = input @ weight
    var output = gelu(hidden + bias)
    return output
```

`hidden` is now an ordinary intermediate. The compiler may eliminate its
standalone allocation and, when a selected matmul implementation permits,
incorporate the consumer as an epilogue.

The semantic operations are identical. Only the available compilation region
changed.

### Escaping values

At the end of an interactive cell:

- values referenced by later cells are moved into session-owned persistent
  storage;
- abstract tensors among those values are materialized into device buffers;
- non-escaping intermediates remain eligible for elimination;
- automatic final-expression display is an observation;
- explicit `display`, host reads, synchronization, and I/O are ordinary
  effects; and
- errors retain xmojo's existing rule that completed mutations remain visible
  while new variables from a raising cell are not persisted.

This extends xmojo's existing persistent-variable ABI rather than introducing
a notebook tensor graph.

### Exported structure

xmojo's parser already partitions module declarations from executable cell
statements. An nbdev-like exporter can use compiler parsing and source maps to
produce two related artifacts:

1. a reusable `.mojo` module containing selected declarations; and
2. an optional narrative runner or test function containing selected
   executable cells in order.

Module declarations are exported at module scope. Executable statements are
wrapped in a generated function so importing a package does not replay an
exploratory notebook. Notebook boundaries remain source locations and
documentation structure, not compiler barriers.

Automatic final-expression display rewriting is omitted from exported source.
An explicit `display(value)` remains an effect if the user chose to write it.
When a runner's final value is meaningful, cell metadata or an explicit return
should define it rather than inferring a reusable API from exploration.

### Cell roles

The first exporter needs only a small set of roles, inspired by nbdev:

- **export**: include declarations in the reusable module;
- **test/story**: include executable code in the compiled narrative test;
- **hide**: execute code but omit it from rendered documentation; and
- **documentation/example**: render and optionally execute without exporting
  it into the reusable API.

The exact directive syntax is a tooling choice. It should be valid Mojo
comments or notebook metadata and should not require a preprocessor language.

### Function and module scope

Fusion primarily occurs within a function. Whole-module compilation can inline
private and parametric helpers, but separately exported public functions remain
real boundaries unless their callers are visible and inlining is legal.

Notebook narratives intended to optimize as one computation must therefore
become one generated or explicit function. The exporter should not imply that
unrelated public functions become one global schedule.

## Observation and display

Tensor representation should distinguish metadata from data access. Shape,
dtype, device, readiness, and allocation information can often be displayed
without downloading the full tensor. A conventional small-tensor
representation may require synchronization and readback; large tensors should
use an explicit, bounded summary.

Notebook display is an important semantic boundary:

- a displayed value must represent completed work;
- a host value cannot refer to transient device storage;
- asynchronous failures are attributed to the cell that awaits or displays
  the result; and
- display must not silently remain in an exported optimized module.

Rich display can later expose compiler explanations, tensor layouts, kernel
plans, and profiling results through xmojo's existing MIME representation
traits.

## Runtime model

The tensor/compiler layer depends on a small xmojo-owned native vocabulary:

```text
Device
Buffer
Executable
Event
KernelArtifact
```

IREE HAL is the intended private implementation for native accelerator device,
allocation, executable, queue, and semaphore operations. HAL types and
driver-specific containers do not appear in the public Mojo tensor API.

ORC remains the interactive host executor. An exported executable uses normal
native Mojo host code. Both call the same runtime boundary and load the same
kind of kernel artifacts. There is no IREE VM or second host control-flow
system.

### Installation and backend selection

The intended supported-platform experience is one ordinary installation:

```bash
pip install xmojo
xmojo
```

The Python wheel is a distribution format for the native xmojo command,
compiler, Mojo packages, and notebook kernel; TinyMojoTorch remains a Mojo API.
Users should not need separate Mojo, IREE, LLVM, SPIRV-Cross, Metal, Vulkan,
CUDA, or ROCm development toolchains.

The first platform artifacts should contain only useful native drivers:

```text
Apple Silicon macOS   Metal
Linux                 Vulkan on AMD and NVIDIA
```

Vulkan provides one portable Linux baseline rather than requiring separate
framework installations for each vendor. It still requires the ordinary AMD
or NVIDIA system GPU driver, just as accelerator frameworks cannot replace a
working kernel driver. Metal is supplied by macOS.

Hardware is selected at runtime rather than installation time:

```mojo
var device = Device.open("auto")
var amd_or_nvidia = Device.open("vulkan:0")
var mac = Device.open("metal:0")
```

`auto` follows a documented preference order over available backends. Explicit
selectors remain stable within a process, buffers belong to one selected
device, and incompatible-device operations fail rather than copying silently.
The Jupyter kernel retains the selected default device and its resources for
the session.

Later native compiler/runtime paths can add PTX or cubin through IREE CUDA and
HSACO through IREE HIP or AMDGPU. If their linked size and system dependencies
remain reasonable they can live in the relevant platform wheel. Otherwise,
small backend wheels can register themselves with the same runtime discovery
layer. Neither arrangement changes tensor programs or the device API.

This is a packaging goal, not a current support claim. The Metal story is the
first implementation. AMD and NVIDIA support becomes releasable only when the
same compile, persistent-buffer, asynchronous-dispatch, and exact-readback
narrative runs on real hardware in CI or equivalent test machines.

## Performance philosophy

Performance is important, but simplicity and usability constrain how it is
obtained.

We expect three performance tiers:

1. **Generic compiler lowering** for uncommon operations and initial
   correctness.
2. **Normal fusion and vectorization** for elementwise work, views, casts, and
   straightforward reductions.
3. **Curated explicit kernels** for the few operations dominating important
   workloads.

For an LLM, the third group is relatively compact: dense and quantized matrix
multiplication, attention, normalization, embeddings/gathers, rotary
embeddings, and sampling-related kernels. A model dominated by a good curated
set can plausibly remain close to a much larger framework even if generic
compositions occasionally leave performance available.

A rough aspiration such as "within five percent on workloads covered by the
curated kernel set" is useful direction, not an API guarantee. We explicitly
accept some performance loss to avoid a large graph optimizer or opaque vendor
stack. Measurements should report compilation time, dispatch overhead, memory
traffic, and generated kernel quality rather than only end-to-end throughput.

## Inspectability and tooling

The notebook should make the implementation unusually easy to interrogate.
Useful operations include:

```text
explain(value or function)       semantic operations and inferred facts
show_fusion(function)            fused regions and barriers
show_buffers(function)           allocations, aliases, and reuse
show_kernel(function, target)    generated source/IR/assembly
show_dispatches(function)        executables, grids, bindings, and events
profile(function)                timings, bytes, and useful device counters
```

These names are illustrative. The principle is that debugging compiler
behavior should not require an external GUI or reverse engineering a cache
entry. Source locations must map explanations and failures back to notebook
cells.

## Testing strategy

Tests should prove user stories at the highest practical level and split only
where clean state, target availability, crashes, or failure isolation require
it.

The central narrative should:

1. define a small tensor computation across multiple notebook cells;
2. inspect and display intermediate values;
3. preserve a device tensor into a later cell;
4. export the notebook computation into a Mojo module and runner;
5. compile the exported form;
6. verify that former cell materializations disappear where legal;
7. compare exact CPU and accelerator results;
8. verify the expected fused kernels and dispatch count; and
9. repeat execution to exercise cached artifacts and persistent resources.

Additional focused stories should cover:

- shape/view composition, including masks and empty dimensions;
- dtype conversion and numerical edge cases;
- mutation and alias barriers;
- explicit device-transfer failures and success;
- asynchronous lifetime and error propagation;
- compiler and runtime diagnostics mapped to notebook source;
- a custom hot kernel replacing a generic lowering; and
- differentiated forward/backward execution when AD is introduced.

Reference CPU lowering and mathematical definitions are the correctness
oracles. Backend comparisons must not merely compare two paths sharing the
same faulty lowering.

## Delivery plan

Each phase ends in a readable, executable vertical slice. Later phases may
change earlier internal APIs; preserving experimental compatibility is not a
goal.

### Phase 0: establish the native runtime boundary

The Metal portion of this phase is complete. The IREE HAL runtime-only
experiment and xmojo-owned
`Device`/`Buffer`/`Executable`/`Event`/`KernelArtifact` boundary described in
the accelerator architecture document now cover device selection, byte-range
buffer views, opaque push constants, asynchronous dispatch, executable
replacement, and measured packaging and lifetime behavior. Vulkan and
discrete-device memory behavior remain later hardware-backed extensions of the
same boundary.

### Phase 1: a CPU tensor semantic slice

Implement the smallest compiler-visible tensor value supporting:

- owned contiguous buffers;
- dtype, rank, dynamic dimensions, and strides;
- a few elementwise operations;
- reshape, permutation, expansion, and slicing;
- one reduction; and
- scalar reference lowering through ORC.

The story should run naturally in xmojo and inspect the intermediate compiler
representation. No autograd, modules, neural-network layers, or accelerator
dispatch is needed yet.

This phase decides whether upstream MLIR tensor operations, direct Mojo
lowering, or a minimal internal dialect gives the cleanest representation.

### Phase 2: notebook export and dual optimization horizons

Build the minimal nbdev-like exporter around xmojo's real parser:

- identify declaration and executable cells;
- preserve source maps and documentation order;
- export a reusable module;
- generate a narrative runner/test;
- omit automatic display rewriting; and
- compile the exported artifact with `xmojo build`.

Demonstrate that the interactive form materializes an escaping tensor while
the exported form eliminates the corresponding boundary.

### Phase 3: fusion and bufferization

Add only the transformations required by the narrative:

- view canonicalization;
- elementwise fusion;
- a simple reduction fusion case;
- explicit effect barriers;
- device-neutral bufferization; and
- a human-readable fusion and allocation explanation.

Compilation time is measured from this phase onward. A rewrite system that is
pleasant but slow is not considered successful.

### Phase 4: first open accelerator vertical slice

Connect one outlined tensor region to the portable target and runtime path in
the accelerator document. Run the same semantic story on CPU and Metal through
IREE HAL, then add Vulkan without changing the public tensor code.

This phase proves typed buffer ABI formation, artifact packaging, persistent
device tensors across notebook cells, display/readback, and exported native
execution. It should not broaden the portable kernel language beyond what the
story needs.

### Phase 5: useful numerical kernels

Add operations in workload order rather than API-category order:

1. a solid matrix multiplication;
2. fused bias/activation epilogues;
3. normalization;
4. quantized matrix multiplication;
5. attention and rotary embedding; and
6. the remaining operations needed by one small real model.

Each optimized kernel begins as ordinary Mojo with an executable generic
fallback and exact tests. Generated or LLM-proposed variants are accepted only
after repeatable correctness and benchmark evidence.

### Phase 6: automatic differentiation and training

Introduce reverse-mode transformation over the established tensor IR, first
for straight-line pure functions. Compile forward and backward together and
make saved values visible in buffer plans. Then add control flow, mutation,
checkpointing, optimizers, and model parameter conventions only as demanded by
complete training stories.

### Phase 7: packaging and ecosystem

Separate the tensor package from xmojo where doing so produces a cleaner
ordinary-Mojo dependency. Publish self-contained PyPI platform wheels with
Metal on Apple Silicon and a Vulkan baseline on Linux for AMD and NVIDIA.
Include native CUDA or AMD backends in platform wheels when practical, or as
automatically discovered backend wheels when their size or system dependencies
justify separation. Keep xmojo as the reference interactive environment,
notebook exporter, and compiler distribution.

Broader backend work follows the accelerator document and measured need. The
public tensor and runtime APIs must not encode IREE, Metal, Vulkan, CUDA, or HIP
implementation details. Release claims require the complete narrative on real
hardware for every advertised backend.

## Early decision tests

Several questions should be answered by code rather than prolonged design:

1. Can upstream tensor/linalg operations coexist cleanly with Mojo value
   semantics and KGEN lowering in xmojo's compiler context?
2. What is the smallest representation that keeps elementwise expressions and
   views optimizable without creating a parallel runtime graph?
3. Can an abstract tensor cross ordinary Mojo helper functions without early
   allocation or opaque dispatch?
4. Where is the clean typed boundary between abstract tensor values and owned
   HAL buffers?
5. Does exported cell concatenation produce useful whole-function IR without
   surprising scope or lifetime changes?
6. What compiler effects are required to preserve mutation, randomness,
   display, and asynchronous errors?
7. Is compilation latency low enough for a notebook after realistic fusion
   and shape rewriting?

The Phase 1--4 narrative is designed to answer these before a broad tensor API
creates compatibility pressure.

## Success criteria

The direction is working when:

- tensor code reads as ordinary idiomatic Mojo;
- notebook errors and displays behave locally and predictably;
- the same code exports without tracing or rewriting into a graph API;
- exported compilation removes interactive materialization boundaries;
- users can see why a fusion, allocation, or dispatch occurred;
- one explicit Mojo kernel can replace a generic implementation without
  modifying the runtime;
- CPU and multiple accelerator drivers share semantics and tests;
- the authored core remains small enough to read end to end; and
- a useful model runs through an entirely open compiler and runtime stack.

## Further reading

- Soumith Chintala, [PyTorch's design
  origins](https://soumith.ch/blog/2023-12-17-pytorch-design-origins.md.html)
- Paszke et al., [PyTorch: An Imperative Style, High-Performance Deep Learning
  Library](https://arxiv.org/abs/1912.01703)
- tinygrad, [developer documentation](https://github.com/tinygrad/tinygrad/blob/master/docs/developer/developer.md)
- Mojo, [vision and architecture](https://docs.modular.com/mojo/vision/)
