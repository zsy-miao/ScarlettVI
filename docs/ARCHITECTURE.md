# ScarlettVI engineering architecture

## Scope

ScarlettVI is an **original C++20 Xbox Series X|S to Windows x64 compatibility-layer project**.
The same x86-64 ISA permits native execution once the console ABI, dependencies,
process environment, GPU command semantics and data formats are translated.
It is NOT yet a functioning game runtime. No game assets, SDK material, firmware,
console code, keys or proprietary binary libraries belong in this repository.

## Verified target properties (read-only inspection)

- `game_scarlett_master.exe` uses PE32+ x64 (machine 0x8664).
- MicrosoftGame.Config identifies a Scarlett-targeted executable, version 1.0.2.1.
- The target's original image base is 0x140000000.
- The target has an ~176 MiB virtual image, large native exception metadata,
  an import table with both Windows API-set and Xbox-specific modules.
- Confirmed Xbox-specific direct dependencies include `d3d12_xs.dll`,
  `xg_xs.dll`, `xmem.dll`, `xapu.dll`, `AcpHal.dll`, `XCRDAPI.dll`,
  `chad.dll`. These require custom console-service implementations.
- Release-mode read-only probe on this machine: all **341/341** executable Windows
  import slots bound, all **489/489** nested Windows import slots bound, all
  **315/315** title-local DLL import slots linked across **7** mapped DLLs.
  **41** total console-specific import *references* were discovered.
  The original experimental XMem bridge now links **15** references (including
  independently analysed `XMemVirtualQuery` and `XMemMapPhysicalPagesScatter`
  caller ABIs); a desktop D3D12 root-signature serialization bridge links **2**
  more; an original partial Xbox Device11 host COM factory links **1**
  additional reference; the original partial GPU VM configuration bridge
  links **1** more, and the original [host APU allocation bridge](APU_HOST_MEMORY.md)
  links the host `ApuCreateHeap`/`ApuAlloc`/`ApuFree` subset, leaving
  **8** Xbox import references unresolved.
  The [host-only XMem tools-memory capability bridge](XMEM_TOOLS_CAPABILITY.md)
  returns FALSE when no Xbox developer/tools partition exists; this
  reduces missing import addresses but does not supply console memory services.
  The [working-set error-path bridge](XMEM_WORKING_SET_FAILURE.md) also
  refuses unsupported Xbox budget queries without writing an assumed
  output structure. The APU bridge implements a bounded CPU allocation
  budget and real host allocations, but no console device-address mapping,
  APU hardware memory or DSP processing. The [owned 32-bit APU
  address mapping](APU_ADDRESS_TRANSLATION.md) now reversibly maps
  ScarlettVI-owned CPU allocations only; other title memory stays unsupported.
  The [family-4 SHAPE host buffer manager](APU_SHAPE_CONTEXTS.md) now
  implements bounded allocation and release, but not audio decoding.
  The [partial AcpHal guest factory](APU_HAL_FACTORY.md) supplies a
  bounded host object/vtable and ring storage, while its DSP configuration
  method still returns `E_NOTIMPL` and is tracked separately.
  The [partial XAPU host connection/result FIFO](XAPU_QUEUE_HOST.md)
  now links four additional named imports with actual owned handles,
  bounded FIFO storage and tested concurrent use; command submission
  remains unbound and audio decoding/playback is not implemented.
  The original [CPU FIR DSP backend](DSP_CPU_CONVOLUTION.md) now computes
  real, bounded interleaved floating-point convolution with streaming
  history and independent multichannel state, tested on generated samples.
  It is deliberately not bound to `chad.dll` while the guest buffer,
  stream activation and command contracts remain incomplete.
  The [host-typed XAPU-to-DSP bridge](XAPU_DSP_INTEGRATION.md) now
  actually filters input samples through a connection's FIR state and
  atomically queues a host-defined completion only on success, including
  actual output-bit checksums. This is not an Xbox packet decoder.
  The [partial XMem title/system host ledger](XMEM_TITLE_SYSTEM_TRANSFER.md)
  now links the title's observed XMemTransferMemory call and actually
  enforces transfer and allocation admission on host-owned XMemAlloc
  heap blocks. Sparse physical pages, virtual reservations and GPU
  allocations remain outside this quota; it is not Xbox partition emulation.
  The [partial Xbox DirectStorage factory](XBOX_DSTORAGE_FACTORY.md)
  now provides original static COM ownership and actual configured host
  staging memory through the title-verified +0x40 method. A separate
  [host-only async file queue](XBOX_DSTORAGE_HOST_ASYNC.md) now performs
  real bounded generated-file reads with FIFO completion and prevents
  staging resizing while alive. A [host-only enqueue/submit gate](XBOX_DSTORAGE_SUBMIT_GATE.md)
  now holds work until explicitly committed or until 32 requests are
  enqueued in the automatic mode. A separate [real guest-IID status
  array](XBOX_DSTORAGE_STATUS_ARRAY.md) now uses the corrected factory
  `+0x28` method and receives real S_OK/first-failure results from
  host-only FIFO barriers. An explicitly [fixture-rooted read-only file COM
  adapter](XBOX_DSTORAGE_FILE_HOST.md) now opens genuine generated files,
  duplicates read handles and holds staging leases until final Release;
  normal guest paths remain disabled. A [fixture-only queue COM prototype](XBOX_DSTORAGE_QUEUE_COM.md)
  now links the observed `CreateQueue1` IID to the real host worker and
  status-array barriers. Unsupported guest requests complete as errors,
  not fake reads; full console queues, game asset streaming, decompression
  and real Xbox destination buffers remain missing.
  [VIctory/ztrx research](VICTORY_REFERENCE_NOTES.md) exposes the major
  loader/shader/streaming gap relative to self-reported runnable ports.
  Import linkage is not
  a gameplay-ready audio service. Public emulator comparisons and remaining real runtime
  gates are documented in [research](PUBLIC_COMPATIBILITY_RESEARCH.md).
  Xbox GPU page tables, tile mappings, coherency and creation flags remain
  unsupported. Import linking is not proof of runtime correctness. The mapper applies final memory protection
  to **60** sections, registered **304274** x64 unwind function entries,
  installed **3** current-thread TLS templates (5776 bytes) and identified
  **2** callbacks (deferred, not executed). Managed host-created workers
  now have independent host TLS template copies; automatic guest thread
  attach and native compiler TLS in the Windows TEB remain unsupported.
  Loader callbacks, Xbox services and GPU translation remain to be implemented.
  A [generated PE64 entrypoint fixture](SYNTHETIC_PE_ENTRY.md) now successfully
  runs a tiny original x64 test executable with one real Windows import,
  six forced DIR64 relocations, host-managed TLS, deferred callback verification
  and protected sections. This validates only a synthetic native entrypoint;
  it does **not** execute the original Xbox title or solve DLL init.
  The second [generated DLL lifecycle fixture](SYNTHETIC_PE_ENTRY.md)
  explicitly runs only generated TLS callback and DllMain attach/detach
  code and verifies private TLS copies across four managed workers. This
  is not general-purpose game DLL callback dispatch.
  This does not imply that runtime behavior is correct.

