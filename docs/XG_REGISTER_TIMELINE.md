# XG +0x50 local register timeline

**Status:** original C++20, file-backed and read-only *byte-pattern* research. This is a partial local register setup survey, **not** a full x64 decoder, control-flow or dataflow proof, executable guest implementation, or verified Xbox graphics ABI.

## Repeatable commands

Run from the `ScarlettVI` project directory:

```powershell
.\build\x64\Release\ScarlettVI.exe xg-vcall-regs-fixture
.\build\x64\Release\ScarlettVI.exe xg-vcalls '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

The scanner operates on already verified post-`XGCreateTextureComputer` call contexts, and inspects **at most 96 file-backed executable bytes preceding each independently verified `+0x50` indirect-call candidate**. It recognizes only REX.W `LEA destination,[RSP+nonnegative offset]` and `MOV destination,source` register-register patterns. It considers no offset larger than 4096, refuses truncated/invalid inputs atomically, deduplicates duplicate contexts, and stops at a preceding verified indirect call.

A separate pure lookup follows recognized register copies back to the closest earlier recognized stack-address setup, respecting the instruction address of each **use**, not just the eventual call. It rejects unknown and cyclic chains. This is intentionally narrower than native x64 execution or a reaching-definition analysis: **unrecognized instructions could overwrite these registers**, so reported associations remain local candidate chains rather than proven live values.

## Independently corroborated local patterns

MSVC Dumpbin read-only disassembly and ScarlettVI's own pattern scanner agree on the six instruction *locations* below, between the direct XG creation call at `0x140344235` and subsequent indirect call at `0x140344283`:

| Setup instruction VA | Local recognized register pattern |
| --- | --- |
| `0x14034423D` | R14 receives stack address `RSP+0x58` |
| `0x140344242` | R9 receives stack address `RSP+0x48` |
| `0x140344247` | RDX receives stack address `RSP+0x38` |
| `0x14034424C` | RAX receives stack address `RSP+0x3C` |
| `0x140344251` | R8 receives stack address `RSP+0xA0` |
| `0x140344277` | R8 receives the contents of R14 |

At the **indirect call** at `0x140344283`, the recognized local register chains are RDX to `RSP+0x38`, R9 to `RSP+0x48`, and R8 to R14 to `RSP+0x58`.

**Chronology matters:** the earlier outgoing stack store at `0x140344272` copies R8 into `[RSP+0x30]` *before* the R8←R14 move. At that earlier store, the recognized local R8 chain instead points to `RSP+0xA0`. The outgoing store at `0x14034427E` copies RAX into `[RSP+0x20]`, and the recognized local RAX chain points to `RSP+0x3C`. The intervening dword store at `0x14034427A` comes from ESI; this scanner does **not** infer an ESI origin.

These distinct observations explain why one guessed, single "R8 value" across the whole block would be incorrect. They **do not** establish an Xbox method's actual types, parameter count, output values, calling convention details, ownership or semantics.

## Independent synthetic verification

The `xg-vcall-regs-fixture` test uses only separately generated instructions and **different test offsets** from the user-owned title. It verifies both REX.W register move encodings, 8- and 32-bit stack displacements, extended GPRs, temporal R8 changes, chain resolution at both an outgoing store and an indirect call, unsupported and truncated forms, invalid metadata, duplicate contexts, 96-byte scan boundaries, preceding-call barriers, cycle rejection and output-preserving failure.

The original game executable is never executed or written; no SDK or title bytes are added to the repository. See [dispatch-successor evidence](XG_DISPATCH_SUCCESSORS.md), [outgoing stack-write analysis](XG_DISPATCH_STACK_SLOTS.md), [argument setup evidence](XG_DISPATCH_ARGUMENTS.md), [indirect call survey](XG_VCALL_SURVEY.md), and [startup readiness](STARTUP_READINESS.md). **Title execution remains BLOCKED.**
