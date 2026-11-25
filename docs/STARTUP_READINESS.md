# ScarlettVI startup readiness

**Status: BLOCKED.** This document records import binding and process
initialization blockers; it does not claim that the target can boot.

## Reproduce the inventory without executing the title

From the ScarlettVI project folder:

```powershell
.\build\x64\Release\ScarlettVI.exe readiness "..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe" --json "reports\readiness.json"
```

The command validates the PE executable and recursively maps its bundled DLL
images into **data-only** private address space. It resolves Windows exports,
existing local DLL exports and the implemented experimental console bridges,
then registers x64 unwinding metadata and current-thread TLS templates.
**No entry point, DLL initializer or TLS callback is invoked.**

The structured output uses `schema: scarlettvi-readiness-v1` and contains
`pending_imports` entries, with:
- `owner`: the importing PE filename (EXE or bundled DLL)
- `module`: the missing dependency library
- `symbol`: the exact imported function name or ordinal
- `iat_rva`: the exact import-address-table slot relative to the owner
- `kind`: xbox-hle, unknown-module, missing-host-export, missing-title-export
- `delay_loaded`: whether the source PE declared a delay-load descriptor

JSON reports are placed under `reports/` which is Git-ignored. Never commit
real game binaries, shader assets, extracted archive contents or title data.

## Verified import inventory (supplied game build)

| Dependency | Unresolved import references | Workstream |
| --- | ---: | --- |
| `AcpHal.dll` | 9 | Audio processing, heap and shape contexts |
| `chad.dll` | 5 | DSP connection, activation and command dispatch |
| `xapu.dll` | 5 | APU command/results and connection lifecycle |
| `xmem.dll` | 3 | Working sets, tools memory, memory-budget transfer (caller patterns analysed; Xbox ABI still unresolved) |
| `xg_xs.dll` | 2 | Xbox texture computer/swizzle ABI (host layouts tested separately) |
| `XCRDAPI.dll` | 1 | Xbox DirectStorage factory |
| **Total** | **25** | |

Measured working address bindings:
- **830/830** Windows import references across the game EXE and local DLLs
- **315/315** title-local DLL import references across 7 local DLL images
- **12** experimental Xbox XMem reference bindings, including independently
  analysed `XMemVirtualQuery` in `XboxBvhBuilder.dll` and the main title's
  `XMemMapPhysicalPagesScatter` caller pattern
- **2** Xbox D3D12 root-signature serialization references bridged to desktop
- **1** experimental `D3D12XboxCreateDevice` import linked to a verified
  Windows `ID3D12Device11` host COM factory; Xbox-only flags ignored
- **1** experimental `D3D12ConfigureVirtualMemoryX` import linked to a
  [validated 16/24 GiB host GPU VA reservation bridge](GPU_VIRTUAL_MEMORY.md);
  Xbox page tables, tiling and CPU/GPU coherency remain unsupported.
  Native host [D3D12 tile backing](GPU_TILES.md) is tested separately;
  it does not bind additional Xbox imports

Counted address bindings do **not** guarantee ABI equivalence, correct
component lifetime, graphics semantics or startup success.

## Mandatory runtime milestones beyond imports

Even if all 25 outstanding references gain function addresses, startup
remains blocked until:

1. Xbox D3D12_XS and XG COM/resource/command ABI and native shader translation
2. Xbox memory heap and graphics virtual-address semantics
3. Xbox audio/DSP and storage service behavior
4. DLL process attach, TLS callback ordering and static TLS for new threads
5. Console virtual filesystem and process environment
6. Controlled title exception handling and fault diagnostics

**Never infer execution readiness from a count of zero.** The readiness
command deliberately leaves `title_execution_ready=false` while these gates
remain unimplemented. The executable and bundled libraries are currently
inspected only; no genuine gameplay or frames have been produced.

This is a research/compatibility project using original C++20 and the public
Windows SDK. No restricted Xbox GDK materials are distributed.
