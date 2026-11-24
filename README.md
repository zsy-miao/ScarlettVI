<div align="center">

<img src="assets/scarlettvi-logo.png" alt="ScarlettVI — neon sunset wordmark" width="520" />

# ScarlettVI

**A custom, C++20 native Xbox Series X|S compatibility layer for Windows.**

[Architecture](docs/ARCHITECTURE.md) · [GPU](docs/GPU_BACKEND.md) · [Memory](docs/MEMORY_BACKEND.md) · [Assets](docs/ASSET_FORMATS.md) · [Source](src)

</div>

---

ScarlettVI is an original **Visual Studio 2026 / C++** research and engineering
project investigating how to run Xbox Series X|S x64 game executables on a
Windows PC. Rather than CPU emulation, the goal is to build a dedicated PE
loader, runtime ABI bridge, Xbox operating-system compatibility layer and
graphics translation pipeline.

The primary compatibility target is an Xbox Series build of Grand Theft Auto VI.
This repository **does not include game files or Xbox proprietary components**.
You must supply your own legitimately obtained software.

## Status

**Early development — not bootable or playable yet.**

The native x64 PE mapper now supports recursive DLL dependencies, forwarded
exports, host import routing, original XMem HLE, section protections, initial
TLS and x64 unwind registration. On the current target, it binds 341/341
executable Windows imports, 489/489 nested Windows imports and 315/315
title-local exports. 29 Xbox-specific import references remain unresolved.

The original experimental 64 KiB physical memory manager supports actual
host memory aliasing through Windows placeholders, with a `VirtualFree` bridge.
The Windows D3D12 backend successfully initializes a real graphics queue,
command list, upload resource, fence and root signature on an RTX 5060 Ti.
These are independently verified host capabilities; the **Xbox-specific
graphics device/command ABI and shader translation are still missing**.

The `inspect`, `map` and `archives` modes are read-only with respect to the
game and never execute its entry point or initializers.

## Build

- Windows 10/11 x64
- Visual Studio 2026 with the C++ desktop workload (MSVC v145)
- Open `ScarlettVI.sln`, choose `x64` + `Release`, then Build Solution.

Or build from a Developer PowerShell:

```powershell
msbuild ScarlettVI.sln /p:Configuration=Release /p:Platform=x64
```

## Inspect a title (read-only)

```powershell
.\build\x64\Release\ScarlettVI.exe inspect "D:\Games\Title\title.exe" --json "reports\imports.json"
.\build\x64\Release\ScarlettVI.exe map "D:\Games\Title\title.exe"
.\build\x64\Release\ScarlettVI.exe archives "D:\Games\Title"
.\build\x64\Release\ScarlettVI.exe gpu
.\build\x64\Release\ScarlettVI.exe memory
```

The `map` mode maps the image into process memory, applies relocations,
links Windows and available title-local imports, prepares TLS and registers
unwind metadata. **It does not execute title code.** The `archives` command
reads RPF9 container metadata only (first 64 bytes of each top-level archive),
not its encrypted directory or contents.

See [architecture](docs/ARCHITECTURE.md) and
[asset format research](docs/ASSET_FORMATS.md) for implementation details.

## Independence and acknowledgements

The project is independently authored. We reviewed the public architecture
and status descriptions of [XCL](https://github.com/xcl-dev/xcl) and
[VIctory](https://github.com/Marvisak/VIctory) for general context, without
copying their code, binary libraries or title files.

## Legal

ScarlettVI does not provide a copy of Grand Theft Auto VI, console firmware,
restricted SDK headers, keys or proprietary DLLs. Game and console brands
remain trademarks of their respective owners. Do not distribute game assets
through issues or pull requests.
