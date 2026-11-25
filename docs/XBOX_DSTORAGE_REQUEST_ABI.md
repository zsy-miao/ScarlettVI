# Xbox DirectStorage request ABI: verified callsites and safe host subset

**State: original Xbox title inspected read-only; four callsites verified.
A generated-file-only `DSTORAGE_REQUEST` copy path has real asynchronous
buffer writeback, but native title streaming and execution remain blocked.**

## Evidence and repeatable verification

The original target contains the following x64 indirect COM calls, verified
using `dumpbin /DISASM /RANGE` and reproduced by the original,
dependency-free, read-only [ABI script](../scripts/check_dstorage_queue_abi.py):

| Callsite in observed title | Method | Vtable offset | Evidence |
| --- | --- | --- | --- |
| `0x1428C9522` | Factory `CreateStatusArray` | `+0x28` | original title instruction |
| `0x1428C95C7` | Factory `CreateQueue1` | `+0x50` | original title instruction |
| `0x1428C9637` | Queue `Close` | **`+0x40`** | queue pointer from object field +0x18 |
| `0x1428C9A0B` | Queue `EnqueueRequest` | **`+0x18`** | queue pointer from object field +0x18, request prepared on the stack |

The `EnqueueRequest` caller loads RCX from its factory-created queue,
passes a stack-constructed request pointer in RDX, and invokes
`call [rax+18h]`. The standalone `Close` caller loads the queue pointer
from the same host-object field and calls `[rax+40h]`. These callsites
confirm method slots, **not all original-title request field offsets**.

**Scope of confidence:** These two **queue slot offsets** are now supported
by actual target callsites, rather than only by desktop DirectStorage ABI
analogy. The other queue virtual slots remain provisional. The request
field order and 64-byte x64 layout follow the
[public Xbox GDK `DSTORAGE_REQUEST` declaration](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_request),
with the unchanged union representing file sources in our C++ subset.
The public [options-bitfield reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_request_options)
also describes compression, source, swizzle and page-mode fields; these
are **NOT implemented**. The targeted title disassembly alone has not
validated every field for every request variant.

From the ScarlettVI root:

```powershell
py -3 scripts\check_dstorage_queue_abi.py '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
.\build\x64\Release\ScarlettVI.exe storage-queue-fixture
```

The Python checker maps only raw executable-backed PE sections, verifies
the exact four indirect-call instruction patterns, and exits nonzero on
mismatch or a malformed PE. It does not execute, patch, dump, decrypt,
or publish game bytes; the target file is never opened for writing.
A different build may have different addresses and must be reanalyzed.

## Guarded, fixture-rooted XMem request snapshot path

- `src/storage/dstorage_queue_host.h` defines the 64-byte x64 request
  shape with `static_assert` checks for relevant field offsets.
  `options=0` selects an ordinary file-sourced, uncompressed,
  non-swizzled, non-physical-memory read.
- Existing host-generated tests may explicitly register an immutable
  snapshot of the request and a host vector or
  [pinned XMem CPU alias span](XBOX_DSTORAGE_CPU_ALIAS.md). Permits are
  one-shot, and are not exported as title services.
- **Expanded:** unregistered request structures are accepted only when
  the **entire 64-byte structure** is backed by either a mapped,
  ScarlettVI-owned **XMem CPU alias**, or a **live XMemAlloc heap
  allocation**. The original physical-memory snapshot verifies all
  intersected pages under its mapping lock; the new
  [heap snapshot bridge](XMEM_HEAP_SNAPSHOT.md) verifies the full
  buffer against a live allocation and copies under the registry lock.
  Unknown raw pointers are never dereferenced; XMem heap requests can
  be freed immediately after enqueue because the worker owns a copy.
- The destination virtual range is resolved separately through
  `PhysicalMemory::resolve_mapped_alias_span`, which returns the
  synthetic frame identity of each intersected CPU page (up to 17).
  The queue **revalidates and pins every page** before admission.
  If a mapping changed or a page is absent, it rolls back all pins
  without writing memory.
- Raw request pointers outside these two XMem ownership registries
  are **never dereferenced**. Requests with nonzero/unsupported options, unknown
  file objects, invalid sizes or unowned destinations become **ordered
  failures**, not fake successful transfers. Only a ScarlettVI-owned
  live `IDStorageFileX` can be reopened.
- Both paths remain gated by the trusted generated-file fixture root:
  `CreateQueue1` remains unavailable by default to native titles,
  and neither method mounts or reads the title's actual game assets.
- The existing host FIFO worker retains the read handle and exactly
  one approved destination: a host fixture vector, pinned XMem CPU
  alias or [pinned XMemAlloc heap subrange](XMEM_HEAP_DESTINATIONS.md).
  Real writeback occurs before the following
  `EnqueueStatus` marker completes. Failed reads do not write partial
  destination data; cancellation or permit withdrawal releases any page
  pin. The marker observes the first real HRESULT failure since its
  previous marker.
- Unregistered requests that are **not verified by XMem CPU alias or
  live heap ownership** are never dereferenced and become ordered
  `E_NOTIMPL` failures. XMemAlloc heap destinations are now
  admitted only with [reference-counted write pins](XMEM_HEAP_DESTINATIONS.md)
  that block premature `XMemFree`; ordinary process heaps and
  arbitrary guest pointers remain unsupported. Existing queue
  safety limits apply.
- Generated-file regression checks raw queue slot +0x18 invocation,
  no write before Submit, byte-for-byte writeback at a nonzero file
  offset, duplicate permit rejection, source-handle independence,
  immediate reuse of the caller's request structure, and rejection of
  intentionally invalid request pointers. A second case submits an
  unregistered 64-byte request **crossing two mapped XMem pages** and
  writes exactly to a separate three-page XMem destination, even after
  the request structure has been wiped, unmapped and freed. Missing
  source pages, ordinary stack destinations and unsupported option bits
  fail safely without leaking pins. A new positive case places the
  request in a real XMemAlloc heap buffer, frees it right after enqueue,
  and verifies genuine file-to-three-page-alias writeback, FIFO status,
  no pre-Submit writes, and no pin leaks. Freed or clipped heap request
  pointers produce ordered `E_NOTIMPL` failures. The latest
  regression also writes real generated-file bytes directly to a
  live XMemAlloc destination and verifies busy free, exact success,
  all-or-nothing EOF and cancellation pin cleanup.

## Still required before any title streaming

A general title-accessible guest-memory translator, Xbox physical
page-list destination semantics and compression modes, BCPack and
swizzling, guest VFS and actual title paths, full queue vtable validation,
cancellation and lifetime semantics, actual Xbox error records/events,
GPU destination resources, and the outstanding graphics/audio services.

**Nothing in this implementation mounts or copies protected title data,
starts the executable, or claims playable GTA VI.**
