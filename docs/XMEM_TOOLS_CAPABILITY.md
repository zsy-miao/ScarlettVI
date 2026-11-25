# Desktop XMem tools-memory availability bridge

**Implementation scope:** This is an original, narrowly scoped Windows host capability policy for the named `xmem.dll!XMemIsToolsMemAvailable` import. It does **not** create an Xbox developer/tools-memory partition or establish the complete console XMem ABI.

## Policy and supported behavior

ScarlettVI has no Xbox tools-memory partition, console memory service, or Xbox device with which to consult such a partition. For the host-only case, the nullary `BOOL WINAPI XMemIsToolsMemAvailable(void)` bridge consistently returns `FALSE` (unavailable).

The implementation:

- Calls no console code and allocates or maps no memory.
- Does not mutate the host's Win32 thread last-error value or any XMem allocation registry.
- Is registered only under the exact named `xmem.dll` import via the existing `xmem::find_export` resolver. No fabricated ordinal exports.
- Is explicitly counted as an experimental desktop XMem link, not as independently validated native Xbox memory semantics.
- Does not enable title execution.

Microsoft publicly lists the export name, but its console-specific reference details are access-restricted. The nullary BOOL signature and FALSE-on-desktop behavior are a conservative host policy supported by independently published reimplementation research, **not an assertion that all console modes or game call paths behave identically**. In particular, this does not prove the game will continue when the query returns FALSE.

## Source-built verification

From the `ScarlettVI` project directory:

```powershell
.\build\x64\Release\ScarlettVI.exe xmem-tools-fixture
.\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

The `xmem-tools-fixture` test invokes the function through the same type of C function pointer that the import router installs in an IAT slot, repeats it 256 times, checks a preserved nonzero Win32 `GetLastError` sentinel, verifies exact-name routing, and ensures `XMemGetWorkingSetStatistics` / `XMemTransferMemory` remain unbound rather than being faked. The test uses only generated host-side calls, never executes the title's code or loads any proprietary DLL.

## Readiness impact on the inspected build

| Import reference family | Before | After |
| --- | ---: | ---: |
| `xmem.dll` pending | 3 | 2 |
| Total Xbox pending | 25 | 24 |
| Experimental XMem IAT links | 12 | 13 |
| Windows direct and nested imports | 830/830 | 830/830 |
| Title execution gate | BLOCKED | BLOCKED |

The two remaining XMem APIs are `XMemGetWorkingSetStatistics` and `XMemTransferMemory`. Neither is given an address: they require genuine selector/data-layout and memory-budget transfer contracts. Audio/DSP (19 references), graphics (2) and Xbox DirectStorage (1) also remain.

Microsoft's public Xbox and Windows `DStorageGetFactory` functions share an entry signature but return different interface families (`IDStorageFactoryX2` versus `IDStorageFactory`). Mapping the console import directly to the Windows export would not constitute an ABI adapter and is not done. See [startup readiness](STARTUP_READINESS.md).

The project remains an experimental compatibility research tool. No game executables/assets or proprietary SDK contents are copied or modified.

Public references:
- https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xmem/functions/xmemistoolsmemavailable
- https://learn.microsoft.com/en-us/xbox/gdk/docs/reference/system/dstorage/functions/dstoragegetfactory
- https://learn.microsoft.com/en-us/windows/win32/dstorage/dstorage/nf-dstorage-dstoragegetfactory
