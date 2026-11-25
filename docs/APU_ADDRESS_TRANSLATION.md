# Owned 32-bit APU address translation

**Status:** Functional reversible host-allocated CPU memory ↔ 32-bit APU address mapping. This does not implement physical Xbox APU RAM, arbitrary game allocations, DMA, audio DSP, or playback. Title execution remains BLOCKED.

## Three additional AcpHal imports

| API | Caller evidence | Implementation |
| --- | --- | --- |
| ApuIsVirtualAddressValid | RCX=CPU pointer, EDX=span, boolean tested in AL | Only live ApuAlloc-owned, fully contained nonzero ranges return TRUE |
| ApuMapVirtualAddress | RCX=CPU pointer, return EAX saved as DWORD | Returns owned 32-bit address, zero if not owned |
| ApuMapApuAddress | ECX=APU DWORD, return RAX tested against null | Returns corresponding owned CPU pointer or null |

Read-only MSVC Dumpbin corroborates ApuIsVirtualAddressValid at 0x140078790 / 0x1400787E3 / 0x140078AFD / 0x140078B41; ApuMapVirtualAddress at 0x14007879C / 0x1400787EF / 0x140078B09 / 0x140078B4D; and ApuMapApuAddress at 0x1400783F6 / 0x140078485. These local caller shapes do not prove full console hardware semantics.

## Ownership, safety, and lifetime

The implementation in src/audio/apu_memory.cpp shares a mutex with ApuAlloc/ApuFree and maintains ordered CPU→APU and APU→CPU maps. It assigns nonzero 32-bit addresses starting at 0x10000, aligned to at least 4 KiB or the allocation alignment. It checks 4 GiB domain overflow in 64-bit arithmetic. Interior offsets round-trip exactly, and null, foreign, out-of-bounds and freed addresses do not resolve.

On free both indexes are erased. Guest APU ranges are deliberately never reused, so a stale guest address cannot reference a new allocation even if the host reuses the old CPU pointer. The optional 32-bit ApuAlloc output now reports the actual owned mapping address; unsupported nonzero flags clear both outputs and fail. The host memory itself is genuine writable zero-initialized aligned storage, charged against the existing enforced host heap budget.

Only allocations managed by ApuAlloc are in this guest mapping. XMem, native game heaps and arbitrary Windows committed addresses are intentionally rejected until their APU visibility and lifetimes are independently verified. A lookup does not pin its returned pointer against concurrent release; callers must synchronize object lifetime.

## Reproduce

    .\build\x64\Release\ScarlettVI.exe audio-address-fixture
    .\build\x64\Release\ScarlettVI.exe audio-memory-fixture
    .\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'

The host-generated address fixture validates the three exact function-pointer signatures, 770 interior-offset round trips in two differently aligned allocations, invalid and one-past-end pointers, optional address output, stale mappings, foreign-pointer rejection, four threads with 128 alloc/map/free cycles each and unchanged owned-allocation counts. No game code is executed.

## Readiness

| Metric | Before | Now |
| --- | ---: | ---: |
| Pending AcpHal references | 6 | 3 |
| Pending Xbox references | 20 | 17 |
| Bound host APU memory references | 3 | 6 |
| Title execution | BLOCKED | BLOCKED |

Remaining AcpHal APIs: AcpHalCreate, AcpHalAllocateShapeContexts and AcpHalReleaseShapeContexts. Other pending imports: xapu five, chad five, xg_xs two, DirectStorage one, XMemTransferMemory one. No real GTA VI boot is claimed.

Public independent research context (no external implementation source copied): https://github.com/xcl-dev/xcl/blob/main/HANDOFF.md and https://github.com/Freminet/ScarlettX.
