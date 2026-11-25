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
| `AcpHal.dll` | 0 | Eight functional host APU memory/SHAPE bindings and one partial HAL factory; DSP configuration remains unsupported |
| `chad.dll` | 5 | Connection, activation and command dispatch are not bound; original CPU FIR DSP processing is independently tested, but the guest command/stream ABI is unverified |
| `xapu.dll` | 1 | `XApuEnqueueCommand` remains unimplemented; four partial host connection/result-queue bindings now linked |
| `xmem.dll` | 1 | `XMemTransferMemory` memory-budget transfer semantics remain unknown |
| `xg_xs.dll` | 2 | Xbox texture computer/swizzle ABI (host layouts tested separately) |
| `XCRDAPI.dll` | 1 | Xbox DirectStorage factory |
| **Total** | **10** | |

Measured working address bindings:
- **830/830** Windows import references across the game EXE and local DLLs
- **315/315** title-local DLL import references across 7 local DLL images
- **14** experimental Xbox XMem reference bindings, including independently
  analysed `XMemVirtualQuery` in `XboxBvhBuilder.dll` and the main title's
  `XMemMapPhysicalPagesScatter` caller pattern, plus the host-only
  [`XMemIsToolsMemAvailable` FALSE capability policy](XMEM_TOOLS_CAPABILITY.md)
  (it does **not** provide an Xbox tools-memory partition), plus the
  [XMem working-set safe failure path](XMEM_WORKING_SET_FAILURE.md) which
  returns FALSE without writing to an unknown-sized output buffer;
  **neither is full Xbox memory-budget emulation**. Readiness explicitly
  reports `XMem=14 (error-only=1)` and the JSON summary includes
  `experimental_xmem_error_only_bound` so failure-only bindings are not
  confused with functioning console services.
- **8** experimental [host APU heap/memory bindings](APU_HOST_MEMORY.md),
  including the [reversible owned 32-bit APU address translator](APU_ADDRESS_TRANSLATION.md)
  and [bounded family-4 SHAPE context buffers](APU_SHAPE_CONTEXTS.md).
  Real aligned CPU allocations and quotas are tested; arbitrary game memory,
  physical APU RAM, actual SHAPE processing and audio command/result queues
  remain unsupported
- **0 bound CHAD imports**; the [original host FIR convolution backend](DSP_CPU_CONVOLUTION.md)
  now performs real generated multichannel signal processing. This is a
  numerical audio primitive only: no guest DSP queue, packet handling,
  codec, sound output or CHAD lifecycle has been linked
- **4** [partial XAPU connection/result queue bindings](XAPU_QUEUE_HOST.md):
  bounded host-owned connections, result count and FIFO dequeue are real.
  The [host-only FIR-to-XAPU integration](XAPU_DSP_INTEGRATION.md) now
  executes actual generated audio commands and records checked output
  receipts, but the guest `XApuEnqueueCommand` remains unbound. Readiness
  reports `XAPU queue partial=4` separately from full services
- **1** [partial AcpHal guest factory binding](APU_HAL_FACTORY.md): the
  factory allocates a correctly shaped host object and real bounded ring
  buffers, but the unverified `+0x10` DSP configuration method returns
  `E_NOTIMPL`; readiness separately reports `HAL factory partial=1`
- **2** Xbox D3D12 root-signature serialization references bridged to desktop
- **1** experimental `D3D12XboxCreateDevice` import linked to a verified
  Windows `ID3D12Device11` host COM factory; Xbox-only flags ignored
- **1** experimental `D3D12ConfigureVirtualMemoryX` import linked to a
  [validated 16/24 GiB host GPU VA reservation bridge](GPU_VIRTUAL_MEMORY.md);
  Xbox page tables, tiling and CPU/GPU coherency remain unsupported.
  Native host [D3D12 tile backing](GPU_TILES.md) is tested separately;
  it does not bind additional Xbox imports

A [public compatibility-project comparison](PUBLIC_COMPATIBILITY_RESEARCH.md)
identifies additional implementation work and licenses to review. The
working-set import is bound to an explicit **unsupported** response, not
working Xbox statistics. Counted address bindings do **not** guarantee ABI equivalence, correct
component lifetime, graphics semantics or startup success.

## Mandatory runtime milestones beyond imports

The [generated PE64 entrypoint fixture](SYNTHETIC_PE_ENTRY.md) now passes
an actual known-code entrypoint invocation through ScarlettVI's mapper,
with one real Windows API import, forced relocations and current-thread TLS.
The [generated DLL fixture](SYNTHETIC_PE_ENTRY.md) also tests explicit
TLS callback / DllMain attach/detach order and private host TLS data for
managed worker threads. Only generated test callbacks are executed. Neither
fixture invokes the original title, any game DLL or their callbacks;
this is **not** proof of full guest loader lifecycle correctness.

Even if all 10 outstanding references gain function addresses, startup
remains blocked until:

1. Xbox D3D12_XS and XG COM/resource/command ABI and native shader translation
2. Xbox memory heap and graphics virtual-address semantics
3. Xbox audio/DSP and storage service behavior
4. Actual guest DLL dependency attach/detach and TLS callbacks, guest
   compiler static TLS in the TEB and all game-created threads
5. Console virtual filesystem and process environment
6. Controlled title exception handling and fault diagnostics

**Never infer execution readiness from a count of zero.** The readiness
command deliberately leaves `title_execution_ready=false` while these gates
remain unimplemented. The executable and bundled libraries are currently
inspected only; no genuine gameplay or frames have been produced.

This is a research/compatibility project using original C++20 and the public
Windows SDK. No restricted Xbox GDK materials are distributed.
