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
- **Verified input boundary:** guest `OpenFile` path pointers must be
  backed either by mapped XMem CPU aliases or by an actual live
  `XMemAlloc`/`XMemAllocDefault` host allocation. The separate
  [heap snapshot bridge](XMEM_HEAP_SNAPSHOT.md) verifies the entire
  UTF-16 path within one live recorded allocation and holds its
  registry mutex across copying, including concurrent `XMemFree`.
  `PhysicalMemory::snapshot_mapped_alias_utf16` still supports paths
  crossing 64KiB physical aliases. Both methods cap the copy at 240
  UTF-16 characters plus NUL, reject unknown native/guest pointers
  **without dereferencing them**, and return an immutable host copy.
- For original C++ tests using ordinary string literals, a **host-only
  per-thread, one-shot permit** supplies a trusted immutable path
  snapshot. `register_host_dstorage_fixture_path` and
  `clear_host_dstorage_fixture_path` are internal C++ functions, not
  exported to guest code. No permanent raw-pointer whitelist exists.
- The `storage-file-fixture` explicitly configures a directory containing only
  generated test data. The trusted root must be an existing, non-reparse directory.
- A generated relative fixture path can now contain **1 to 8 ASCII components**
  (96 characters per component, 240 total). Both forward and backward
  separators are accepted. Empty/doubled/trailing separators, dot segments,
  absolute/drive/UNC paths, alternate streams, reserved DOS device names
  (including COM0/LPT0), and unsafe punctuation are rejected.
- The trusted root is opened as a Windows directory handle, validated as
  non-reparse and normalized using `GetFinalPathNameByHandleW`. After
  `CreateFileW`, the **opened file handle's resolved path** must be strictly
  inside that root. This also rejects intermediate symlink/junction escapes.
  Directory and reparse targets are rejected as final file objects.
- The separate host-only direct async queue intentionally remains
  **single-leaf only**. Nested files reach its worker through the validated
  `IDStorageFileX` object and independent `ReOpenFile` handles; there is no
  second permissive direct-read path parser. No actual Xbox volume is mounted.
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
- The factory file objects now feed a restricted, real
  [guest-IID `DSTORAGE_REQUEST` queue](XBOX_DSTORAGE_REQUEST_ABI.md)
  using registered or validated XMem-owned request snapshots and pinned
  CPU physical-alias destinations. Actual Xbox volume semantics,
  decompression, guest-heap destinations and title asset streaming
  remain unimplemented.

## Host-only read-only virtual mounts

The generated-fixture file COM adapter now supports a bounded logical
mount table. After the default fixture root has been configured by the
host, `configure_dstorage_fixture_mount(name, trusted_root)` may install
up to **four** additional separately validated read-only directory
roots. Names are 1–16 ASCII characters, begin with a letter and may
contain letters, digits, underscores or hyphens. Names are
case-insensitive. Mount registration and removal are host-only
functions; there is no guest Xbox import that can configure them.

For example, a generated UTF-16 XMem path
`pack0:/bundle/asset.bin` selects the host-registered `pack0` root.
Both `/` and `\\` after the colon are accepted, and a path without a
prefix continues to use the default fixture root. `PACK0` and
`pack0` select the same mount, but a drive-like or unknown prefix is
never treated as a host drive. An unknown mount returns `E_NOTIMPL`,
not a fallback to the default root.

Mount targets must exist and be real, non-reparse directories.
Each mount's resolved canonical path is saved. The previously
implemented per-component checks and final **opened-handle containment**
apply to the selected mount independently, so traversals, path suffix
tricks and intermediate symlink/junction escapes cannot cross its
boundary. Duplicate names are rejected, and exceeding four mounts
fails closed. `clear_dstorage_file_fixture_root()` atomically clears
the base root and the whole mount registry for future opens; already
open COM files retain their independently owned read-only handles
until their last reference is released.

The file fixture creates a second generated directory **outside** the
default root with different contents under the same filename. It
verifies exact data isolation, mount-name case handling, prefix syntax,
unknown mounts, capacity limits, XMem-sourced prefixed UTF-16 strings,
mount reset, retained-handle lifetime, and optional reparse-root
rejection. The queue fixture also calls COM `OpenFile` using
`pack0:/generated.bin` stored across XMem CPU pages, releases the
path mapping, then completes a real queued read using the selected
mount's file object.

This is **not** the real Xbox VFS: no title-specific mount names,
game content directories, package metadata, save writes, game assets,
license checks, or executable title access have been enabled.

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
The expanded fixture verifies exact reads from generated single-leaf and
nested `packs/level1/sample.bin` files, including mixed separators,
traversal, DOS device names, overlong paths, and nine-component paths.
It now also opens the nested file via the **actual guest-named COM
`OpenFile` slot using a two-page mapped XMem UTF-16 source path**,
then safely wipes/reuses the path memory while preserving the independent
open file. Additional cases exercise regular and page-backed XMemAlloc
heap paths, and confirm that an opened file is still valid after the
heap path is freed. It checks invalid/unregistered host pointers, absent second
source page, unterminated 241-unit strings, short paths that end before
an unmapped page, failure output immutability and zero leaked XMem
frames/pins.
Where Windows allows test-created directory symbolic links, it also checks
that a nested link leading outside the root is refused. The original raw
COM dispatch, IID, independent handles, staging leases, file lifecycle,
and parallel-open tests continue to pass. The queue fixture opens
`packs/batch/generated.bin` using a **cross-page mapped XMem UTF-16 path**
through the real file COM vtable, releases that input mapping immediately,
and still performs exact readback through queue Submit and status markers.

**Remaining blockers:** native Xbox VFS mount semantics and asset paths,
real GPU texture/command behavior, audio streams, full memory translation,
and process initialization. The import address count is zero, but many
bindings are partial or error-only. Title execution is still BLOCKED.
No protected game files are modified or executed by these fixtures.
