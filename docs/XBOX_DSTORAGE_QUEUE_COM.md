# Fixture-rooted Xbox DirectStorage queue COM

**Status: partial, tested guest-IID COM queue bound to the existing host
DirectStorage scheduler; normal title execution remains blocked.**

## Verified evidence and compatibility assumptions

The original title was previously examined **without executing it**.
That read-only inspection found the factory `CreateQueue1` call at
`0x1428C95C7`, factory vtable `+0x50`, with guest queue IID
`3c02cb0c-c050-4195-99a1-a1ce94ba33c5`.

Microsoft's public Xbox GDK docs establish the [CreateQueue1 signature](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragefactoryx2/methods/idstoragefactoryx_createqueue1),
[queue descriptor fields](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_queue_desc1),
[options bitfield](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_queue_options),
[IDStorageQueueX methods](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragequeuex/idstoragequeuex),
[IDStorageQueueX2 extension](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragequeuex2/idstoragequeuex2),
[EnqueueStatus semantics](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragequeuex/methods/idstoragequeuex_enqueuestatus),
and [Submit](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragequeuex/methods/idstoragequeuex_submit).
No Xbox SDK files or proprietary source were copied.

**Important:** direct read-only title callsites now confirm **queue**
`EnqueueRequest +0x18` and `Close +0x40` independently. Other queue
virtual method offsets remain provisional, and the complete guest request
and destination-memory ABI is not implemented. See the
[request ABI evidence and generated-buffer test](XBOX_DSTORAGE_REQUEST_ABI.md).
Do not enable actual title asset loads on this partial evidence.

## What is implemented

- Factory `CreateQueue1` produces a real, reference-counted COM object
  for the observed queue IID **only** when a trusted directory with
  generated test files has explicitly been configured. Unconfigured
  production/readiness calls still fail `E_NOTIMPL` and clear output.
- The original 64-bit host declaration models the published descriptor:
  64-bit source type, 16-bit capacity, signed 8-bit priority, optional
  name pointer and 64-bit options flags. The name is never dereferenced.
  Our conservative host test accepts only file source, 64-entry capacity,
  priorities -1 through 2, and the `AutoSubmitDisabled` option bit.
  These are **fixture limits**, not a claim about Xbox maximum capacity.
- `EnqueueStatus` installs a real reference-owned status barrier, processed
  in FIFO order by the existing host worker. `Submit` releases the held
  batch. The queue supports both manual submission and the 32-request
  automatic threshold, consistent with the existing host-only scheduler.
- Internal-only test hooks can queue genuine Win32 reads of **generated
  host files**, either by validated leaf name or by a ScarlettVI-owned
  `IDStorageFileX` object. The file-object route now calls `ReOpenFile`
  for an independent read position and transfers that handle into the
  bounded worker. Already-admitted reads continue after source `Close`
  and final `Release`. No Xbox guest request buffer is interpreted.
- The status array reports S_OK, the first actual I/O error, and an
  independent reset at each barrier. The worker now auto-retires completed
  **unsupported guest request error tokens** (which have no payload
  consumer), preventing long-lived queues from exhausting 64 slots.
- Queue `EnqueueRequest +0x18` now supports a strictly registered,
  original host **generated-buffer** subset of the documented 64-byte
  `DSTORAGE_REQUEST` layout: ordinary uncompressed file sources and
  byte-for-byte asynchronous writeback into pinned, explicitly approved
  test buffers. The raw request is matched against a host-only permit
  registry **before any pointer is dereferenced**. Ordinary game requests
  still insert ordered **E_NOTIMPL failures**, never fake successful reads.
  `EnqueueSignal` still fails closed.
- The queue owns a manual-reset Windows error event. Unsupported calls
  signal the event and store a diagnostic HRESULT; it does not yet
  implement full Xbox error-record reporting for actual failed reads.
- `Close` stops outstanding work; COM object lifetime holds the staging
  lease until final Release. `Query`, `Query1` and
  `RetrieveErrorRecord` deliberately avoid writing unknown-sized Xbox
  output buffers. No GPU fence, guest event, cancellation tag ABI,
  title path, decompression engine or destination buffer is emulated.
- `EnqueueStatus` refuses unknown/freed status COM pointers by checking
  the address against a synchronized ScarlettVI-owned live object registry
  before any producer-side dereference, then retaining a safe reference.

## Executable verification

```powershell
.\build\x64\Release\ScarlettVI.exe storage-queue-fixture
.\build\x64\Release\ScarlettVI.exe storage-file-fixture
.\build\x64\Release\ScarlettVI.exe storage-status-fixture
.\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

The fixture covers factory raw vtable `+0x50`, queue raw vtable
`EnqueueRequest`/`EnqueueStatus`/`Submit`/`Close`/`GetErrorEvent`,
exact IID and IUnknown identity, invalid descriptors, unconfigured-root
rejection, bounded real generated-file readback, no I/O before manual
`Submit`, ordered failure markers, first-error aggregation and reset,
staging lease ownership, event signaling and unverified output safety.
It additionally tests a raw +0x18 `EnqueueRequest` with a registered
host-generated buffer, no write before Submit, successful exact writeback,
duplicate permit rejection, request-structure reuse and arbitrary pointer
rejection. The [read-only ABI probe](../scripts/check_dstorage_queue_abi.py)
checks four observed title callsites without executing game code.
Additional regression coverage checks two real `IDStorageFileX`-backed reads
with different offsets, unchanged file positions on the original handle,
close/release-before-submit ownership, rejection of foreign/closed COM
file pointers, and 160 unsupported requests across 20 successive batches.
Microsoft describes independent handles through
[`ReOpenFile`](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reopenfile).

**Remaining native startup blockers:** guest request packing/ownership,
guest filesystem and storage semantics, Xbox GPU resource/command ABI,
audio/XAPU/CHAD, process initialization and eight unresolved Xbox import
references. Running these host tests does not execute the game.
