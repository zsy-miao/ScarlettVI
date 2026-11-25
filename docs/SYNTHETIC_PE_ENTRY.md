# Generated x64 PE64 entrypoint smoke test

**Status: PASS on the current Windows host.** ScarlettVI now runs an entirely generated, original PE32+ x64 function through its own mapper. This is a synthetic executable, *not* title execution. Real-title execution remains blocked.

## What executes

`src/loader/synthetic_entry_test.cpp` constructs a fixed, minimal 0x800-byte PE file with three sections (RX `.text`, RW `.data`, RO `.reloc`) under the Git-ignored `reports/` folder. The file is removed by RAII after the smoke test. It contains no game code, proprietary DLLs, network code or untrusted instructions.

The known x64 entrypoint calls a single Windows API using its own PE Import Address Table, then returns: `GetCurrentProcessId`. Another generated function is registered as a TLS callback which would modify a marker if invoked, but remains **deferred** and is not executed.

## End-to-end features actually tested

1. `PeFile` parses a valid PE32+ x64 header, three sections and a named import descriptor for `KERNEL32.dll!GetCurrentProcessId`.
2. A temporary `VirtualAlloc(MEM_RESERVE)` holds the preferred image base, forcing the real `MappedImage` constructor to relocate. Six genuine DIR64 relocations are applied (one data pointer, four TLS directory absolute addresses and a TLS callback VA), and the relocated pointer is checked against the new mapped base.
3. `ImportRouter::bind_windows` obtains the real host export and patches one actual named IAT slot. The test confirms one Windows symbol is bound and no unknown/Xbox imports are pending.
4. `MappedImage::finalize_image` applies section permissions, allocates and populates one Windows-managed host TLS slot (not automatic guest compiler TLS in the TEB), initializes 8 bytes of template data plus 8 zero bytes, validates but defers exactly one callback, and refreshes the host instruction cache.
5. The generated entrypoint executes and returns the actual Windows process ID. The callback marker remains unchanged, proving no deferred TLS callback ran accidentally.
6. Import-table writes after finalization are rejected, and destructor cleanup releases mapped memory and TLS state.

## Generated DLL attach/detach and worker-thread TLS follow-up

A second test, `synthetic-dll-fixture`, uses the **same fixed PE generator**
with a DLL image flag and a generated DLL entrypoint. Its only generated
TLS callback increments an in-image counter; the generated `DllMain`
reads that counter, records the supplied reason and returns TRUE.

The test explicitly invokes the generated callback **before** `DllMain`
on process attach and invokes `DllMain` **before** the callback on
process detach. It verifies both reasons and the counter sequence. These
calls are deliberately scoped to test-generated code; the general mapper
still **defers all callbacks and never invokes game initializers**.

The mapper now provides `attach_current_thread_tls()` and
`detach_current_thread_tls()` for a **managed host-created worker thread**.
Each thread has an independently copied TLS template, with a mutex
protecting allocation and cleanup. Four generated worker tests exercise
32 attach/mutate/re-attach/detach rounds each; main-thread template and
contents stay unchanged. Image users must join and detach worker threads
before destroying the mapped image.

**Important limitation:** The host TLS slots are populated through
`TlsAlloc/TlsSetValue`. This does *not* wire the guest's compiler-style
static TLS access into the Windows TEB, does not automatically install
TLS for arbitrary game-created threads, and does not implement the guest
DLL dependency initialization order or thread notifications. Generated
`DllMain` and TLS callbacks are invoked by the fixture *only*.

## Run

    .\build\x64\Release\ScarlettVI.exe synthetic-entry-fixture
    .\build\x64\Release\ScarlettVI.exe synthetic-dll-fixture

The source bytes are generated within the test. Nothing in this command reads or executes the original game build. The temporary fixture is written only under `reports/` and deleted before the command returns.

## Remaining blockers to running a real title

This test establishes only a tiny known-code execution path. It does **not** implement executable startup ABI for a full Xbox game. Next prerequisites include actual automatic guest DLL dependency attach/detach, native compiler static TLS in the TEB and its thread lifecycle, callback scheduling, runtime-guest service initialization, safe exception trapping and crash logs, virtual title filesystem, console-compliant HAL/XAPU/CHAD behavior, and console shader translation to DXIL / D3D12 resource semantics.

`readiness` remains data-only and continues to report 14 unresolved Xbox import references and `TITLE EXECUTION: BLOCKED`. Do not confuse this synthetic-entry PASS with any attempted GTA VI boot.
