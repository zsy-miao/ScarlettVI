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
| `chad.dll` | 0 | Five [partial host CHAD links](CHAD_HOST_CLIENT.md): two real owned client operations, one tagged XMem CPU FIR-only submit and two **error-only** stream activation operations. Guest Xbox DSP remains unsupported |
| `xapu.dll` | 0 | Four partial connection/result-queue bindings; `XApuEnqueueCommand` now links to a [strict XMem host FIR command bridge](XAPU_XMEM_GUEST_FIR.md) that returns `E_NOTIMPL` for real unknown Xbox commands |
| `xmem.dll` | 0 | One **partial** `XMemTransferMemory` host quota binding; only title-observed direction and host XMemAlloc budget admission are implemented |
| `xg_xs.dll` | 0 | Both symbols now link to [explicit error-only XG functions](XG_IMPORT_FAILURE_BRIDGE.md); **both remain functionally unsupported**. Host Morton GPU detiling is NOT Xbox swizzle |
| `XCRDAPI.dll` | 0 | [Partial Xbox DirectStorage factory](XBOX_DSTORAGE_FACTORY.md) binds one symbol and real staging. The [host-only async Win32 queue](XBOX_DSTORAGE_HOST_ASYNC.md), [enqueue/submit scheduler](XBOX_DSTORAGE_SUBMIT_GATE.md) and [guest-IID status arrays](XBOX_DSTORAGE_STATUS_ARRAY.md) now work; an [explicitly fixture-rooted file COM adapter](XBOX_DSTORAGE_FILE_HOST.md) performs real read-only opens, and a [fixture-rooted queue COM prototype](XBOX_DSTORAGE_QUEUE_COM.md) processes real host barriers; general guest asset access remains unsupported |
| **Total** | **0** | **Zero missing IAT addresses is NOT functional Xbox support.** The experimental audio and graphics interfaces remain incomplete and native execution is BLOCKED |

Measured working address bindings:
- **830/830** Windows import references across the game EXE and local DLLs
- **315/315** title-local DLL import references across 7 local DLL images
- **15** experimental Xbox XMem reference bindings, including independently
  analysed `XMemVirtualQuery` in `XboxBvhBuilder.dll` and the main title's
  `XMemMapPhysicalPagesScatter` caller pattern, plus the host-only
  [`XMemIsToolsMemAvailable` FALSE capability policy](XMEM_TOOLS_CAPABILITY.md)
  (it does **not** provide an Xbox tools-memory partition), plus the
  [XMem working-set safe failure path](XMEM_WORKING_SET_FAILURE.md) which
  returns FALSE without writing to an unknown-sized output buffer;
  **neither is full Xbox memory-budget emulation**. The additional
  [partial title-to-system host quota transfer](XMEM_TITLE_SYSTEM_TRANSFER.md)
  enforces actual XMemAlloc heap admission across transfers and frees,
  but does not migrate Xbox physical pages, GPU allocations or sparse VM.
  Readiness reports `XMem=15 (error-only=1, budget-partial=1)` and
  separate JSON summary counters for those two limited bindings.
- **8** experimental [host APU heap/memory bindings](APU_HOST_MEMORY.md),
  including the [reversible owned 32-bit APU address translator](APU_ADDRESS_TRANSLATION.md)
  and [bounded family-4 SHAPE context buffers](APU_SHAPE_CONTEXTS.md).
  Real aligned CPU allocations and quotas are tested; arbitrary game memory,
  physical APU RAM, actual SHAPE processing and audio command/result queues
  remain unsupported
- **5 CHAD imports linked**, classified separately as
  `CHAD client partial=2, CHAD host FIR-only=1, CHAD error-only=2`.
  The [bounded client bridge](CHAD_HOST_CLIENT.md) owns real host handles
  and executes the original CPU FIR on strictly tagged, XMem-owned test
  packets, while actual Xbox DSP stream activation/deactivation always
  returns `E_NOTIMPL`. No Xbox stream processing, guest packet decoding,
  console codec, audio playback, or DSP callbacks are implemented
