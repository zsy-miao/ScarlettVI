# Partial XAPU connection and result-queue service

**Status: original Windows-hosted connection/result FIFO with real ownership, but NO guest command execution.** The Xbox title cannot boot with this service alone. `XApuEnqueueCommand` remains **unbound** until its command structures, asynchronous completion contract, and effects can be verified and implemented.

## Direct title caller evidence

Using Microsoft `dumpbin /disasm:nobytes` on the adjacent user-provided game executable **read-only**, the named thunk mappings are:

| Exact `xapu.dll` export | Thunk / callsite(s) | Verified caller shape |
| --- | --- | --- |
| `XApuConnect` | thunk `0x145F40110`, call `0x1443AEE69` | RCX and RDX are two caller-owned descriptor pointers; R8 is a writable `HostXApuConnection**`; EAX is checked for negative HRESULT |
| `XApuDequeueResult` | thunk `0x145F40120`, call `0x144EAA89A` | RCX is saved connection handle; RDX points to a stack result buffer; EAX checked for negative HRESULT; result fields read at +0 and +4 |
| `XApuDisconnect` | thunk `0x145F40130`, call `0x1443AE930` | RCX is saved connection handle; caller then zeroes that handle |
| `XApuEnqueueCommand` | thunk `0x145F40140`, four calls including `0x1400483D5` and `0x144EE9978` | RCX handle, RDX command pointer, R8D=0; command DWORD at +0. Actual audio command effects are not verified; remains pending |
| `XApuGetResultCount` | thunk `0x145F40150`, call `0x144EAA86B` | RCX handle and EDX=0; returns EAX as number of queued completions |

The inspected result caller clears at least 0x50 bytes of stack output and interprets several fields. `XApuResultEnvelope` is a zero-initialized, original 0x50-byte layout used for **synthetic test-generated completions only**. This is not a claim that the full Xbox completion union is understood. None of the two opaque Connect descriptors are dereferenced or retained because their fields are unverified.

Microsoft's public [Game Development Kit error-code reference](https://learn.microsoft.com/en-us/xbox/gdk/docs/reference/errorcodes) documents `XAPU_E_PENDING_RESULTS = 0x8AC81001` for a currently empty queue and distinguishes XAPU queue-full and invalid-command errors. No restricted Xbox GDK header, game bytes, or external GPL implementation source was copied.

## Actual host-backed service

Original files `src/audio/xapu_host.{h,cpp}` and `src/audio/xapu_host_test.cpp` provide:

- `XApuConnect`: accepts two non-null opaque descriptors and a writable out-connection pointer; creates a real, exclusively owned host connection with a 64-entry result FIFO and returns an HRESULT. Reports invalid arguments and out-of-capacity errors rather than making up a connection.
- `XApuGetResultCount`: reports the **actual number** of pending FIFO entries in the owned connection when selector is zero. Empty queues give count zero, stale/unknown handles cannot become valid.
- `XApuDequeueResult`: returns `XAPU_E_PENDING_RESULTS` on an empty live connection without changing the caller's output bytes; otherwise copies an actual queued synthetic completion and removes it in FIFO order. Rejects unknown handles and null output.
- `XApuDisconnect`: invalidates the connection, drops pending results, and disallows all later actions through the handle. Handles are retained as bounded tombstones for the process lifetime, so stale pointers cannot be silently reused for a different connection.
- Process-wide caps: 64 total connection handles ever, 64 results per live connection. These are conservative **desktop host policies**, not Xbox hardware limits. The host mutex protects all connection and result operations.
- A **fixture-only** helper can enqueue an explicitly generated result to test full/empty, wraparound, FIFO and concurrent behavior. A subsequent [host-only real FIR processing bridge](XAPU_DSP_INTEGRATION.md) can also submit typed host commands and queue verified, actual processing receipts. Neither helper is exposed as an Xbox guest command API.

`XApuEnqueueCommand` is **not** bound; real *guest* XAPU audio command decoding, activation, stream scheduling and Xbox DSP execution are unimplemented. The host-only FIR bridge processes original generated commands, not title commands. As a result the game may require commands this subset cannot execute. These four imports are counted separately as `experimental_xapu_partial_bound=4`, not successful device emulation.

## Verification

    .\build\x64\Release\ScarlettVI.exe audio-xapu-fixture
    .\build\x64\Release\ScarlettVI.exe audio-xapu-dsp-fixture
    .\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'

The synthetic fixture validates the four exact exported C signatures; verifies `XApuEnqueueCommand` stays unbound; checks null/missing arguments, foreign and stale handles, no-write empty queue errors, 64-entry capacity, FIFO across 64 records, 100 wraparound cycles, per-connection isolation, full state after disconnect and four worker threads with 8 real connect/queue/dequeue/disconnect cycles each. It verifies all pending synthetic test results and live connections return to initial counts. No original title code runs.

## Readiness delta

| | Before | Now |
| --- | ---: | ---: |
| Pending `xapu.dll` references | 5 | **1** (`XApuEnqueueCommand`) |
| Pending all Xbox references | 14 | **10** |
| Partial XAPU connection/result addresses | 0 | **4** |
| Actual audio command execution | absent | **absent** |
| Real title execution | BLOCKED | **BLOCKED** |

The other 9 unresolved references are CHAD DSP (5), XG graphics (2), Xbox DirectStorage (1), and XMem transfer (1). The remaining command processing and Xbox graphics/storage infrastructure cannot be replaced with no-op successes.
