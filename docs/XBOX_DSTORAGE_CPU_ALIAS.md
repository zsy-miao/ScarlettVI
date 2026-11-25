# DirectStorage writeback to pinned XMem CPU aliases

**Status: tested host-approved 64KiB CPU-alias spans with real
asynchronous writeback. Native title streaming and execution remain blocked.**

ScarlettVI connects its DirectStorage FIFO worker to the original
[XMem 64KiB CPU-alias implementation](../src/runtime/physical_memory.h)
rather than using host-created vectors alone. The destination is real
Windows file-mapped CPU memory owned by the XMem compatibility layer.

## Ordinary CPU virtual destinations, not Xbox page arrays

The published [Xbox DirectStorage request](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_request)
distinguishes the ordinary CPU virtual-memory `Destination` from
`DestinationPageArray` when the physical-pages option is selected.
Only ordinary virtual-address `Destination` with zero request options
(file source, no compression, no swizzling, no physical-page destination)
is supported by this prototype. It does **not** implement the Xbox
physical destination page-array contract, GPU destinations, or BCPack.

## Host-approved span pinning

`HostDStorageAliasDestination::pin_pages` accepts a trusted CPU
destination pointer, a byte count and the **exact synthetic frame identity
for each intersected 64KiB CPU virtual-address page**. Its safeguards:

- A read must be nonempty and at most **1MiB** (the current host worker
  limit). An unaligned 1MiB destination can intersect at most **17 pages**.
  Destination arithmetic is overflow-checked before any mapped memory
  is touched. The caller must supply exactly the required frame count.
- Every frame is verified by the existing
  `PhysicalMemory::pin_mapped_alias(page_base, frame)`; an unknown,
  unmapped, stale, or incorrectly numbered page is rejected.
- Bookkeeping is allocated **before** any pin is acquired. If page N
  fails, previously acquired pins 0–N-1 are rolled back by RAII, without
  leaving partially admitted transfers.
- Existing XMem remap, unmap and release operations refuse pinned pages
  with `ERROR_BUSY` until every pin is released. These are real mapped
  host CPU pages, not simulated page arrays.
- The queued read retains the exact ordered pin set and independent
  file handle. It copies bytes to the now-validated contiguous CPU
  virtual span **only after the complete file read succeeds**. Writeback
  occurs before the next FIFO status marker completes.
- Failed reads, queue-admission failures and withdrawn one-shot permits
  release all pins and never perform partial destination writes.

The original single-page host helper
`register_host_dstorage_fixture_alias_request(request, frame)`
remains strict: a single frame **cannot** authorize a crossing write.
The new
`register_host_dstorage_fixture_alias_pages_request(request, frames, count)`
approves a multi-page range only for trusted generated tests.
Neither registration function is exported as a guest service.

**New guarded request snapshot route:** In the generated-file fixture,
`EnqueueRequest` can also accept a request pointer **without any explicit
host registration** when the entire 64-byte request is backed by mapped,
ScarlettVI-owned XMem CPU pages. `PhysicalMemory::snapshot_mapped_alias`
validates every intersected request page under its registry mutex,
then copies the request. It resolves the destination's registered CPU
pages and revalidates all frames through the existing pinning system.
Unknown pointers, native stack/heap storage, unowned destinations and
unsupported options are rejected without dereferencing them. The queue
is still unavailable unless an explicitly configured trusted generated
fixture root exists; this is **not** unrestricted title streaming.

## Verification

`src/storage/dstorage_queue_host_test.cpp` exercises both the original
single-page route and a real **three-page** write:

- A generated file is read into a virtual destination starting 333 bytes
  before the end of the first 64KiB page, traversing the full middle page
  and finishing inside the third.
- All bytes are compared to the original generated source. Before
  `Submit`, the destination is unchanged and all three pins prevent
  unmapping or releasing the alias region.
- A deliberately wrong **middle** frame and an unmapped middle page fail
  admission, then leave zero pins (proving rollback of the first pin).
- Too few supplied frames and address wraparound are rejected without
  attempting a memory write.
- A submitted read beyond the generated file EOF completes with an actual
  error, does not modify any destination bytes, and releases every pin.
- Withdrawing an unused permit releases all three pins. **Closing a second
  queue with an unsubmitted three-page request** likewise cancels that
  job, preserves the destination, and releases its pins.
- The virtual region can then be unmapped and freed without leaked frames
  or aliases; the original queue's staging lease stays unaffected.
- An unregistered request **crossing two XMem source pages** safely
  snapshots its struct and pins a separate three-page destination.
  Its original source mapping can be wiped and released before Submit.
  Wrong option bits, an unowned destination, and an unmapped source
  page are refused; successful generated data writes exactly as
  requested and all destination pins are released.

Run from the ScarlettVI project root:

```powershell
.\build\x64\Release\ScarlettVI.exe storage-queue-fixture
.\build\x64\Release\ScarlettVI.exe memory
.\build\x64\Release\ScarlettVI.exe memory-scatter
```

No original-title data is opened for this fixture, and no game code runs.

## Remaining limitations

The implementation needs a title-accessible guest virtual-memory
translator, proper guest destination ownership, Xbox storage decompression,
resource swizzling, cancellation/error records, a real guest VFS, GPU
buffer destinations, and full queue-vtable conformance before it can run
actual title workloads. A pin verified in the synthetic XMem registry is
**not** proof of Xbox CPU/GPU coherence or physical address translation.