- **4** [partial XAPU connection/result queue bindings](XAPU_QUEUE_HOST.md):
  bounded host-owned connections, result count and FIFO dequeue are real.
  The [host-only FIR-to-XAPU integration](XAPU_DSP_INTEGRATION.md) now
  executes actual generated audio commands and records checked output
  receipts. The guest `XApuEnqueueCommand` symbol now also links to a
  [strict versioned XMem host FIR command](XAPU_XMEM_GUEST_FIR.md) and
  performs real generated audio processing with safe CPU-page ownership;
  **real Xbox commands return `E_NOTIMPL`**. Readiness reports
  `XAPU queue partial=4, XAPU XMem host FIR-only=1` separately from
  functional Xbox DSP services
- **1** [partial AcpHal guest factory binding](APU_HAL_FACTORY.md): the
  factory allocates a correctly shaped host object and real bounded ring
  buffers, but the unverified `+0x10` DSP configuration method returns
  `E_NOTIMPL`; readiness separately reports `HAL factory partial=1`
- **2** Xbox XG imports linked to [explicit `E_NOTIMPL` handlers](XG_IMPORT_FAILURE_BRIDGE.md): **no** XG texture-computer object or swizzle output exists. They are separately reported as `XG texture imports error-only=2` rather than functional graphics coverage
- **2** Xbox D3D12 root-signature serialization references bridged to desktop
- **1** experimental `D3D12XboxCreateDevice` import linked to a verified
  Windows `ID3D12Device11` host COM factory; Xbox-only flags ignored
- **1** [partial DirectStorage factory binding](XBOX_DSTORAGE_FACTORY.md):
  exact guest-observed IID, process-static COM object, real 32-MiB owned
  staging memory and validated vtable `+0x40` sizing. The separate
  [host-only async file queue](XBOX_DSTORAGE_HOST_ASYNC.md) now performs
  genuine bounded Win32 reads, exact FIFO completions, factory staging
  leases and a [hold-until-submit scheduler](XBOX_DSTORAGE_SUBMIT_GATE.md).
  The corrected factory vtable `+0x28` now creates a genuine
  [guest-IID status array](XBOX_DSTORAGE_STATUS_ARRAY.md) whose result
  slots track actual host worker barriers. `OpenFile` at `+0x20` now has a
  [read-only fixture-rooted file implementation](XBOX_DSTORAGE_FILE_HOST.md),
  disabled by default. `CreateQueue1` now also returns a
  [fixture-only queue COM object](XBOX_DSTORAGE_QUEUE_COM.md) for verified
  generated reads and status barriers, including safe independently
  reopened handles from fixture-rooted file COM objects. The
  [file COM adapter](XBOX_DSTORAGE_FILE_HOST.md) now supports validated
  nested paths with final-handle root containment, and the queue fixture
  reads actual generated nested files via the owned file object. Normal
  Xbox title paths, general guest memory and full Xbox queues remain
  blocked; a [generated-buffer request subset](XBOX_DSTORAGE_REQUEST_ABI.md)
  now tests real file-to-host-memory copies through queue vtable `+0x18`.
  A separate [bounded multi-page pinned XMem CPU-alias bridge](XBOX_DSTORAGE_CPU_ALIAS.md)
  verifies three-page mapped writeback, all-or-nothing page pin admission,
  rollback and unmap interlocks. A request structure stored inside
  verified XMem-managed CPU mappings can now be snapshotted without a
  host-only permit, but general title heap/stack memory, guest VFS and
  Xbox physical page-array destinations remain unavailable.
- **1** experimental `D3D12ConfigureVirtualMemoryX` import linked to a
  [validated 16/24 GiB host GPU VA reservation bridge](GPU_VIRTUAL_MEMORY.md);
  Xbox page tables, tiling and CPU/GPU coherency remain unsupported.
  Native host [D3D12 tile backing](GPU_TILES.md) and the new
  [GPU-executed host texture detiler](GPU_TEXTURE_DETILE.md) are tested
  separately; neither binds additional Xbox imports or reproduces XG
  console texture swizzling

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

Even with **all currently observed import addresses now linked**, startup
remains blocked because most Xbox-specific services are partial or error-only,
and the following runtime requirements are still unmet:

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
