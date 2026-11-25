# DirectStorage writeback to pinned XMem CPU aliases

**Status: implemented and tested only for host-approved, generated-file
requests. Xbox game asset streaming remains unavailable; title execution
continues to be blocked.**

ScarlettVI now connects its tested DirectStorage queue worker to the
existing original [XMem 64KiB CPU-alias implementation](../src/runtime/physical_memory.h),
rather than copying only into host-created `std::vector` buffers.
This shares a real, Windows file-mapped CPU destination with existing
XMem page reservation and alias ownership.

## Contract

The [Xbox DirectStorage request definition](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_request)
distinguishes ordinary virtual-memory `Destination` from
`DestinationPageArray` when `DestinationIsPhysicalPages` is set.
**This milestone implements only the ordinary `Destination` form**, with
`Options.AsUINT64 == 0` (file source, no compression/swizzle, and
`DestinationIsPhysicalPages == false`). It does **not** implement
Xbox DirectStorage physical-page-array destination semantics.

The host-only `HostDStorageAliasDestination::pin`:

- Accepts an actual CPU pointer, the exact synthetic XMem physical-frame
  identity for its backing 64KiB page, and an exact byte length.
- Checks non-null and bounded size and rejects any destination that crosses
  a 64KiB page boundary, including integer overflow edge cases.
- Calls the *existing*
  `PhysicalMemory::pin_mapped_alias(page_base, physical_frame)`.
  This only succeeds when the page is a genuine ScarlettVI-owned mapped
  CPU alias and the specified synthetic frame matches its mapping.
- Holds that registry pin with RAII until the pending request has either
  completed or been cancelled / rejected / unregistered. Existing
  XMem alias remap, unmap, and release operations return `ERROR_BUSY`
  until the reference is released.
- Copies a **fully successful** generated file read to the pinned
  destination immediately before the next FIFO status barrier completes.
  Failed reads do not partially overwrite the CPU alias.
- Keeps the current 64-entry outstanding and 1MiB-per-read host quotas;
  no arbitrary guest memory pointers are ever dereferenced by the queue.

Only a trusted host fixture can register a one-time request permit
with `register_host_dstorage_fixture_alias_request(request, frame)`.
The normal queue call cannot register a pointer; it only consumes already
approved snapshots and treats unapproved guest requests as ordered failures.
The caller's original request struct can be reused after
`EnqueueRequest` returns. The host-side permit retains a pin
across this call and transfers it into the worker.

## Verified implementation

- `src/storage/dstorage_alias_destination.{h,cpp}`: original XMem-backed
  pinned write target with range checks and deterministic release.
- `src/storage/dstorage_queue_host.{h,cpp}`: additional trusted
  one-shot alias permit, no extra title export.
- `src/storage/dstorage_async_queue.{h,cpp}`: retains pins alongside
  file handles and optional generated-vector destinations, performs exact
  readback writeback, releases pins on cancellation and shutdown.
- `src/storage/dstorage_queue_host_test.cpp`: real generated-file queue
  test with a mapped 64KiB CPU alias. Verifies a 512-byte read with a
  nonzero source offset and an end-of-page destination, byte-by-byte
  correctness, no writes before Submit, pinned unmap/release blocking,
  wrong-frame and cross-page rejection, no partial writes after EOF,
  one-shot permit withdrawal, and zero leaked XMem frames/pins.

Run:

```powershell
.\build\x64\Release\ScarlettVI.exe storage-queue-fixture
```

No protected game data is accessed. No console graphics, GPU residency,
physical page-list mode, title VFS routing or arbitrary guest-memory
translation is claimed. This is a **host-verified one-page CPU-alias
writeback path**, not a general Xbox memory manager.
