# XMemAlloc heap snapshot bridge

**Status:** original ScarlettVI host-owned heap memory can now be
snapshotted for read-only guest inputs alongside the previously supported
64KiB physical-page CPU aliases. This does **not** translate arbitrary
title process heap, stack, Xbox GPU memory or host `VirtualAlloc` ranges.

## Ownership and lifetime

`src/runtime/xmem_bridge.cpp` already tracks all live
`XMemAlloc` and `XMemAllocDefault` returned addresses, sizes,
allocation kinds and host budget charges. Two new HOST-ONLY C++ helpers
reuse this registry:

- `snapshot_owned_xmem_allocation(source, count, output)` accepts a
  nonzero range no longer than **1 MiB** that lies *entirely* inside
  one registered, live, charged XMemAlloc allocation. It copies the
  verified range while holding the registry mutex. A simultaneous
  `XMemFree` cannot release that allocation halfway through the copy.
  The output is not modified when verification fails.
- `snapshot_owned_xmem_utf16(source, max_chars, output)` accepts
  at most **240** UTF-16 code units plus a required terminator. Each
  16-bit unit must fit within the **same** registered live allocation;
  a missing NUL, truncated allocation, misaligned source or unknown
  pointer fails without touching the destination string. On success
  the caller receives its own immutable `std::wstring` copy.

Both the aligned-heap and fully committed page-backed **XMemAlloc**
variants are recognized. Ordinary C++ allocations, arbitrary user
pointers, freed XMem allocations, and independent `XMemVirtualAlloc`
reservations are rejected. `XMemVirtualAlloc` is deliberately excluded
because Xbox-style reservations may be partially committed, decommitted,
or hold permissions that do not permit CPU reading. These methods never
publish guest exports or provide writable memory pinning.

Concurrent snapshot/free operations are synchronized by the allocation
registry. **Concurrent unsynchronized writes by guest code are not**
made atomic by this bridge; consumers requiring consistent producers
still need a guest-side synchronization contract. A reused numerical
address cannot by itself establish the identity of a prior allocation.

## Real use in the host

The test-only DirectStorage COM `OpenFile` first tries the original
XMem **physical alias** UTF-16 snapshot, then the new XMemAlloc heap
snapshot. If neither registry owns the bytes, it returns
`E_INVALIDARG` without dereferencing the input pointer. Existing
host-only string-literal fixture permits remain separate.

Similarly, the queue's `EnqueueRequest +0x18` accepts generated
uncompressed 64-byte `DSTORAGE_REQUEST` records resident in either
registry, then verifies the file and destination. The request is
copied before the worker accepts it, so the producer may immediately
free its XMem heap request buffer. Asynchronous **destination** memory
is still restricted to validated, **pinned XMem physical aliases**;
ordinary heap destinations remain unsupported because they need a
separate lifetime-safe write-pin system. Unknown actual Xbox command
fields, compressed payloads and title asset paths remain unsupported.

## Tests

```powershell
.\build\x64\Release\ScarlettVI.exe xmem-heap-fixture
.\build\x64\Release\ScarlettVI.exe storage-file-fixture
.\build\x64\Release\ScarlettVI.exe storage-queue-fixture
```

The new standalone fixture verifies exact interior byte snapshots,
allocation-end containment, integer overflow, unknown host pointers,
UTF-16 terminators and failure immutability, page-backed XMemAlloc,
exclusion of unrelated `XMemVirtualAlloc` reservations, concurrent
snapshot/free safety, and failure after release. The file fixture
opens generated mount files from both XMemAlloc allocation kinds and
checks that the returned file remains valid after the source is freed.

The queue fixture additionally uses a guest-style request stored
in an XMemAlloc heap allocation to schedule real readback into
already pinned three-page XMem aliases. It verifies no write before
Submit, exact bytes, pin cleanup, safe immediate request free,
and ordered failures for freed or allocation-truncated descriptors.

No proprietary game code or content is executed by these tests.
**Native title execution remains BLOCKED** due to unimplemented Xbox
graphics, audio, streaming and process startup behavior.
