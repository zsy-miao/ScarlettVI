# XMem working-set statistics: safe unsupported desktop path

## Implemented behavior

ScarlettVI now resolves the exact named `xmem.dll!XMemGetWorkingSetStatistics` import using an original C++20 host error path with the observed two-argument x64 calling signature:

```cpp
BOOL WINAPI XMemGetWorkingSetStatistics(uint32_t selector, void* output);
```

This does **not** provide Xbox working-set or memory-budget accounting. With a non-null output pointer the function returns `FALSE`, sets `ERROR_NOT_SUPPORTED`, and **writes no bytes** to the caller's buffer. With a null output it returns `FALSE` / `ERROR_INVALID_PARAMETER`. It does not allocate or transfer memory, and it does not fabricate an Xbox budget or output layout.

## Why a failure path is safer here

XCL documented a real stack corruption caused when an earlier 0x78-byte Xbox-statistics output was written into a smaller, older-GDK caller buffer. Xbox2Windows independently reported a selector `0x20000001` and a BOOL-style success check at AMD middleware callsites, but neither establishes complete selector semantics or the structure's version-independent size. On our desktop host no correct general budget is available.

A successful function pointer binding is *not* proof that game code can tolerate the error path, and it is **not a complete import implementation**. Do not replace this failure with a guessed successful response.

The original implementation is in `src/runtime/xmem_bridge.cpp` and exported only by exact name through `xmem::find_export`. No proprietary Xbox headers, DLL code or copyleft source code was copied.

Public research:
- https://github.com/xcl-dev/xcl/blob/main/HANDOFF.md
- https://github.com/R0Y00/Xbox2Windows/blob/main/docs/experiments.md

## Reproduce

```powershell
.\build\x64\Release\ScarlettVI.exe xmem-stats-fixture
.\build\x64\Release\ScarlettVI.exe xmem-tools-fixture
.\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

`xmem-stats-fixture` verifies the exact export pointer and calling signature; selector, null-pointer and error-state handling; and 256 generated selector/buffer-length pairs. For each case it preserves an independently patterned 96-byte region, including canaries surrounding output pointers of sizes 1 to 64 bytes. It checks that the entire region remains bit-identical on failure.

The readiness console output now explicitly distinguishes the error-only link as `XMem=14 (error-only=1)`, with JSON field `experimental_xmem_error_only_bound: 1`. This makes the reduced count auditable without implying a working statistics service.

The router **keeps `XMemTransferMemory` unresolved**. For the current read-only build, this change lowers pending Xbox import addresses from 24 to 23, leaving one XMem import, 19 APU/audio/DSP imports, two XG imports, and one Xbox DirectStorage factory import. Even with zero pending imports, the unimplemented runtime milestones would keep title execution BLOCKED.
