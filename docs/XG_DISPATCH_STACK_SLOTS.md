# XG outgoing stack-write evidence

**Scope:** bounded, read-only x64 byte-pattern analysis. No guest code is executed, and this is not a verified Xbox ABI or COM method signature.

## Verified local findings

After the verified `XGCreateTextureComputer` call at `0x140344235`, the title uses an indirect dispatch call at `0x140344283` (table offset `+0x50`). Both Microsoft Dumpbin disassembly and the new independent C++ pattern scanner corroborate these three nearby stack stores:

| Store VA | Stack destination | Source GPR | Width |
| --- | --- | --- | --- |
| `0x14034427E` | `[RSP+0x20]` | RAX | 8 bytes |
| `0x14034427A` | `[RSP+0x28]` | ESI | 4 bytes |
| `0x140344272` | `[RSP+0x30]` | R8 | 8 bytes |

Their gaps to the call are 0, 5 and 12 bytes, respectively. The surrounding disassembly also shows setup of RCX, RDX, R8 and R9 and a register transfer to R8 before the call. Such shapes are compatible with a multi-argument x64 call, but **argument meaning, count, lifetime, return values and the Xbox XG object's ABI remain unknown**. The analyzer has no proof of the actual live register contents.

## Reproduce

```powershell
.\build\x64\Release\ScarlettVI.exe xg-vcall-stack-fixture
.\build\x64\Release\ScarlettVI.exe xg-vcalls '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

`scan_xg_postcall_stack_writes` works only on the existing bounded executable-section windows and validated indirect-call candidates. It scans at most 32 preceding bytes for MOV from 32-bit or 64-bit GPR to non-indexed `[RSP+offset]`, with aligned positive offsets `0x20`–`0x80`. It supports disp8/disp32 and REX-extended registers. It keeps the closest matching write per slot and stops reuse of evidence across an earlier indirect call. It skips a full matched instruction to avoid falsely reinterpreting the opcode after a REX prefix as a separate 32-bit store.

Synthetic fixtures cover mixed widths, REX registers, positive displacement boundaries, nearest-store selection, previous-call barriers, unsupported address forms, malformed metadata, duplicate contexts, oversized windows, and error atomicity. No proprietary title instruction sequences or assets are checked in.

A separate [chronological register-timeline survey](XG_REGISTER_TIMELINE.md) now observes the R8 stack-address setup **before** its `[RSP+0x30]` store, and distinguishes it from the later R8←R14 register copy before dispatch.

This is **pattern evidence only**, not control-flow/dataflow reconstruction or Xbox import implementation. No other post-create indirect-call site had a qualifying stack write in this bounded scan. Absence of a match does not prove absence of stack arguments. **Title execution remains BLOCKED.**

See [XG_DISPATCH_ARGUMENTS.md](XG_DISPATCH_ARGUMENTS.md), [XG_VCALL_SURVEY.md](XG_VCALL_SURVEY.md) and [STARTUP_READINESS.md](STARTUP_READINESS.md).
