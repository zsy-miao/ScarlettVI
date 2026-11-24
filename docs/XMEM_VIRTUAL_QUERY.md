# Experimental XMemVirtualQuery compatibility bridge

## Evidence: read-only analysis of supplied XboxBvhBuilder.dll

This bridge is independently authored C++20. Its calling convention and only
the fields needed by this particular bundled DLL were derived from the
**user-provided** `XboxBvhBuilder.dll`, examined **without changing it**.
No proprietary Xbox headers, code, DLLs or title binaries are committed.

The PE import table contains `xmem.dll!XMemVirtualQuery` at IAT VA
`0x180024260` (module image base `0x180000000`, RVA `0x24260`).
Four direct call sites were observed in the original DLL at:

| Call site VA | Observed preparation | Observed result |
| --- | --- | --- |
| 0x180019C69 | RCX: GetCurrentProcess, RDX: candidate address, R8: 64-byte stack output | EAX is tested, then output bytes |
| 0x180019CCB | same 3-argument pattern | EAX, +0x30 access bits, +0x20 State |
| 0x180019D9D | same 3-argument pattern | EAX, +0x30 access bits, +0x20 State |
| 0x180019DF9 | same 3-argument pattern | EAX, +0x30 access bits, +0x20 State |

Before each query, two 32-byte AVX stores zero the destination stack
buffer. No fourth parameter is deliberately prepared. The caller checks
`eax != 0`, `(byte[buffer+0x30] & 3) != 0`, and
`(dword[buffer+0x20] & MEM_COMMIT) != 0`. One call additionally tests
`(byte[buffer+0x24] & 6) != 0` for the Win32 protection state.
The first 48 bytes thus match x64 `MEMORY_BASIC_INFORMATION`:
`State` at +0x20, `Protect` at +0x24.

## Original implementation

`src/runtime/xmem_query.{h,cpp}` implements the independently observed
3-argument ABI as:

```cpp
BOOL WINAPI XMemVirtualQuery(
    HANDLE process, const void* address, VirtualQueryInfo* output);
```

The 64-byte output uses the standard 48-byte Windows
`MEMORY_BASIC_INFORMATION` prefix followed by two zero-initialized
64-bit fields. `VirtualQueryEx` obtains the real host memory state,
including placeholder-backed 64 KiB pages created by ScarlettVI.

The low bits of the additional field at offset 0x30 **are not publicly
documented as Xbox semantics**. Our explicit Windows compatibility policy
is bit 0 = CPU-readable committed memory and bit 1 = CPU-writable committed
memory. Reserved, no-access and guard pages return neither bit.
This is only a **tested approximation** for the inspected BVH call sites,
not a console-correct general implementation.

## Verified local checks

`ScarlettVI.exe memory` now verifies the query against a real Windows
pagefile-backed, 64 KiB aliased memory view: the query returns
`MEM_COMMIT`, `PAGE_READWRITE` and both desktop access flags.
The read-only `readiness` pass on the supplied title now binds
`xmem.dll!XMemVirtualQuery` from the nested `XboxBvhBuilder.dll`,
reducing remaining Xbox import references to **28**.

No game code or DLL initializer was executed; actual BVH runtime behavior
remains unverified.

## Remaining compatibility risks

The exact meaning of the Xbox-specific field at +0x30, cross-process
permissions, per-memory-type flags and physical/GPU memory semantics
cannot be established solely from these call sites. Any new caller with
different requirements must be analysed before extending this bridge.

Microsoft's [XMem documentation overview](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/gc-reference-system-toc)
lists parts of XMem under restricted access. No restricted documentation or
SDK materials were used.
