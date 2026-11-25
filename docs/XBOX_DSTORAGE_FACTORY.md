# Partial Xbox DirectStorage factory and real host staging

**Status: guest-facing COM factory partly implemented, but no Xbox DirectStorage queue, asset reads, decompression or async completions.** The import XCRDAPI.dll!DStorageGetFactory is linked, with a separately labeled partial count. TITLE EXECUTION: BLOCKED remains enforced.

## Independently verified caller and public ABI

Read-only inspection of the user-provided Xbox PE found one direct call to the DStorageGetFactory thunk at **0x141D8E4F0**. The thunk **0x145F3FF80** resolves to IAT **0x146793FB8**. The caller sets RCX to an interface GUID in read-only data and RDX to a writable factory-pointer slot. The observed GUID was **62959d11-d45f-4884-a0a3-f7862ea243ae**. It checks that a valid pointer and successful HRESULT were received, then immediately calls its returned object's **vtable +0x40** with EDX=**0x02000000** (32 MiB).

Microsoft's public [DStorageGetFactory documentation](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/functions/dstoragegetfactory) confirms the HRESULT / REFIID / void** ABI and static factory. The [IDStorageFactoryX methods](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragefactoryx/idstoragefactoryx) list IUnknown plus six base methods, placing SetStagingBufferSize in slot 8 (+0x40). [SetStagingBufferSize](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragefactoryx/methods/idstoragefactoryx_setstagingbuffersize) takes one UINT32 byte count and may only be changed when no queue/file objects exist. The [public DirectStorage overview](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/console/storage/directstorage/directstorage-overview) lists valid sizes 0,16,20,24,28,32 MiB and a 32-MiB default.

These values were independently researched from public docs and **data-only disassembly**. No console SDK header, protected title instruction bytes, game assets or third-party GPL source were copied. Windows desktop IDStorageFactory is not substituted for the Xbox COM vtable.

## Original partial implementation

Sources: src/storage/dstorage_factory_host.h, src/storage/dstorage_factory_host.cpp, src/storage/dstorage_factory_host_test.cpp.

- Exact guest-observed IID is recognized, along with IUnknown for QueryInterface. Unknown IIDs fail E_NOINTERFACE. The original host factory is process-static and maintains synchronized COM reference counts.
- The first successful factory lookup obtains **real, zeroed 32-MiB host staging storage**. Page touches are verified across the full allocation, not merely accounted as a number.
- SetStagingBufferSize supports only the six documented sizes, preparing a new owned host allocation before atomically installing it. Invalid sizes fail E_INVALIDARG, allocation errors fail E_OUTOFMEMORY, preserving the previously installed buffer.
- CreateQueue, CreateQueue1, CreateStatusArray and OpenFile all fail **E_NOTIMPL** with null output pointers: no fake-success Xbox operations, no PC DirectStorage pass-through and no proprietary asset access.
- SetCpuAffinity returns E_NOTIMPL, and console SetDebugFlags has no claimed guest effect. GetStats is void and leaves unknown-sized guest output untouched, rather than inventing a console statistics structure.
- Staging storage and COM reference changes are synchronized; queues and files do not yet exist, so a future implementation must add the publicly required no-resize-while-live rule.

**Limitations:** This is not an NVMe driver, Xbox DirectStorage request queue, file-system mount, asynchronous completion service, decryption/decompression engine, or Xbox memory partition. It handles only the observed factory startup subset; the other methods explicitly remain unimplemented.

## Executable regression

    .\build\x64\Release\ScarlettVI.exe storage-factory-fixture
    .\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'

The generated host-only fixture exercises the exact exported function pointer, raw COM **vtable slot +0x40**, singleton and IUnknown identity, unknown-IID rejection, supported staging sizes, invalid-size rollback, real 64-KiB page writes/readback, E_NOTIMPL for queue/files, no output buffer writes for unknown-sized stats, and **4 workers ×48 GetFactory/reconfigure/Release calls**. No title code executes.

## Readiness change

| Diagnostic | Previous | Current |
| --- | ---: | ---: |
| Xbox unresolved function references | 9 | **8** |
| Unresolved XCRDAPI.dll imports | 1 | **0** |
| Partial DirectStorage factory binding | 0 | **1** |
| Functional Xbox asset streaming | No | **No** |
| Original game boot | BLOCKED | **BLOCKED** |

The current eight unresolved Xbox symbols comprise CHAD DSP five, XApuEnqueueCommand one, and XG graphics two. More importantly, functional Xbox file/queue/request and GPU/console process systems remain missing. The readiness report names the binding **DirectStorage factory partial=1**; it is not a bootability claim.
