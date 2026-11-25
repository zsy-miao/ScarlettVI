# Partial Xbox DirectStorage status arrays with real completion barriers

**Status: guest-facing `IDStorageStatusArrayX` COM creation is functional, with a bounded original host implementation of completion state. Xbox guest `CreateQueue`, `CreateQueue1`, `DSTORAGE_REQUEST`, guest-path file routing, decompression and game-asset streaming remain unsupported. `OpenFile` has a separate [generated-file test-only adapter](XBOX_DSTORAGE_FILE_HOST.md). The title-execution safeguard remains BLOCKED.**

## Confirmed interface addresses and a corrected factory vtable

Read-only original-title disassembly plus read-only PE .rdata extraction identify the following **interface IDs and callsite argument shapes**, without executing any game bytes:

| Factory method | Guest vtable offset | Confirmed direct callsite | IID extracted from original title data |
| --- | --- | --- | --- |
| DStorageGetFactory (module import) | N/A | 0x141D8E4F0 | 62959d11-d45f-4884-a0a3-f7862ea243ae |
| OpenFile(path, IID, out) | **+0x20** | 0x1428C8C1E and 0x1428C9D54 | 43704f9f-27c6-4a9f-8815-d16527894df4 |
| CreateStatusArray(capacity, name, IID, out) | **+0x28** | 0x1428C9522 (capacity 1) | cd66d752-2786-44f7-9070-e2a6fdad3f1b |
| CreateQueue1(desc, IID, out) | **+0x50** | 0x1428C95C7 | 3c02cb0c-c050-4195-99a1-a1ce94ba33c5 |

**Important ABI correction:** a prior original factory declaration used an alphabetical `CreateQueue, CreateStatusArray, OpenFile` virtual order. The actual title makes the 3-argument `OpenFile` call at slot +0x20 and the 4-argument `CreateStatusArray` call at +0x28. ScarlettVI now declares `CreateQueue, OpenFile, CreateStatusArray` in that order. Both slots are tested via raw COM function pointers, avoiding false confidence from calls that go through the same mistaken C++ declaration. This correction does **not** imply game file streaming works: normal `OpenFile` remains E_NOTIMPL, while an explicitly configured generated-file root now supports a [real host file object](XBOX_DSTORAGE_FILE_HOST.md).

**Status method ordering caveat:** the Xbox public reference lists `GetHResult` and `IsComplete` but does not explicitly guarantee a vtable layout. The adapter uses `IsComplete` at +0x18 and `GetHResult` at +0x20, matching the independently published Windows DirectStorage ABI. This is a reasoned Xbox compatibility assumption until separately confirmed from actual title status-object use. It is NOT a confirmed Xbox-specific vtable result.

The public Xbox [CreateStatusArray](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragefactoryx/methods/idstoragefactoryx_createstatusarray) contract confirms `UINT32 capacity, PCSTR name, REFIID, void**`. The public [status array interface](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragestatusarrayx/idstoragestatusarrayx) provides GetHResult and IsComplete; [GetHResult](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragestatusarrayx/methods/idstoragestatusarrayx_gethresult) returns E_PENDING while work is unfinished, the first prior read error on failure, or S_OK on success. [EnqueueStatus](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragequeuex/methods/idstoragequeuex_enqueuestatus) notes that status entry state is undefined until first use. Our original implementation conservatively exposes E_PENDING until actual host work is attached; it does not pretend earlier work completed.

## Actual original implementation

Files: `src/storage/dstorage_status_host.{h,cpp}`, `src/storage/dstorage_status_host_test.cpp`, plus changes in `src/storage/dstorage_factory_host.{h,cpp}` and `src/storage/dstorage_async_queue.{h,cpp}`.

- The factory's now-correct `+0x28` method returns an actual reference-counted heap COM object for the **exact observed status IID**. Supports standard IUnknown identity/QueryInterface and documented status GetHResult / IsComplete methods at +0x20 / +0x18 respectively. No desktop COM object is substituted.
- Capacity is bounded to **1–256 entries** by an original host safety policy, not an asserted Xbox limit. Unsupported interface IDs and invalid capacities return explicit HRESULT errors with the caller's output pointer cleared. Debug names are neither retained nor blindly dereferenced.
- Each entry owns synchronized `E_PENDING / S_OK / failure` state. Its pre-queue state is intentionally fail-closed (`E_PENDING`), because the public console contract does not define the initial value. A pending index cannot be enrolled twice concurrently; after real completion it may be reused.
- New **host-only** `HostAsyncFileQueue::enqueue_status(token, status, index)` submits a zero-payload FIFO barrier. The host worker processes each preceding *committed* file read, remembers the first actual failure since the previous barrier, and marks the status entry with that real result when it reaches the barrier. The aggregate resets after each marker. This is not the guest `IDStorageQueueX::EnqueueStatus` export: the guest queue COM object does not exist yet.
- Holding a status marker under the Manual submission policy does **not** execute I/O. Taking a completed marker retires it from the queue; cancellation of a marker before submission yields `ERROR_OPERATION_ABORTED` rather than a fake successful status.
- The queue owns an additional COM reference for each accepted barrier until its result is retired or its queue is destroyed. This prevents use-after-free if the creating title-like caller releases its own reference during real host I/O.
- No privileged console memory or hardware status is invented. Existing host queue limits (64 outstanding requests, 8 MiB reserved bytes, safe leaf filename rules) still apply; each status barrier consumes an entry but **zero payload bytes**.

## Generated-file regression

    .\build\x64\Release\ScarlettVI.exe storage-status-fixture
    .\build\x64\Release\ScarlettVI.exe storage-submit-fixture
    .\build\x64\Release\ScarlettVI.exe storage-factory-fixture

The new test constructs an original 4 KiB file in ignored `reports/`, never accesses the protected title assets, and checks:

- Factory COM vtable raw slot **+0x20 OpenFile = E_NOTIMPL**, **+0x28 CreateStatusArray returns a real object**, exact observed IID and IUnknown QI, and rejects unsupported GUIDs, invalid capacities and null output pointers.
- Raw status object vtable IsComplete (+0x18) and GetHResult (+0x20) slots; fail-closed initial E_PENDING; invalid indices and pending-slot double-enrollment rejection.
- Three verified queued status markers: successful actual file read -> S_OK; missing-file ReadFile -> actual `ERROR_FILE_NOT_FOUND`; subsequent valid read -> independent S_OK after first-error aggregate resets. Every real payload byte is compared.
- A marker cancelled before submit produces a cancelled completion, and reusing its status entry is supported when complete.
- A marker survives early release of its original COM owner, remains referenced while queued, and is destroyed after the real completion is taken. All test files and host references are cleaned up.

## Readiness

**8 Xbox imports remain unbound** (CHAD 5, XApuEnqueueCommand 1, XG 2). The previously bound DirectStorage factory remains **partial=1**: `CreateStatusArray` now functions, along with its real staging memory management and the separate host-only worker and queue scheduler. normal title `OpenFile` routing, `CreateQueue`, `CreateQueue1`, guest status notifications, decompression, and actual title-asset streaming remain missing. The test-only file adapter is documented separately. `TITLE EXECUTION: BLOCKED` is unchanged.