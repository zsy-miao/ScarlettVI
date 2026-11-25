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

**Important:** the exact queue object's **Xbox virtual method ordering**
remains provisional (following public API inheritance and independently
published Windows DirectStorage vtable ordering). The original title's
own queue-object callsites have not yet confirmed every offset. Do not
enable this for real guest asset loads on that evidence alone.

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
- An internal-only test hook can queue genuine Win32 reads of **generated
  host files**. The status array reports S_OK, the first actual I/O error,
  and an independent reset at each barrier.
- `EnqueueRequest` is **not** a working guest read: the unverified Xbox
  `DSTORAGE_REQUEST` is never dereferenced. Instead, the guest call
  inserts an ordered **E_NOTIMPL failure** so later status barriers
  cannot falsely report success. `EnqueueSignal` also fails closed.
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

**Remaining native startup blockers:** guest request packing/ownership,
guest filesystem and storage semantics, Xbox GPU resource/command ABI,
audio/XAPU/CHAD, process initialization and eight unresolved Xbox import
references. Running these host tests does not execute the game.
