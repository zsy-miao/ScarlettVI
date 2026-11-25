# Host SHAPE context storage for AcpHal

**Status:** Functional, bounded CPU-backed SHAPE context buffer allocation and release for the *one* record family independently observed in the inspected title. This is not SHAPE DSP processing, audio playback, or Xbox APU hardware memory.

## Verified request layout and scope

Read-only Microsoft Dumpbin at VA 0x1443AEC40 shows the title calling `AcpHalAllocateShapeContexts` with RCX pointing to a writable descriptor. The title clears 16 bytes of counts at +0, writes the qword `0x15E` at +0x10 (a 32-bit count 350 and next count zero), then checks EAX for a negative HRESULT after the import call. It then reads a pointer at the descriptor's +0x38 and zeroes `count * 64` bytes. The companion `AcpHalReleaseShapeContexts` at VA 0x1443AE899 has no explicit arguments.

Independent public interoperability research corroborates a 0x48-byte descriptor: 6 little-endian uint32 counts (+0..+0x14), followed by 6 CPU pointers (+0x18..+0x40). The family indexed 4 has a verified 64-byte record stride in this title. Other family record widths and codecs are **unknown**, so the bridge refuses nonempty counts in families 0, 1, 2, 3, and 5 with `E_NOTIMPL`; it does not invent strides.

Public evidence (research only, no third-party implementation code copied):
- https://github.com/OnlyMoisties/GTA6-BootTest/blob/main/REPORT.md
- https://github.com/xcl-dev/xcl/blob/main/HANDOFF.md
- https://learn.microsoft.com/en-us/xbox/gdk/docs/reference/audio/acphal/functions/acphalreleaseshapecontexts

## Implementation

`src/audio/apu_shape.{h,cpp}` exposes the exact-name `AcpHalAllocateShapeContexts(request*)` and `AcpHalReleaseShapeContexts()` entries through ScarlettVI's existing `AcpHal.dll` resolver. It uses `ShapeContextRequest` with compile-time `sizeof == 0x48` and output-pointer offset `0x18`.

- A request must have a nonzero family-4 count no greater than 4096 and all six output slots initially null.
- The active bytes are allocated with existing `ApuAlloc`: `count * 64`, 2048-byte alignment, real zero-initialized writable host memory, and the enforced host APU heap quota.
- Only after complete successful allocation is output slot 4 written. Failure leaves every output untouched and rolls back without leaking buffers or changing owner state.
- At most one descriptor owns the process-wide SHAPE buffers; a repeat with matching owner/count/pointer is idempotent, while concurrent/conflicting requests fail `HRESULT_FROM_WIN32(ERROR_BUSY)`.
- Explicit release clears the owned output pointer, calls the tracked `ApuFree`, erases the 32-bit mapping and is idempotent. A descriptor must remain valid until release.
- This code provides storage only. It performs no context decoding, SHAPE audio mixing or DSP execution.

## Verify

    .\build\x64\Release\ScarlettVI.exe audio-shape-fixture
    .\build\x64\Release\ScarlettVI.exe audio-address-fixture
    .\build\x64\Release\ScarlettVI.exe audio-memory-fixture
    .\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'

The synthetic SHAPE fixture checks exact export routing and x64 function pointer types, bad counts, unsupported families, pre-filled pointers, heap exhaustion and no partial guest output, 350*64 bytes real read/write and mapping, idempotent allocation, conflict and live-budget protection, release with pointer/mapping invalidation, repeated release, reuse with different owner and no allocation leaks. No title code executes.

## Current readiness result

| Component | Previously unresolved | Now unresolved |
| --- | ---: | ---: |
| AcpHal.dll | 3 | **1** |
| All Xbox HLE | 17 | **15** |
| Experimental APU host memory bindings | 6 | **8** |

The table above records the SHAPE storage milestone. The subsequent [partial AcpHal guest factory](APU_HAL_FACTORY.md) also gives `AcpHalCreate` a real host object/vtable and bounded ring buffers, reducing the latest missing Xbox import count to **14**. The factory's DSP configuration method is unimplemented and returns `E_NOTIMPL`; no title boot is claimed. Five `xapu` and five `chad` audio/DSP imports remain, plus XG, DirectStorage and memory transfer. `TITLE EXECUTION: BLOCKED` remains enforced.
