# CHAD / XDSP host-client and guarded FIR bridge

**Status: all five observed `chad.dll` imports have host entry points;
Xbox DSP stream activation, packet decoding, and hardware audio remain
UNIMPLEMENTED. This does not boot the game.**

The original, tested CPU FIR convolver is now reachable through an
**explicitly tagged host-generated XMem command** via
`XDspSubmitCommand`. The bridge never treats an unrecognized Xbox
command as successful.

## Exact import coverage

| Xbox import | Current behavior |
| --- | --- |
| `XDspConnectWithMaximumStreamLimit` | Creates a real owned, bounded CPU client and gives back an opaque unique host handle |
| `XDspDisconnect` | Disables the owned client; rejects stale or foreign handles |
| `XDspSubmitCommand` | Executes only a `HostXDspFirCommandV1` payload from mapped XMem CPU pages; unknown Xbox commands return `E_NOTIMPL` |
| `XDspActivate` | Explicit `E_NOTIMPL`: no actual Xbox DSP stream creation |
| `XDspDeactivate` | Explicit `E_NOTIMPL`: no fabricated stream teardown |

The title's read-only disassembly shows a six-position Connect caller:
RCX carries an opaque value, **EDX a 32-bit hint**, R8D the stream
limit, R9D `0x80`, then a stack DWORD `1` and an output pointer.
These signatures are **provisional** (especially the meaning of
the opaque values), not a full Xbox GDK contract.
The observed `XDspSubmitCommand` wrapper passes two pointers.
An Activate wrapper appears to forward five opaque arguments,
but their meanings and stream ownership are unverified.

The existing C++ host registry uses a mutex and 64 total lifetime
client handles; disconnected clients become permanent tombstones so a
stale handle can never be reused. The caps, checks and requirement for
connect flags `1` are original conservative host policies, not
claims about Xbox hardware resource limits. No opaque Connect input
address is dereferenced, even if its numerical value resembles a
pointer.

## Tested generated FIR subset

`HostXDspFirCommandV1` is an original, **48-byte** desktop test
structure. It uses magic `0x31534448` (`HDS1`), version 1,
reserved-zero fields, input/output pointers, sample count and host
receipt ID. It is **not** an Xbox command packet. The host-generated
test supplies an actual identity FIR kernel with
`configure_host_xdsp_fir`, a helper **not exported as an Xbox API**.

`XDspSubmitCommand` then:

1. Validates the client against the owned handle registry and copies
   the entire tagged command only from mapped ScarlettVI-owned XMem
   CPU aliases, even when the command crosses a 64KiB page boundary.
2. Refuses any unknown command, version, reserved fields, unsafe
   pointers, or nonpositive/oversize sample count.
3. Copies all input samples from verified XMem aliases. A wholly or
   partially unmapped input cannot start processing.
4. Resolves and pins every output CPU page through the existing
   [DirectStorage XMem pinning primitive](XBOX_DSTORAGE_CPU_ALIAS.md).
   A mismatched or unmapped page rolls back all pins and writes no
   output.
5. Processes real interleaved floating-point samples synchronously in
   `HostDspConvolver`. Pins remain held for the full processing
   call and are released on both success and failure. Only actual
   successful FIR work increments the per-client completion counter.

This generated-only host path does **not** publish Xbox DSP results,
invoke callbacks, allocate Xbox stream resources, emulate the console
DSP microcode, or play sound. Xbox commands remain `E_NOTIMPL`.

## Verification

```powershell
.\build\x64\Release\ScarlettVI.exe audio-chad-fixture
.\build\x64\Release\ScarlettVI.exe audio-dsp-fixture
.\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

The generated CHAD fixture checks all five exact symbol lookups,
bad arguments and unique handle ownership, unconfigured DSP and no
fake completion, unknown command magic, request/input/output mapping
failures, exact identity FIR output across XMem page boundaries,
pin release, disconnected-handle rejection and zero leaked XMem
frames. It never executes title code or reads title asset data.

[Microsoft's public GDK error-code reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/errorcodes)
distinguishes real XDSP queue full, pending results, resource exhaustion
and activation lifecycle errors. Those meanings are further evidence
that unconditional `S_OK` stubs would be misleading.

## Startup readiness

The five CHAD IAT references can be bound to these host entry points.
Readiness must label them separately as **2 host-client partial,
1 tagged host FIR-only, 2 error-only**. A total of zero *unresolved
addresses* is not a complete implementation. Xbox GPU texture layout,
shader translation, guest VFS, actual audio stream lifecycle and title
initialization remain substantial barriers; the diagnostic tool still
does **not** execute original game code.
