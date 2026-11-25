# Original host CPU DSP convolution backend

**Status: real, tested CPU audio signal processing. Not a bound Xbox `chad.dll` service, not XAPU command execution or audio playback.** ScarlettVI now implements an original finite-impulse-response (FIR) filter in C++20, backed by genuine Windows CPU floating-point operations on independently generated input samples.

## Why this is relevant to CHAD

Public [Microsoft GDK error-code definitions](https://learn.microsoft.com/en-us/xbox/gdk/docs/reference/errorcodes) refer to `XDSP_E_INVALID_IMPULSE_RESPONSE_LENGTH`, `XDSP_E_MAX_AGGREGATE_IMPULSE_RESPONSE_EXCEEDED` and `XDSP_E_NOT_ALL_HANDLES_DEACTIVATED`. Those errors establish that impulse-response lengths and stream activation lifecycles matter to the console DSP interface. They do **not** identify game-provided packet layouts or confirm the guest's exact PCM format.

Read-only original-title PE disassembly also established the *actual* five CHAD thunk/IAT pairs, preventing confusion with nearby Xbox imports:

| Export | Thunk | Game IAT | Verifiable direct call |
| --- | --- | --- | --- |
| `XDspActivate` | `0x145F3FEF0` | `0x146793F30` | `0x1411D475D`: five-argument wrapper with return status `EAX==0` |
| `XDspConnectWithMaximumStreamLimit` | `0x145F3FF00` | `0x146793F38` | `0x145B5262E`: RCX client handle, EDX stream count, R8D value, R9D `0x80`, stack DWORD `1`, stack pointer to guest result at caller offset `+0x488`; EAX zero tested |
| `XDspDeactivate` | `0x145F3FF10` | `0x146793F40` | No confirmed direct `E8` caller (not proof of no indirect use) |
| `XDspDisconnect` | `0x145F3FF20` | `0x146793F48` | No confirmed direct `E8` caller (not proof of no indirect use) |
| `XDspSubmitCommand` | `0x145F3FF30` | `0x146793F50` | `0x144E5B20D`: RCX and RDX opaque pointers, EAX checked for zero |

The register and address evidence was collected only by reading the protected game file with Microsoft's `dumpbin` and an independently written PE thunk mapper. **No guest code ran, and no game bytes were copied into the project.** Callsites do not prove whether guest pointers are resources, clients, streams or command descriptors. Consequently the CHAD imports remain **unbound**. Routing these imports to unconditional `S_OK` would hide the missing hardware behavior, not implement it.

## Signal processing that now genuinely works

Source: `src/audio/dsp_convolution.{h,cpp}`. Standalone original C++20 `HostDspConvolver` supports:

- Mono through eight interleaved floating-point channels and kernels from one through 4,096 float taps; 4,096 input frames maximum per processing call. These caps are conservative **host implementation policies**, not Xbox hardware limits.
- Real, direct FIR convolution: for each sample, `output[n] = sum(kernel[k] * input[n-k])`; each channel maintains its own circular history across independent calls. A sample written at the current ring position is included in the current output.
- A lock-protected, transactional `configure` operation: invalid channel counts, missing kernels, oversized kernels or non-finite coefficients do not overwrite a previously valid stream. Reconfiguring deliberately resets processing history.
- Lock-protected `process`: rejects malformed interleaving, too-short output, non-finite input, too-large frame count or arithmetic overflow. It calculates the entire block in scratch storage and commits stream history and output only when valid; input and output may be the same span.
- A `reset()` operation clears channel histories while retaining the kernel; a checked 64-bit processed-frame counter tracks real work. No background worker or guest event callback is fabricated.
- A low-overhead decrement/wrap inner loop rather than division/modulus per tap. Direct FIR remains `O(frames × channels × taps)`, so long reverberation impulses will eventually need a partitioned FFT or other optimized backend for realtime performance.

**This is not Xbox SHAPE/XAPU/CHAD ABI emulation.** No address from the game's DSP heap is dereferenced; no guest client, stream, codec or result has been decoded; no system sound device is opened. The filter is an independently tested processing component that can later be integrated with exact verified command descriptors.

## Reproduce

    .\build\x64\Release\ScarlettVI.exe audio-dsp-fixture
    .\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'

The `audio-dsp-fixture` uses only generated coefficients and samples. It verifies mono impulses `{1, 0.5, 0.25}`, in-place processing, streaming across differently sized chunks, independent stereo channels, reset, invalid kernel sizes and channel counts, NaN/Inf rejection with unchanged state, output buffer canaries, maximum eight-channel/4,096-frame identity processing, rejection of a 4,097-frame call, float-overflow rejection without writes, and four threads concurrently running 128 blocks of 32 samples each through a shared identity filter. The fixture tests actual numerical outputs, not only return codes.

## Remaining startup blockers

At this milestone the Xbox import count remains **10**: `chad.dll` five (all still unbound), `XApuEnqueueCommand` one, `xg_xs.dll` two, Xbox DirectStorage one, and `XMemTransferMemory` one. Existing XAPU connection/result handling and AcpHal HAL factory are still partial. The native GPU shader translator, device memory semantics, title filesystem and process lifecycle are also incomplete. **`TITLE EXECUTION: BLOCKED`** remains correct.
