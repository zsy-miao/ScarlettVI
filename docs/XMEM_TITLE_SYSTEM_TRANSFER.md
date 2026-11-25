# XMem title-to-system host budget transfer

**Status: bounded host budget accounting with real XMemAlloc admission control.** The exact Xbox import xmem.dll!XMemTransferMemory is bound, but only direction 1 and multiples of 2 MiB are supported. It **does not relocate Xbox physical memory**, reproduce true Series X|S title/system quotas or implement system-service consumer allocations. The binding is explicitly marked **budget-partial**. TITLE EXECUTION: BLOCKED remains enforced.

## Verified title caller and interface

A direct E8 call to IAT thunk 0x145F3FD70 (IAT 0x146793A50) was independently verified in the supplied executable at VA **0x14221608E**, using Microsoft dumpbin and read-only PE parsing. The caller:

- Sets ECX=1 for requested direction.
- Computes the 64-bit difference between two working-set values and rounds upward to a multiple of **0x200000** (2 MiB).
- Caps the size at **0x1000000** (16 MiB), supplies it in RDX and tests EAX as a BOOL (zero=failure).

Microsoft publicly documents that [XMemTransferMemory moves title memory into the system partition](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/game-save/game-saves-debugging) and that system-transferred memory may persist across a game restart ([XLaunchNewGame](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgame/functions/xlaunchnewgame)). The detailed API documentation is authorization-restricted. We do not copy restricted headers or assume unverified other directions.

The original exported signature is BOOL WINAPI XMemTransferMemory(uint32_t direction, size_t bytes) noexcept. Only direction=1 is accepted; other values return FALSE / ERROR_NOT_SUPPORTED. The size must be 2-MiB-aligned, else FALSE / ERROR_INVALID_PARAMETER. A zero-byte request succeeds without changing accounting or Win32 last-error state.

## Genuine host behavior, with clearly limited scope

Original sources: src/runtime/xmem_transfer.h and .cpp. A process-wide, mutex-protected host budget ledger maintains a title quota, a system quota and actually charged title heap bytes.

- Initial title allocation limit: **16 GiB**, system limit: zero. These are arbitrary ScarlettVI desktop host policy values, not official console RAM partition figures, not physically committed, and not automatically synchronized with GPU or Windows system memory.
- XMemAlloc / XMemAllocDefault **reserve real heap quota before obtaining real Windows host RAM**. OS allocation failures roll back the charge, and freeing an owned host allocation returns that charge. Unknown pointers do not release any quota.
- Successful transfers subtract bytes from the title limit and add exactly the same amount to the system limit. Transfers that exceed uncharged title capacity fail ERROR_NOT_ENOUGH_MEMORY; overflow rejects without mutating the ledger.
- All admissions and transfers operate under one synchronized budget ledger, so a simultaneous heap allocation and transfer cannot spend quota twice.
- The host-only budget snapshot reports exact title/system/charged numbers. A testing-only configurator can set tiny synthetic budgets when no host heap charge is live; it is never a guest DLL export.
- The system side is **transferred capacity accounting**, not actual system process memory allocations.

**Limitation:** The ledger currently enforces only the host-owned XMemAlloc heap path. XMemVirtualAlloc, synthetic 64-KiB physical pages, GPU tiles, sparse 16/24-GiB VA reservations, APU heaps and unrelated native allocations are not charged. This is not complete Xbox physical-memory partition management. XMemGetWorkingSetStatistics continues its safe failure-only behavior because its output layout is not verified.

## Synthetic regression testing

    .\build\x64\Release\ScarlettVI.exe xmem-transfer-fixture
    .\build\x64\Release\ScarlettVI.exe xmem-stats-fixture
    .\build\x64\Release\ScarlettVI.exe xmem-tools-fixture
    .\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'

The new original test file src/runtime/xmem_transfer_test.cpp verifies the MS x64 two-argument BOOL function pointer, exact export names, invalid directions/sizes, no partial budget mutation, zero-size no-op and unchanged Win32 error, and overdraw rejection.

It sets a tiny 6-MiB title/4-MiB system policy, makes actual writable 3-MiB and 1-MiB heap allocations, transfers 2-MiB increments, demonstrates denied allocations against the newly reduced title limit and frees allocations to restore quota. Four concurrent host workers request sixteen 2-MiB transfers against a 10-MiB title capacity; **exactly five** succeed with no double-spend and final quota totals unchanged. The original host budget is restored after the fixture.

No original game executable, DLL initializer, audio code or asset executes, and no proprietary or upstream GPL code was copied.

## Readiness

The read-only title audit now reports **15 host XMem imports bound**, including **one error-only** and **one budget-partial**. XMemTransferMemory is no longer an unresolved import. Total unbound Xbox imports decrease from **10 to 9**: CHAD five, XAPU enqueue one, XG two and Xbox DirectStorage one.

**GTA VI is NOT bootable through ScarlettVI.** Readiness continues to enforce TITLE EXECUTION: BLOCKED; binding a partial memory host service does not implement Xbox graphics, storage or startup lifecycle.
