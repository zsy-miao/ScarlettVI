# Pinned XMemAlloc destinations for real host DirectStorage writes

**Status:** ScarlettVI now performs real generated-file DirectStorage readback
into bounded XMemAlloc heap destination buffers, **not** just 64 KiB XMem CPU
physical aliases. No actual Xbox title assets, GPU page-list destinations,
compression, or native game execution are enabled.

## Owned write-pin contract

`src/runtime/xmem_bridge.cpp` registers host-owned
`XMemAlloc`/`XMemAllocDefault` allocations. The new
`pin_owned_xmem_destination` helper accepts an exact, nonzero subrange
of at most **1 MiB** inside a single live allocation and returns an
RAII `HostXMemWritePin`. It does not inspect arbitrary guest pointers.

- Allocation-range verification and pin acquisition are atomic under
  the same registry mutex used by `XMemFree`.
- Every live allocation has a pin reference count. `XMemFree` on a
  pinned allocation refuses to release it and sets
  `ERROR_BUSY`; the host caller must retry after the pins are gone.
  Distinct pins against the same allocation are counted separately.
  `VirtualFreeBridge` also rejects any live, registered XMemAlloc
  address (including committed page-backed blocks), with
  `ERROR_BUSY` if pinned or `ERROR_INVALID_ADDRESS` otherwise.
  This prevents bypassing the allocator registry through a raw Windows
  `VirtualFree` import.
- The pin's `write` method checks the exact admitted byte size,
  revalidates the recorded allocation identity and writes while
  holding the mutex. Unknown pointers, overflowed subranges and
  released allocations are never used.
- `HostXMemWritePin` releases its allocation pin on destruction.
  This is a lifetime mechanism, **not** a data-race solution for
  unsynchronized writes made by guest code concurrently with I/O.
- Independent `XMemVirtualAlloc` reservations, ordinary C++ heap
  buffers, title stack pointers and unknown console memory remain
  ineligible.

## Actual queue integration

The existing restricted `IDStorageQueueX2::EnqueueRequest` path
already snapshots recognized guest request bytes from an XMem CPU
alias or host-owned XMemAlloc heap allocation. Now its destination
validation first tries a bounded XMem physical alias pin, then a
bounded `HostXMemWritePin` if the destination is a live XMemAlloc
heap range. These are alternative, never overlapping, destination
types. An unowned/foreign buffer remains an ordered `E_NOTIMPL`
failure, not a bogus successful read.

The host worker retains the file handle and destination ownership
until it has read the full requested byte count. Only an exact
successful file read writes the destination; partial/EOF reads
leave the heap bytes untouched. The pin is released on success,
actual failure, unsubmitted cancellation, or queue shutdown.
A subsequent `EnqueueStatus` marker records the outcome after
the writeback has completed. No guest pointer is dereferenced
before ownership validation. The queue's existing 64-job and
8 MiB staged-byte limits still apply.

## Regressions

```powershell
.\build\x64\Release\ScarlettVI.exe xmem-heap-fixture
.\build\x64\Release\ScarlettVI.exe storage-queue-fixture
.\build\x64\Release\ScarlettVI.exe storage-file-fixture
```

The heap fixture checks multiple overlapping owned pins, rejected
out-of-range/foreign pins, exact writes, failure for mismatched sizes,
`XMemFree(ERROR_BUSY)` while any pin lives, `VirtualFreeBridge`
bypass rejection for page-backed allocations, and successful free
after the final release. The queue fixture checks an XMemAlloc-sourced
request and XMemAlloc **destination**, immediate release of request
memory, no output before Submit, rejection of premature destination
free, exact 2048-byte file writeback, untouched bytes after EOF,
cancellation of a held request, and no leaked pins.
The established tests of 64 KiB XMem alias destinations continue to
run independently.

**Still blocked:** authentic Xbox DirectStorage request variants and
hardware decompression, real guest VFS and title content mounting,
graphics/shader translation, Xbox audio streams and complete title
initialization. `TITLE EXECUTION: BLOCKED` remains accurate.
