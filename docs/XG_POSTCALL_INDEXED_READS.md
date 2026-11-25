# XG +0x10 post-call indexed stack reads and field copies

**Scope:** original bounded, read-only x64 instruction-pattern research on file-backed PE bytes. No title code is executed. These are candidate caller-side accesses, **not** proof of an Xbox interface, pointer alias, actual callee outputs, live register values, execution path, memory ownership, or texture format contract.

## Observations corroborated with MSVC Dumpbin

The indirect `+0x10` dispatch at VA `0x140979C9F` follows an RDX stack-address setup at `0x140979C97`, where RDX is computed as `RSP+0x58`. The caller then computes an index using `MOVSXD R14,EBP` and `IMUL RAX,R14,0x70` immediately after the dispatch. This is evidence of a scaled record-position calculation in the caller, **not** proof of a returned array length or callee-written record.

The read-only disassembly and ScarlettVI's new scanner agree on three subsequent indexed-stack loads and three adjacent copies into another structure:

| Indexed load VA | Source access | Offset relative to RDX stack address | Width | Adjacent copy VA | Destination |
| --- | --- | ---: | ---: | --- | --- |
| `0x140979CAC` | `[RSP+RAX+0xC4]` | `+0x6C` | 4 bytes | `0x140979CB3` | `[RSI+0x10]` |
| `0x140979CB6` | `[RSP+RAX+0xA8]` | `+0x50` | 8 bytes | `0x140979CBE` | `[RSI+0x38]` |
| `0x140979CC2` | `[RSP+RAX+0xB8]` | `+0x60` | 8 bytes | `0x140979CCA` | `[RSI+0x40]` |

The fixed offsets all lie within `0x70` bytes beyond the RDX stack address, and the caller independently uses a `0x70` multiplier when calculating RAX. The evidence is **consistent with** the caller reading fields inside indexed records after the dispatch, but it cannot establish the producer, initialization state or complete descriptor layout.

The other observed `+0x10` call, at VA `0x145121A84`, uses RDX=`RSP+0x60` and is followed immediately by a `+0x08` indirect call. It therefore has **no matches before that next indirect call** under the scanner's conservative boundary. This is not evidence that its output parameter is unused or that it behaves differently; other reads may occur elsewhere.

## Reproducible commands

Run inside the `ScarlettVI` directory:

```powershell
.\build\x64\Release\ScarlettVI.exe xg-vcall-reads-fixture
.\build\x64\Release\ScarlettVI.exe xg-vcalls '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

`src/analysis/xg_vcall_indexed_reads.{h,cpp}` validates supplied `FF /2` indirect-call metadata and separately reuses the prior `LEA RDX,[RSP+disp]` argument scanner. It scans **at most 96 file-backed bytes after a `+0x10` call**, stopping at the start of a subsequent independently verified indirect call. Only the narrow `MOV r32/r64,[RSP+RAX*1+positive disp8/disp32]` encoding is recognized; the displacement must be within `0x70` bytes above the previously observed RDX stack-address offset. The `0x70` bound is an intentionally local search filter, **not a general Xbox record-size claim**.

For each recognized stack load, an optional second pattern checks that the *immediately following instruction* moves the loaded GPR to `[RSI+nonnegative disp8]` at the same width. The analyzer does not assert any broader reaching definitions or memory provenance, and only looks for non-indexed RSI field stores with no other intervening instruction. Matched instructions are skipped completely to avoid treating a REX-prefixed opcode as a second 32-bit pattern.

The separate fixture uses original generated instruction bytes, **different buffer offsets from the game**, and checks mixed 32-/64-bit loads, extended registers, immediate matching and mismatching destination stores, intervening instructions, negative and indexed addressing, RDX prep absence, 96-byte horizon, intervening-call barrier, duplicated input, malformed candidates, bounds rejection and atomic failure. An earlier byte-pattern scanner can misread the final byte of certain synthetic instructions as a redundant REX prefix before a following call: the tests use independently unambiguous synthetic boundaries, and results from real call sites are corroborated with Microsoft's disassembler. These limitations prevent treating raw byte matches as verified instruction-control flow.

No proprietary game code, original title instruction streams, SDK files or game assets are placed in GitHub. This work does not bind `xg_xs.dll` imports; **TITLE EXECUTION remains BLOCKED**.

See [XG dispatch successors](XG_DISPATCH_SUCCESSORS.md), [XG dispatch arguments](XG_DISPATCH_ARGUMENTS.md), [XG register timeline](XG_REGISTER_TIMELINE.md), and [startup readiness](STARTUP_READINESS.md).
