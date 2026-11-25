# XG post-create dispatch argument preparation survey

**Scope:** read-only, bounded x64 instruction-*pattern* analysis of the user-owned
title's executable PE file. This is **not** a declaration of Xbox XG method
signatures, an executable implementation, a complete instruction decoder, or
proof that these register values survive arbitrary intervening instructions.

## Repeatable commands

From the ScarlettVI directory:

```powershell
.\build\x64\Release\ScarlettVI.exe xg-vcall-args-fixture
.\build\x64\Release\ScarlettVI.exe xg-vcalls '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

The existing `xg-vcalls` command first verifies file-backed XG import call
sites and finds indirect-call candidates in a 128-byte post-create window.
Then `scan_xg_dispatch_argument_setups` looks back **at most 24 bytes** from
each candidate and reports two intentionally narrow byte signatures:

- REX.W `MOV RCX, qword ptr [base + nonnegative displacement]`
- REX.W `LEA RDX, [RSP + nonnegative displacement]`

The scanner supports base-register addressing, non-indexed SIB forms,
positive disp8 and bounded disp32. It rejects RIP-relative, scaled/indexed
and negative-displacement forms, as well as incomplete or inconsistent input.
Candidate calls must match the independently generated `FF /2` scan results.
Only the nearest matching signature per argument-register form is reported.
If a prior indirect call is visible in the same window, scanning begins
**after** that call, so stale argument preparation is not carried across
an obvious volatile-register clobber point.

This remains pattern matching: **not** a reaching-definition analysis, ABI
discovery, or control-flow proof. In particular, unknown intermediate calls
and writes could change the actual live register values.

## Independently corroborated locations

The real-title read-only Release survey now yields **six** argument setup
signatures associated with **four** of the five indirect-call candidates
found after `XGCreateTextureComputer`. These instruction locations were also
checked with Microsoft's Dumpbin disassembler, without executing the title:

| Indirect-call VA | Dispatch offset | Argument pattern and source | Setup instruction VA | Gap after setup |
| --- | --- | --- | --- | ---: |
| `0x14034428D` | `+0x08` | RCX loaded from `[R15]` | `0x140344287` | 3 bytes |
| `0x140979C9F` | `+0x10` | RCX loaded from `[RSI+0x08]` | `0x140979C8B` | 16 bytes |
| `0x140979C9F` | `+0x10` | RDX computed as `[RSP+0x58]` address | `0x140979C97` | 3 bytes |
| `0x145121A84` | `+0x10` | RCX loaded from `[RSP+0x20]` | `0x145121A70` | 15 bytes |
| `0x145121A84` | `+0x10` | RDX computed as `[RSP+0x60]` address | `0x145121A7C` | 3 bytes |
| `0x145121A8F` | `+0x08` | RCX reloaded from `[RSP+0x20]` | `0x145121A87` | 3 bytes |

The earlier `+0x50` call at `0x140344283` uses a more elaborate argument
setup with extra registers and stack parameters. Its setup is **not**
covered by these two signatures within the 24-byte scan. Absence of
a signature is not absence of arguments or of a method call.

Both `+0x10` call sites prepare an RDX address into local stack storage
just before calling; this is consistent with an output-storage argument,
but it **does not prove** the method contract or returned content. Both
`+0x08` calls show a fresh RCX pointer load. Their occurrence after another
call might be consistent with a cleanup convention, but **is not proof**
of a Release-like method, COM object or ownership model.

## Validation and safety boundary

`xg-vcall-args-fixture` uses only original, generated x64 patterns. It checks
a stack-address setup, memory-based RCX setup, positive 8/32-bit
displacements, REX-extended base registers, non-indexed SIB addressing,
near/far windows, malformed/indexed/negative source patterns, stale
signatures across preceding calls, inconsistent candidate metadata,
deduplication, and atomic rejection of invalid contexts.

No instruction bytes, game executable, textures, assets, or proprietary
console SDK files are committed to this repository. No original title code
is invoked. All Xbox graphics imports remain unresolved; title execution
must stay **BLOCKED**.

See [XG_VCALL_SURVEY.md](XG_VCALL_SURVEY.md),
[XG_DESCRIPTOR_EVIDENCE.md](XG_DESCRIPTOR_EVIDENCE.md) and
[STARTUP_READINESS.md](STARTUP_READINESS.md).
