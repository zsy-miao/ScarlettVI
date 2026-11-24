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
  `chad.dll`. Some are not present in the supplied title directory.

## Native layer roadmap, in dependency order

1. **PE parser & mapper (implemented):** robust x64 PE header/section/import/delay-import
   inspection, data-only native image mapping, base relocation handling, report generation.
2. **ABI import bridge:** signature-correct API routing, host API sets and title-local DLL
   dependency loading, independent Xbox runtime high-level emulation, TLS callbacks,
   exception registration, thread process environment and platform handles.
3. **Console filesystem and runtime:** drive virtualization, package and saved-data paths,
   game-services and async job queues, heaps, virtual-memory allocation semantics,
   XGameRuntime and XMemory behavior.
4. **GPU:** Xbox D3D12/XG interfaces and resource heaps, GPU VA, descriptors, command
   streams, RDNA2 shader decompilation/recompilation to DXIL, format/texture conversion.
5. **Title features:** RPF streaming, BCPack where applicable, shaders and render-target
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
`ScarlettVI.exe map <path-to-title-exe>` performs a data-only relocation probe.

Verified behavior should always be updated here based on concrete experiments.
