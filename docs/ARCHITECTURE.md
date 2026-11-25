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
  The original experimental XMem bridge links **12** references (including
  independently analysed `XMemVirtualQuery` and `XMemMapPhysicalPagesScatter`
  caller ABIs); a desktop D3D12 root-signature serialization bridge links **2**
  more; an original partial Xbox Device11 host COM factory links **1**
  additional reference; the original partial GPU VM configuration bridge
  links **1** more, leaving **25** Xbox import references unresolved.
  Xbox GPU page tables, tile mappings, coherency and creation flags remain
  unsupported. Import linking is not proof of runtime correctness. The mapper applies final memory protection
  to **60** sections, registered **304274** x64 unwind function entries,
  installed **3** current-thread TLS templates (5776 bytes) and identified
  **2** callbacks (deferred, not executed). Proper other-thread TLS initialization,
  loader callbacks, Xbox services and GPU translation remain to be implemented.
  This does not imply that runtime behavior is correct.

## Native layer roadmap, in dependency order

1. **PE parser & mapper (implemented):** robust x64 PE header/section/import/delay-import/export
   inspection, data-only native image mapping, base relocation handling, PE section
   protection and x64 exception-function table registration, report generation.
2. **ABI import bridge (partly implemented):** precise import-blocker attribution
   and read-only startup-readiness report implemented. Host API-set import patching,
   recursive title-local DLL mapping, export forwarding (independent MSVC fixture
   verified), x64 unwind registration and partial current-thread TLS, without
   DLL initialization. Remaining: complete Xbox runtime HLE, TLS callbacks,
   other-thread TLS, process environment and platform handle semantics.
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
   [GPU_TILES.md](GPU_TILES.md)). Remaining: Xbox GPU page-table APIs and
   coherent CPU/GPU physical memory, console D3D12/XG methods, descriptors,
   RDNA2 shader decoding, texture swizzles and actual frame presentation.
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
`gpu-tile-map` verifies native D3D12 64 KiB tile residency and GPU physical
aliasing across segmented 16 GiB guest buffers (see [GPU_TILES.md](GPU_TILES.md)).
The gpu-texture-layout command verifies block-compressed, mipmapped Windows
GPU textures and an original reversible synthetic microtile layout.
See [GPU_VIRTUAL_MEMORY.md](GPU_VIRTUAL_MEMORY.md).
All GPU modes use only independently generated test data.
See [GPU_BACKEND.md](GPU_BACKEND.md) and [MEMORY_BACKEND.md](MEMORY_BACKEND.md).

Verified behavior should always be updated here based on concrete experiments.
