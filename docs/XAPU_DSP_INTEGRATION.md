# Host XAPU-to-DSP command processing integration

**Status: real host-typed DSP execution and completion receipts.** A [new tagged XMem host FIR bridge](XAPU_XMEM_GUEST_FIR.md) connects generated command records through the `XApuEnqueueCommand` import, but **real Xbox commands still return `E_NOTIMPL`**. This is not a console command decoder or working game-audio playback path.

## Why the host-only boundary exists

Read-only `dumpbin` of the supplied Xbox executable confirms `XApuEnqueueCommand` receives RCX=connection, RDX=command pointer, and R8D=0 at four verified sites: `0x1400483D5`, `0x144EE9978`, `0x144F2092D`, `0x144F20BB8` (the thunk resolves to IAT `0x146794158`). A first DWORD with values **1, 2, or 5** is visible in several generated command buffers. The rest of these records contain additional ID, format, routing and payload fields whose effects and completion layouts are not independently established. In particular, the opcode is **not** proof that a command describes a CPU FIR filter, or that a guessed completion would be valid.

The actual Xbox command packet remains unsupported rather than returning a no-op or copying a fabricated completion. The original title's executable and DLLs are never invoked or modified by the host tests.

## What the original host implementation does

`src/audio/xapu_host.cpp` now owns a real `HostDspConvolver` for every live `HostXApuConnection`, in addition to its existing bounded result FIFO. The new **host-only**, non-DLL-exported C++ APIs are:

- `configure_host_xapu_fir(handle, kernel, channels)`: installs an actual finite-impulse-response kernel for that connection. It uses the existing filter validation; an unknown/stale connection is rejected, and a connection with pending completion records is reported BUSY instead of allowing old-generation results to be mistaken for current ones.
- `submit_host_xapu_fir(handle, input, output, command_id)`: checks the real connection and FIFO capacity, runs the real multichannel DSP filter, and records a host-format completion **only if** processing succeeds. Invalid/non-finite data or pre-configuration submissions do not modify the result queue. Full queues return `XAPU_E_QUEUE_FULL (0x8AC81000)` before processing, without altering audio output or filter history.
- The **existing** `XApuGetResultCount` and `XApuDequeueResult` guest-named imports can observe these genuine host-generated completions, including FIFO order. No fabricated title-command result is ever issued.

The single process-wide XAPU mutex protects each DSP operation and its FIFO update as one transaction. This guarantees that a disconnect, queue-full transition, reconfiguration or other submit cannot occur between successful processing and receipt insertion. It is deliberately conservative; multithreaded per-connection DSP scheduling and realtime queues will require a more specialized locking design once the guest scheduling model is verified.

### Host-only completion layout

This exact `XApuResultEnvelope` layout is **invented by ScarlettVI solely for host-generated tests**, and MUST NOT be interpreted by the real game as an Xbox hardware result:

| `words[]` index | 32-bit field |
| --- | --- |
| 0 | Magic `0x52494653` (`SFIR`, little-endian) |
| 1 and 2 | Low/high 32 bits of submitted 64-bit host command ID |
| 3 | Actual processed frame count |
| 4 | Actual channel count |
| 5 | FNV-1a checksum over bitwise IEEE-754 float representations of the *processed output samples* |
| 6 and 7 | Low/high 32 bits of cumulative processed frame count |
| 8–19 | Zero/reserved |

## Executable tests

    .\build\x64\Release\ScarlettVI.exe audio-xapu-dsp-fixture
    .\build\x64\Release\ScarlettVI.exe audio-xapu-fixture
    .\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'

`src/audio/xapu_dsp_integration_test.cpp` verifies exact stereo signal samples and cumulative frame receipts, persistent filter history across commands, rejected unconfigured submissions, invalid-impulse rollback, BUSY while results remain, NaN/Inf rejection with untouched output, genuine full-FIFO rejection without history mutation, FIFO dequeue/flush ordering, real in-place output and checksum, unknown and stale handle rejection, and four concurrent producers × eight actual identity-filter processing commands into one shared completion queue. It checks all 32 unique receipts and their per-command output checksums.

Only generated samples and buffers are used. This does not read or execute any game assets, DSP firmware, proprietary DLLs or title instructions.

## Readiness and priorities

Current readiness is **5 pending Xbox import addresses**, all from CHAD DSP. `XApuEnqueueCommand` is bound only for ScarlettVI-generated XMem host FIR commands; real Xbox packets remain unsupported. The other four XAPU connection/result imports remain partial and both XG bindings are error-only. **`TITLE EXECUTION: BLOCKED` remains unchanged.**

The next productive step is determining real guest XAPU command sizes, field ownership, IDs and completion semantics, mapping the supported processing subset to this host filter, and separately completing CHAD stream activation and lifecycle. None of these interfaces should claim `S_OK` without performing the corresponding operation.