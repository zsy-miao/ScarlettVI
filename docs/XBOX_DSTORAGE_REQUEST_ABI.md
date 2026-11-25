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

## Verified, strictly fixture-only host path

- `src/storage/dstorage_queue_host.h` defines the 64-byte x64 request
  shape with `static_assert` checks for relevant field offsets.
  `options=0` selects an ordinary file-sourced, uncompressed,
  non-swizzled, non-physical-memory read.
- A host-generated test must explicitly register a *snapshot* of the
  request pointer and a `shared_ptr` to an owned `vector<uint8_t>`.
  This registration function is **not** exported or available to titles.
  Registration rejects inconsistent source/destination sizes, arbitrary
  destination addresses, unsupported options, missing files, and duplicate
  permits.
- Queue `EnqueueRequest` compares the request **address** with the
  synchronized permit registry, rather than dereferencing arbitrary
  user/guest pointers. It consumes one permit and uses the immutable
  snapshot. Only a ScarlettVI-owned live `IDStorageFileX` can be
  reopened; file-source size must be 1–1,048,576 bytes and equal
  destination size.
- The existing host FIFO worker retains the read handle **and the
  destination vector** until it has read exactly the requested bytes.
  Actual destination writeback occurs before the following
  `EnqueueStatus` marker completes. Failed reads do not write partial
  destination data, and the marker observes the real first HRESULT
  failure since its previous marker.
- Arbitrary, unregistered, malformed, or unsupported guest requests
  are **never dereferenced**: they generate ordered `E_NOTIMPL` or
  validation-error completions, not fake success. Existing 64-slot and
  staging-budget safety limits remain active.
- Generated-file regression checks raw queue slot +0x18 invocation,
  no write before Submit, byte-for-byte writeback at a nonzero file
  offset, duplicate permit rejection, source-handle independence,
  immediate reuse of the caller's request structure, and rejection of
  intentionally invalid request pointers.

## Still required before any title streaming

A title-accessible secure guest memory map / pinning mechanism (not just
host-owned vectors), Xbox page-list and compression modes, BCPack and
swizzling, guest VFS and actual title paths, full queue vtable validation,
cancellation and lifetime semantics, actual Xbox error records/events,
GPU destination resources, and the outstanding graphics/audio services.

**Nothing in this implementation mounts or copies protected title data,
starts the executable, or claims playable GTA VI.**
