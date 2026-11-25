# XMem-backed XAPU host FIR command bridge

**Status: one real generated host FIR command can now enter via the
guest-named `XApuEnqueueCommand` import. This does NOT decode actual
Xbox XAPU command packets, play game audio, or allow native title boot.**

## Strict host command format

`HostXApuFirCommandV1` is an **original ScarlettVI-defined** 48-byte
generated-test structure. Its fields are:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | uint32 | Magic `0x31464953` (`SIF1`) |
| 4 | uint16 + uint16 | Version 1, reserved zero |
| 8 | const float* | Input CPU sample span |
| 16 | float* | Output CPU sample span |
| 24 | uint32 + uint32 | Float sample count, reserved zero |
| 32 | uint64 | Host-only command receipt ID |
| 40 | uint64 | Reserved zero |

The title's real XAPU record shape is not established; its observed
first DWORD often has values 1, 2, or 5, not this host magic.
**Unknown real title commands intentionally receive `E_NOTIMPL`.**
There is no fallback to fake result packets.

## Actual execution

A generated host test first calls the existing `XApuConnect` and
`configure_host_xapu_fir` to install a real FIR kernel. The observed
`XApuEnqueueCommand(connection, command, flags)` three-argument ABI
then takes a strictly gated path:

1. Requires zero flags, snapshots the full 48-byte command **only from
   mapped ScarlettVI-owned 64KiB XMem CPU pages**, using
   `PhysicalMemory::snapshot_mapped_alias` under the mapping mutex.
   A pointer from the ordinary process heap/stack is never dereferenced.
2. Verifies exact magic/version/reserved fields, requires aligned
   nonzero sample pointers and caps the request to 16,384 floats.
3. Copies all input samples through the same mapping-safe snapshot API,
   so a partially unmapped or invalid input never starts DSP.
4. Validates **every** intersected output page and pins all XMem alias
   identities, rolling back on stale/missing mappings. All pins stay held
   while the original `HostDspConvolver` processes real samples.
5. Under the existing XAPU connection lock, atomically runs the FIR
   operation and adds a **host-defined** completion packet to its FIFO.
   The existing `XApuGetResultCount` and `XApuDequeueResult` return
   the actual resulting receipt. The output is written to the verified
   mapped CPU destination, and the pins release after processing.

The original result magic `SFIR`, sample checksum and frame count are
**not** claimed to be Xbox hardware packets. The bounded queue refuses
a full-capacity submission before any processing/output writes.

## Regressions

Run `ScarlettVI.exe audio-xapu-guest-fir-fixture`. The generated
five-page XMem fixture places the command header across one page boundary,
the input across another, and the output across a third. It verifies
byte-exact float output for an identity FIR, a real completed receipt
with expected command ID/frame/channel fields and output checksum,
zero outstanding physical alias pins, and no leaked XMem frames.

Negative cases cover arbitrary and stack-owned command pointers,
nonzero flags, wrong host magic, unconfigured DSP, unowned input/output,
unmapped command pages, unmapped input and destination pages, a full
completion queue (no output modification) and a disconnected handle.
No title code, Xbox DSP binary, original game asset or proprietary
library is executed.

## Remaining blockers

The real title's command packet layout, stream scheduling, sound mixing,
DMA/SHAPE configuration, Xbox output semantics and CHAD lifecycle are
unimplemented. Even with this import now linked, readiness stays
**BLOCKED** because the bound function returns `E_NOTIMPL` for real
Xbox commands and further graphics/audio/startup services are absent.