## Native layer roadmap, in dependency order

1. **PE parser & mapper (implemented):** robust x64 PE header/section/import/delay-import/export
   inspection, data-only native image mapping, base relocation handling, PE section
   protection and x64 exception-function table registration, report generation.
2. **ABI import bridge (partly implemented):** precise import-blocker attribution
   and read-only startup-readiness report implemented. Host API-set import patching,
   recursive title-local DLL mapping, export forwarding (independent MSVC fixture
   verified), x64 unwind registration and host-managed per-thread TLS buffers,
   without actual game DLL initialization. Remaining: complete Xbox runtime
   HLE, automatic game DLL TLS callbacks, guest compiler TLS/TEB semantics,
   process environment and platform handle semantics.
3. **Console filesystem and runtime:** the original experimental 64 KiB
   pagefile-backed physical-page manager uses Windows VirtualAlloc2
   placeholders, MapViewOfFile3 and VirtualFree interception for true CPU
   aliases. Contiguous and non-contiguous scatter mappings across multiple
   regions are verified using independently observed title call patterns.
   Huge 16/24 GiB CPU virtual windows split 64 KiB placeholders only when
   individual pages are mapped. Remaining: console memory attributes,
   2 MiB clusters, real GPU-visible physical page coherency, virtual drives,
   packages, saves, services and jobs.
4. **GPU:** original native Windows D3D12 device, queues, command lists,
   upload resources, fences and root signatures verified on RTX 5060 Ti.
   GPU-executed texture transfers, independently compiled DXIL Model 6
   buffer compute and SRV/UAV texture shaders check 2,048 generated
   integers and 16,384 generated RGBA8 texture pixels. The guest 64 KiB
   physical-page bridge also executes DXIL compute and reads back 2,048
   results through guest virtual aliases.
   The 16/24 GiB logical guest GPU VA windows now use separate **4 GiB
   host D3D12 reserved-resource segments**. Real 64 KiB D3D12 default-heap
   tiles are mapped into two distant guest GPU windows and aliased across
   host resource boundaries. A 65,536-byte GPU copy/readback, explicit
   aliasing barriers and completion fences pass (see
   [GPU_TILES.md](GPU_TILES.md)). The staged XMem CPU-page path now verifies 64 KiB readback through separate sparse D3D12 GPU heap tiles and explicit CPU-alias writeback (see [XMEM_GPU_STAGING.md](XMEM_GPU_STAGING.md)). Remaining: Xbox GPU page-table APIs and
   coherent CPU/GPU physical memory, console D3D12/XG methods, descriptors,
   RDNA2 shader decoding, real Xbox texture swizzles and frame presentation.
   The original [GPU texture detiling backend](GPU_TEXTURE_DETILE.md)
   now compiles actual SM6 DXIL and converts host synthetic Morton
   tiles into linear block-compressed/uncompressed texture bytes in
   **20 real GPU dispatch submissions** with exact readback. Xbox XG
   object/descriptor ABI and RDNA2 texture addressing remain unimplemented.
