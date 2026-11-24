<div align="center">

<img src="assets/scarlettvi-logo.png" alt="ScarlettVI — neon sunset wordmark" width="520" />

# ScarlettVI

**A custom, C++20 native Xbox Series X|S compatibility layer for Windows.**

[Architecture](docs/ARCHITECTURE.md) · [Source](src) · [Build](#build)

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

**Early development — not bootable or playable yet.** The working milestones include a native x64 PE inspector, data-only image mapper,
Windows import-address-table binding and title-local DLL mapping/export linking.
The tools enumerate imports, exports, sections and relocations and produce JSON
dependency reports without executing game code. This establishes facts about the target before enabling
any execution.

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
```

The `map` mode only maps the image into process memory and handles relocations;
**it does not run the game**. See [ARCHITECTURE.md](docs/ARCHITECTURE.md)
for the implementation plan and current constraints.

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
