# AcpHal APU host allocation and ownership bridge

**Status: narrow, functional Windows CPU-memory subset only.** This bridge does not supply the Xbox APU hardware, device-address translation, DSP command processing, an Xbox heap, XMA decoding, or audio playback. Title execution remains BLOCKED.

## What has been implemented

ScarlettVI now links two exact-name imports from `AcpHal.dll`:

- `ApuAlloc`: `HRESULT WINAPI ApuAlloc(void** output, void* heap, uint32_t byteCount, uint32_t alignment)`
- `ApuFree`: `void WINAPI ApuFree(void* pointer)`

The independently read-only disassembled title wrapper at `0x1443A6110` prepares an output-pointer argument, sets the heap parameter to null, passes a 32-bit byte count and uses `0x800` alignment before calling the `ApuAlloc` thunk. Microsoft's DirectXTK and an independently published Wwise integration likewise show an out-pointer allocation and a corresponding one-pointer free. That is strong evidence for a **null-heap allocation subset**, not for the rest of the APU ABI.

The original host implementation, `src/audio/apu_memory.{h,cpp}`, uses real Windows CRT `_aligned_malloc`/`_aligned_free` ownership with a thread-safe allocation registry:

- Accepts only a null explicit heap; a non-null Xbox heap is rejected with `E_NOTIMPL`.
- Validates non-null output, nonzero byte count and power-of-two alignment from 4 to 65536. Host pointer alignment is at least the native pointer alignment.
- Limits a single allocation to 64 MiB and total live host allocations to 256 MiB (conservative **desktop** caps; **not** Xbox hardware limits). Invalid sizes/alignment fail with `E_INVALIDARG`; allocation exhaustion returns `E_OUTOFMEMORY`.
- Sets the output pointer to null on every failure where the out-pointer is valid.
- Returns writable, zero-initialized, correctly aligned real host storage on `S_OK`.
- Frees only exact base pointers allocated by the bridge; null, interior, double-free and foreign pointers are safely ignored. Live byte and allocation counts are auditable through a host-only query.
- Binds by exact named export only, never by invented ordinal.

The Xbox APU may use device-visible or 32-bit address maps which cannot be inferred from a CPU pointer. Nothing here claims CPU-backed host storage can be submitted to the console's hardware decoding interface.

## Evidence and licence boundaries

Public C/C++ examples demonstrating the allocation pair:
- https://github.com/microsoft/DirectXTK (public examples; no code copied)
- https://git.peroxy.dev/kookroach/game-physics/src/commit/409a71248accd4b4cb585d7d2cce9667458b98ba/DirectXTK/Audio/SoundEffect.cpp
- https://github.com/xcl-dev/xcl/blob/main/HANDOFF.md (API motivation only; GPL source was **not** copied)

The title is inspected **read-only** using Microsoft Dumpbin. No title bytes, proprietary SDK headers, guest binaries, or third-party implementation code are placed in ScarlettVI.

## Build and verify

```powershell
.\build\x64\Release\ScarlettVI.exe audio-memory-fixture
.\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

`audio-memory-fixture` is completely host-generated. It verifies:

- Exact-name import lookup and C ABI function-pointer signatures.
- Alignment 4, 8, 16, 32, 64, 2048, 4096 and 65536 bytes, including a one-byte allocation.
- Zero initialization and actual writable first/last bytes.
- Null output pointer, invalid alignment, unsupported explicit heap, zero and oversized allocations; null outputs and correct HRESULT results on failure.
- Interior pointer, foreign pointer, double-free and null free cannot touch unrelated host storage.
- Four concurrent worker threads × 128 actual allocation/free cycles, followed by an unchanged allocation registry snapshot.

## Impact on the inspected title's import inventory

| Category | Before | After |
| --- | ---: | ---: |
| `AcpHal.dll` missing import addresses | 9 | 7 |
| Total missing Xbox import addresses | 23 | 21 |
| Host APU alloc/free bindings | 0 | 2 |
| Game execution | BLOCKED | BLOCKED |

There remain **17 APU/audio/DSP imports** spread across `AcpHal.dll` (7), `chad.dll` (5) and `xapu.dll` (5), plus the `xg_xs.dll` graphics methods (2), `XCRDAPI.dll` DirectStorage factory (1) and `xmem.dll` memory transfer method (1). The new addresses are reported separately as `APU host alloc/free=2` and do not imply the entire audio subsystem works.

**Next:** validate `ApuCreateHeap`'s actual sizing and creation contract, build host APU address translation using independent guest-facing synthetic tests, then tackle audio command and result queues with real lifecycle semantics. Do not guess interfaces merely to reduce the pending count.