5. **Title features:** RPF9 header inventory implemented (12 top-level archives, 130.5 GB of file metadata examined without copying payloads); remaining RPF streaming, BCPack where applicable, shaders and render-target
   diagnostics, audio device & codec bridges, controller input, frame presentation.
6. **Execution and diagnostics:** safe thread/exception state, fault collection,
   frame-by-frame runtime validation on user-owned copies.

Import *existence* alone is not enough: return values, calling conventions,
object layouts, GPU memory semantics and sync behavior must be correct.
Arbitrarily downloading console system DLLs is neither required nor a safe or
reliable replacement for this work.

## Safety and data isolation

The target installation at `../GTA6.Xbox.v1.0.2.1` is **read-only** to all tools
and code in the first milestone. `inspect` and `map` never execute title entry
points, DLL initializers or TLS callbacks, and do not modify any title files.
Only original source code, configuration, documentation and ScarlettVI artwork
are eligible for the public repository.

## Development

Open `ScarlettVI.sln` with Visual Studio 2026 (v145), x64 Debug or Release.
`ScarlettVI.exe inspect <path-to-title-exe> --json <path-to-report.json>`
creates a precise machine-readable import/dependency inventory.
`ScarlettVI.exe readiness <path-to-title-exe> --json reports/readiness.json`
generates an exhaustive, owner-attributed unresolved import and runtime gate
report, with an explicit BLOCKED state for execution. It never runs title code.
`ScarlettVI.exe map <path-to-title-exe>` performs data-only mapping, import
resolution, unwind registration and TLS preparation. `ScarlettVI.exe gpu`
probes desktop D3D12 capabilities. `ScarlettVI.exe memory` validates original
64 KiB alias mappings and `ScarlettVI.exe memory-scatter` validates cross-region
scatter mapping. Neither command executes game instructions.
The `ScarlettVI.exe gpu-transfer`, `gpu-compute` and
`gpu-texture-shader` commands exercise actual host D3D12 copies, DXIL shader
compute execution and shader-visible texture descriptor binding respectively.
The `gpu-memory-bridge` mode also verifies guest physical pages, synthetic
GPU VA translation, actual host DXIL dispatch and aliased guest readback.
The `memory-sparse`, `gpu-sparse-va` and `gpu-vmem-config` modes verify
16/24 GiB sparse CPU/GPU address windows and partial Xbox configuration.
`gpu-xmem-tiles` uses the reusable [GpuPageStager](GPU_PAGE_STAGER.md)
component to verify three 64 KiB CPU XMem-to-D3D12 heap tile payloads
across two independent GPU sessions, GPU aliases on different native
segments, explicit CPU writeback, failure rejection and zero leaked pages
([XMEM_GPU_STAGING.md](XMEM_GPU_STAGING.md)).
`gpu-page-registry` validates four pinned XMem frames, eight CPU alias
lifetime pins, eight GPU mappings using four unique 64 KiB tile slots
inside one shared 4 MiB real D3D12 heap, 17 completed 64 KiB transfers
in ten actual native submissions (including two deliberately interrupted
submissions), safe heap-slot reuse, atomic pinned-unmap rejection and
retryable fence-safe cleanup with zero leaks
([GPU_FENCE_RECOVERY.md](GPU_FENCE_RECOVERY.md))
([GUEST_GPU_PAGE_REGISTRY.md](GUEST_GPU_PAGE_REGISTRY.md)).
`xg-calls-fixture` and the data-only `xg-calls <title.exe>` command
verify original x64 RIP-relative FF25 import jump stubs and E8 direct
call candidates for the two unresolved XG graphics imports. The title
scan confirms **four XGCreateTextureComputer and two XGSuggestSwizzleMode
call sites**. The new `xg-context` mode verifies E8 displacements and prints
strictly bounded call-site windows for independent analysis, with generated
negative-test fixtures. Neither command executes title code or guesses the ABI
([XG_CALLSITE_PROBE.md](XG_CALLSITE_PROBE.md)). A separate read-only
[descriptor evidence study](XG_DESCRIPTOR_EVIDENCE.md) records a few
verified title-local field transfers but does not define the Xbox XG ABI.
The original [synthetic field-projection fixture](XG_FIELD_PROJECTION.md)
models five observed transfers plus independently verified category-dependent
constants and zeroed fields with 512 generated checks across three branches
and 256 lookup-index cases, including bounds, overlap detection, explicit
coverage metadata and output preservation.
It does not implement a console import, or enable title execution.
A new [read-only XG indirect-call survey](XG_VCALL_SURVEY.md) extracts bounded
128-byte post-create code windows and finds five FF /2 indirect-call byte
patterns corroborated by Dumpbin at three caller sites. The FF /2 scanner
now avoids interpreting redundant REX-looking displacement bytes as call
prefixes and recognizes a local overlapping RSP-LEA shape; it is still not
a full x64 decoder. A bounded 32-byte
backward signature check now also finds a matching RCX-sourced table-register
load for each of the five indirect calls, and rejects inconsistent call
metadata. These are not decoded control flow and no COM method or lifetime
interpretation is claimed. The [bounded XG dispatch-argument survey](XG_DISPATCH_ARGUMENTS.md)
finds six RCX/RDX setup signatures before four of the five indirect calls,
revalidating call-site metadata and avoiding stale evidence across an
intervening indirect call. A separate [bounded outgoing stack-write
survey](XG_DISPATCH_STACK_SLOTS.md) corroborates three GPR-to-stack MOV
stores immediately before the complex `+0x50` dispatch. Neither scanner
establishes method argument semantics or ownership. A new
[bounded +0x50 register-timeline probe](XG_REGISTER_TIMELINE.md) observes
six REX.W LEA/MOV patterns and conservatively distinguishes R8's
source before its outgoing stack write from R8's later source at dispatch.
The [bounded +0x08 successor survey](XG_DISPATCH_SUCCESSORS.md) finds two
adjacent indirect dispatch sequences following `+0x50` and `+0x10` calls,
respectively, with a matching earlier storage-address expression. These
patterns cannot prove object identity or whether `+0x08` releases ownership.
A [bounded +0x10 post-call indexed-read and field-copy scanner](XG_POSTCALL_INDEXED_READS.md)
finds three stack accesses relative to the previously passed RDX stack
address, each followed by a same-width store to an RSI field. The observed
caller-side data copies do not prove that an Xbox method produced the data.
`gpu-page-scattered` uses a host-only planner to sort eight unordered
XMem-backed GPU-page descriptors into four native-resource-safe runs
across 4 GiB and 8 GiB D3D12 segment boundaries. It verifies **24
64 KiB real GPU transfers in three submissions**, outer CPU pinning,
third-run mapping-fence rollback, interrupted NULL-unmap retry and zero
leaks ([GPU_PAGE_SCATTERED.md](GPU_PAGE_SCATTERED.md)).
`gpu-page-batch` integrates the native tile batch helper with the
pinned XMem frame registry: four CPU-backed physical frames, eight
CPU aliases and eight GPU aliases are prevalidated and pinned before
**two real native GPU tile mapping calls**, followed by a queue fence.
It verifies **20 completed 64 KiB GPU transfers in five submissions**,
interoperability with single-frame registration and two-native-call
four-page **grouped NULL unmapping**, plus synthetic pre-/post-unmap
fence failures, retryable GPU retirement, safe re-registration and zero
leaked CPU pins/pages
([GPU_PAGE_BATCH_REGISTRATION.md](GPU_PAGE_BATCH_REGISTRATION.md)).
`gpu-tile-batch` verifies native one-resource multi-range
`UpdateTileMappings`: four selected physical heap tiles map to four
adjacent GPU guest tiles per call, across two distant resource aliases,
with **524,288 generated bytes** verified in two actual GPU readbacks.
It tests two different physical orders, four map/four NULL unmap calls,
and zero GPU mapping leaks ([GPU_TILE_BATCH.md](GPU_TILE_BATCH.md)).
`gpu-tile-map` verifies native D3D12 64 KiB tile residency and GPU physical
aliasing across segmented 16 GiB guest buffers (see [GPU_TILES.md](GPU_TILES.md)).
The gpu-texture-layout command verifies block-compressed, mipmapped Windows
GPU textures and an original reversible synthetic microtile layout.
The gpu-texture-detile command exercises a [real cached DXIL detiling
pipeline](GPU_TEXTURE_DETILE.md) against that layout with 20 GPU
submissions, bounds checks and exact R8/BC readback.
See [GPU_VIRTUAL_MEMORY.md](GPU_VIRTUAL_MEMORY.md).
All GPU modes use only independently generated test data.
See [GPU_BACKEND.md](GPU_BACKEND.md) and [MEMORY_BACKEND.md](MEMORY_BACKEND.md).

Verified behavior should always be updated here based on concrete experiments.
