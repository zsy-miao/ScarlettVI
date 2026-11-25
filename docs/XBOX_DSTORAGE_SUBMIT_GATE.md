# Host DirectStorage-style enqueue/submit gate and FIFO completion

**Status: original real Win32 I/O scheduling in a host-only queue. NOT an Xbox IDStorageQueueX COM implementation, NOT a DSTORAGE_REQUEST parser or guest asset streamer.** ScarlettVI still rejects Xbox guest CreateQueue/CreateQueue1/OpenFile with E_NOTIMPL; title execution is BLOCKED.

## Verified public contract

Microsoft publicly documents that an [Xbox IDStorageQueueX](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragequeuex/idstoragequeuex) keeps work until Submit, with FIFO notifications, and an [EnqueueRequest](https://learn.microsoft.com/en-us/xbox/gdk/docs/reference/system/dstorage/interfaces/idstoragequeuex/methods/idstoragequeuex_enqueuerequest) can automatically submit when the enqueues since the last submission reach half of queue capacity. Its [Submit method](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragequeuex/methods/idstoragequeuex_submit) returns void.

These are architectural cues for the independent ScarlettVI host service. **The host types and their return values are not claimed to match an Xbox binary ABI.** The existing guest-observed DirectStorage factory interface and staging method remain partial; no protected game code or assets are executed.

## New native C++20 behavior

The existing `HostAsyncFileQueue` in `src/storage/dstorage_async_queue.{h,cpp}` now separates the admission of a request from execution by the worker:

- `enqueue(HostFileReadRequest)` reserves the normal host 1 MiB/request, 64-request/8 MiB outstanding limits, records a unique token and monotonic 64-bit sequence number, then keeps the request in a **held FIFO**. There is no file open, read, or completion before submission.
- `submit_pending()` transfers **all** held requests to the worker's runnable FIFO without allocating memory or doing disk I/O on the caller. It returns the number of newly committed host requests (unlike Xbox's void Submit), for test diagnostics.
- `submit(request)` remains the immediate-submit convenience interface for previous host callers. It commits any older held requests **before** the new one under the same mutex, ensuring no reordering or synthetic success.
- `SubmitPolicy::Manual` keeps work held until an explicit `submit_pending()` call. `SubmitPolicy::HalfCapacity` commits after the 32nd held request (half ScarlettVI's fixed host queue capacity of 64), using the published Xbox threshold concept. Both are **host policies**, not Xbox guest flags.
- `cancel_unsubmitted(token)` cancels only a still-held request. The request becomes a real `ERROR_OPERATION_ABORTED` completion with no payload; its reservation remains until the completion is taken. Trying to cancel an already-submitted request is rejected as busy rather than pretending an in-flight OS read was stopped.
- The existing `shutdown()` now cancels **both** held and runnable requests; an already-running Win32 read may finish, then the worker joins. All accepted request tokens can be retired through the normal result API.
- The worker has access only to the runnable FIFO. The two FIFOs use C++ `std::list::splice`, which transfers existing nodes under a lock without allocation, so batch commits are not vulnerable to partially applied heap failures.

The original restricted-file-leaf validation, non-reparse root/file policy, exact-length Win32 reads, failure-only missing/EOF completions, data ownership and factory staging lease all remain in force. Existing immediate-submit programs retain their working behavior.

## New real generated fixture

    .\build\x64\Release\ScarlettVI.exe storage-submit-fixture
    .\build\x64\Release\ScarlettVI.exe storage-async-fixture
    .\build\x64\Release\ScarlettVI.exe storage-factory-fixture

`src/storage/dstorage_submit_gate_test.cpp` uses only an independently generated 8192-byte Windows file under ignored `reports/`, cleans exactly that file/directory, and verifies:

- Three manual enqueues reserve capacity but remain unprocessed: no completion even after a zero-timeout wait, and caller output remains unchanged.
- Cancelling token 12 yields an actual error completion with its original sequence 2, with tokens 11 and 13 still held. A manual submission of two surviving requests produces exact, byte-for-byte, FIFO file reads with sequences 1 and 3.
- A new immediate submission following three held requests commits older work first; tokens 21–24 deliver actual bytes in sequences 4–7. Cancelling submitted work fails with a busy error.
- A manual queue shut down with two unsubmitted requests yields explicit cancel completions, zero payloads and no new requests accepted.
- In half-capacity mode, 31 held requests cause **zero reads**. Adding request 32 commits the batch of 32. All 32 payloads and sequences match the independently generated original source.
- Four threads independently enqueue 8 more requests apiece. The 32nd triggers the next automatic batch; all 32 unique completion sequence IDs and actual file contents are checked.
- Five additional held requests below the threshold do not start until an explicit submission; then all five complete with verified byte ranges.

The established `storage-async-fixture`, `storage-factory-fixture`, CPU/loader/audio/memory tests and real NVIDIA RTX 5060 Ti D3D12 tests must also pass.

## Xbox-facing limitations and readiness

The real game `XCRDAPI.dll!DStorageGetFactory` still binds a **partial host COM factory** only. `IDStorageFactoryX2::CreateQueue1`, `IDStorageFactoryX::CreateQueue`, `OpenFile` and status-array methods still fail E_NOTIMPL. The real console request structure, interface IIDs, file lifecycle, compression/decompression, signal/status/event ownership, Xbox memory destination, cancellation tag semantics and guest paths remain unverified. Therefore host `enqueue` and `submit_pending` are **not guest exports**.

The read-only import inventory remains **8 Xbox references unresolved**, with Windows **830/830**, title-local **315/315**, and `TITLE EXECUTION: BLOCKED`. No game binaries, shaders or assets are executed or copied.