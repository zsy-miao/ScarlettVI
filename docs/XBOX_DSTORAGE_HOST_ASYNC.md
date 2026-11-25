# Bounded asynchronous host file queue for future Xbox DirectStorage

**Status: original, tested asynchronous Windows-host file I/O; NOT a bound Xbox IDStorageQueueX interface.** The existing partial Xbox DStorageGetFactory binding remains partial. It still returns E_NOTIMPL for guest CreateQueue and CreateQueue1, and for OpenFile unless a [test-only file root](XBOX_DSTORAGE_FILE_HOST.md) has explicitly been configured. The separate [guest-IID status-array](XBOX_DSTORAGE_STATUS_ARRAY.md) is now implemented but is not a guest queue. The new internal queue performs real generated-file reads and verified completions, but neither game assets nor title code are opened or executed.

## What is implemented

New original files src/storage/dstorage_async_queue.h, .cpp and _test.cpp implement HostAsyncFileQueue, created only with an explicit host-trusted directory and a **host-only** HostFileReadRequest{token, filename, offset, bytes}. These are ScarlettVI's own types; they are not Xbox DSTORAGE_REQUEST or console queue descriptor structures.

- A dedicated Windows worker thread performs genuine CreateFileW, GetFileSizeEx, SetFilePointerEx and ReadFile operations. Only exact-length successful reads return payload bytes. Short/out-of-range reads, missing files and OS failures return actual HRESULT error completions with **no partial data**.
- Requests are admitted and assigned unique monotonic sequence indices. The later [host enqueue/submit gate](XBOX_DSTORAGE_SUBMIT_GATE.md) now holds optional requests separately from the runnable FIFO, permits explicit submission, cancellation before submission and optional half-capacity automatic commits. One worker processes committed requests in FIFO order. Each accepted request receives a unique 64-bit token. The status and independently owned byte vector remain stored until explicitly retrieved by try_take/wait_take.
- At most **64 outstanding reads**, with a **total 8 MiB reserved-byte budget**, and at most **1 MiB per read**. An unconsumed successful completion continues to reserve its share of capacity until taken; errors and failed submissions never fabricate successful data.
- Only single **ASCII leaf filenames** containing letters, digits, '.', '_' and '-' are accepted, maximum 96 characters. Traversal, absolute paths, drive/ADS separators, nested directories, dot entries and Windows device aliases are rejected. The host specifies a trusted, existing non-reparse root; opened files with reparse-point or directory attributes are refused. No guest-supplied title path is accepted.
- shutdown() immediately cancels queued work, permits an already-running synchronous host read to finish and joins the worker. Completed/cancelled requests can still be consumed, while new submissions fail. Destruction releases every owned worker and buffer.
- A **host-only staging lease** links the internal worker queue to the existing partial Xbox factory. When any such queue exists, SetStagingBufferSize returns ERROR_BUSY, even for the current size. After the host queue is destroyed, stage-size changes are again permitted. This enforces the public Xbox rule for these host-owned workers; it does not imply the Xbox guest COM CreateQueue method works.

The [guest-IID status-array object](XBOX_DSTORAGE_STATUS_ARRAY.md) can now receive host-only FIFO status barriers: it aggregates the first actual Win32 read error between markers, exposes E_PENDING before completion, and safely retains COM references until marker retirement. This is not the Xbox EnqueueStatus guest COM method.

Actual program memory is allocated and actual generated file bytes are read. No fake success or empty no-op result is used. This is not NVMe hardware access, Xbox compression/decryption, or a complete game-visible API. The [host-only submission gate](XBOX_DSTORAGE_SUBMIT_GATE.md) now implements the published **Enqueue → Submit** scheduling shape and a 32-request auto-submit threshold with actual Win32 reads, but does not bind an Xbox COM method.

## Real generated regression

    .\build\x64\Release\ScarlettVI.exe storage-async-fixture
    .\build\x64\Release\ScarlettVI.exe storage-submit-fixture
    .\build\x64\Release\ScarlettVI.exe storage-status-fixture
    .\build\x64\Release\ScarlettVI.exe storage-factory-fixture
    .\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'

The fixture writes an entirely generated 2-MiB-plus file into a new directory under ignored reports/, and removes exactly that file/directory on exit. It does not visit the adjacent game build. Tests include:

- Actual Win32 file I/O with **64 queued 128-KiB reads**, exactly **8 MiB reserved**, all full payloads byte-for-byte equal to independently generated source ranges. The 65th request is rejected without touching existing reservations.
- Validating unique tokens, 1..64 FIFO submission sequences, no unbounded queue growth, unknown-token lookup semantics and no partial output mutations on failed queries.
- Path traversal, drive paths, alternate data streams, Windows device aliases, zero-length and oversized read requests fail admission.
- Missing file and beyond-EOF requests produce distinct real Win32 error completions and zero payload bytes.
- **Four concurrent producer threads each submit eight independently indexed reads** and verify all 32 unique completion bytes and monotonic FIFO sequence numbers.
- Shutdown with eight 1-MiB pending reads yields either an authentic completed read or explicit cancellation, never fabricated bytes. After shutdown, new submissions fail. The host staging lease prevents even a no-op resize while a queue exists, releases on destruction, and rejects creating a disk-backed host queue while staging size is zero.

## Public contract and what remains

Microsoft's [IDStorageFactoryX2::CreateQueue1 reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragefactoryx2/methods/idstoragefactoryx_createqueue1) says Xbox queues hold read requests until Submit and complete entries in order. The [SetStagingBufferSize reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragefactoryx/methods/idstoragefactoryx_setstagingbuffersize) forbids resizing while an Xbox queue or file object exists. These guided the original host worker and lease design; they do not authorize substituting its host-only types for Xbox guest ABI structures.

To move beyond this **internal** milestone, independently establish guest queue/file object IIDs and vtable layouts, allocation ownership, DSTORAGE_REQUEST size/version, file handle, compression/BCPACK semantics, Submit/Enqueue and completion arrays/fences, game VFS containment, and memory/asset mapping. Only then return working Xbox queue objects or enable general Xbox title file services from the already-bound partial factory.

**Readiness remains 8 unresolved Xbox imports**, the existing factory is counted once as partial, and TITLE EXECUTION: BLOCKED is unchanged. No original game code has been run.