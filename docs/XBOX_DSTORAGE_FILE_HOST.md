# Fixture-rooted DirectStorage file COM adapter

**State: generated-file host adapter works; native game execution remains blocked.**
This original C++ implementation introduces a reference-counted file COM object
for the guest-observed IDStorageFileX IID `43704f9f-27c6-4a9f-8815-d16527894df4`.
The factory `OpenFile` callsite at vtable `+0x20` was previously confirmed by
read-only inspection. The provisional file-object method order used here is
`Close` at `+0x18`, then `GetHandle` at `+0x20`; Xbox title callsites for these
methods have **not** yet been confirmed, so this is not a fully verified ABI.

Microsoft's public Xbox GDK documentation describes [OpenFile](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragefactoryx/methods/idstoragefactoryx_openfile),
[IDStorageFileX](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragefilex/idstoragefilex),
[Close](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragefilex/methods/idstoragefilex_close), and
[GetHandle](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragefilex/methods/idstoragefilex_gethandle).

## Explicit limits and security

- **Disabled by default**. In normal readiness/title inspection the factory
  still returns `E_NOTIMPL` for file opens; no console/game filesystem is mounted.
- The `storage-file-fixture` explicitly configures a directory containing only
  generated test data. The trusted root must be an existing, non-reparse directory.
- A guest-style filename must be a single ASCII leaf of at most 96 characters.
  Traversal, drive/ADS syntax, reserved device names and other unsafe characters
  are refused. Read-only CreateFileW excludes file/directory reparse targets.
- Each successful `OpenFile` owns a real read-only Windows file handle. The
  returned `GetHandle` value is a duplicated Windows handle that the caller
  closes independently, even after the COM object has been explicitly closed.
- Separate file-object staging leases forbid `SetStagingBufferSize` until the
  last COM reference is released, including if Close was called earlier.
- A new **host-only** `reopen_owned_dstorage_file_for_read` service checks
  a synchronized registry of live ScarlettVI-owned file objects *before*
  dereferencing any candidate pointer, then pins its reference count.
  It calls Win32 `ReOpenFile` to obtain a separately positioned read handle,
  unlike the documented `GetHandle` duplication, and transfers ownership
  to the fixture-only queue. Unknown and closed file objects fail safely.
  This allows already-submitted host reads to survive closing the source
  file COM object without retaining arbitrary game memory.
- The API is not yet wired to an Xbox virtual filesystem, guest queue
  `DSTORAGE_REQUEST`, decompression engine, destination buffers or asset streaming.

## Regression commands

```powershell
.\build\x64\Release\ScarlettVI.exe storage-file-fixture
.\build\x64\Release\ScarlettVI.exe storage-factory-fixture
.\build\x64\Release\ScarlettVI.exe storage-status-fixture
.\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

The file and [queue COM fixture](XBOX_DSTORAGE_QUEUE_COM.md) verify
real generated-file reads, source-object lifetime and independent handle
positions without interpreting Xbox `DSTORAGE_REQUEST` structures.
The new fixture verifies actual generated-file reads, raw factory OpenFile
vtable dispatch, exact COM IID, independent duplicated handles, error propagation,
missing and malformed paths, reference lifecycle, staging interlocks and
parallel opens. The real game binary is only inspected in the separate
readiness command: no title code is executed and no game data is changed.

**Remaining blocker:** eight Xbox import references (CHAD 5, XAPU 1, XG 2),
plus verified guest queue, filesystem, graphics, audio and process lifecycle.
