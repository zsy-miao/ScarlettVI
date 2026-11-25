# XG dispatch successor and object-storage patterns

**Scope:** original C++20 data-only PE analyzer. This report investigates local call order, not a proven Xbox object lifecycle, COM interface, virtual-method identity, or return-value contract. No game instructions are run or copied into the repository.

## Findings corroborated independently

Microsoft's read-only Dumpbin disassembler and ScarlettVI's bounded source-byte scanner agree on **two** post-`XGCreateTextureComputer` paths in which an indirect call at table displacement `+0x08` follows another dispatch with *only* a pointer reload and a table-pointer load between them.

| Initial dispatch VA | Initial table displacement | Immediate successor VA | Reload source | Prior matching storage-access instruction |
| --- | --- | --- | --- | --- |
| `0x140344283` | `+0x50` | `0x14034428D` at `+0x08` | `[R15]` at `0x140344287` | `0x14034423A`, also `[R15]` |
| `0x145121A84` | `+0x10` | `0x145121A8F` at `+0x08` | `[RSP+0x20]` at `0x145121A87` | `0x145121A70`, also `[RSP+0x20]` |

In each case, the *entire* gap between calls consists of the observed shapes `MOV RCX,[base+disp]` followed by `MOV RAX,[RCX]`; the following dispatch is via `[RAX+0x08]`. The earlier load and later reload refer to the same **address expression**. This is **not** proof that the same object address remains stored there before and after the first call. The scan cannot prove control-flow reachability, concurrency, memory mutations, or the actual callee identity.

The other `+0x10` site at `0x140979C9F` has **no matching immediate `+0x08` successor** within this deliberately narrow pattern. A separate [post-call indexed stack-read and field-copy survey](XG_POSTCALL_INDEXED_READS.md) identifies three subsequent stack accesses followed by RSI-field writes at this site. This does not prove it lacks cleanup or a method call elsewhere.

The read-only disassembly also shows that the **immediate successors don't test the previous call's RAX return value**: the `+0x50` and the `+0x10` paths above reload the pointer into RCX and then overwrite RAX with the dispatch-table pointer. This is a *local instruction-sequence observation*, not a proof that a call returns no value or that its return is never used elsewhere. The `+0x08` call's own return meaning is unverified. In particular, **do not assume that `+0x08` is `Release`, even if that offset resembles an ordinary COM table**.

## Implementation and reproduction

```powershell
.\build\x64\Release\ScarlettVI.exe xg-vcall-followups-fixture
.\build\x64\Release\ScarlettVI.exe xg-vcalls '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

`src/analysis/xg_vcall_successors.{h,cpp}` accepts only already-validated file-backed `XGCreateTextureComputer` post-call contexts and independently corroborated `FF /2` indirect-call candidates. It checks for a `+0x08` call at most **16 bytes** after a preceding `+0x10` or `+0x50` call. It must decode the **entire inter-call gap** as the two narrow load forms above, and searches back **at most 96 bytes** for the most recent recognized `MOV RCX,[base+disp]` source pattern. Only nonindexed, nonnegative GPR-based addressing with bounded displacement (0–4096) is considered.

The all-synthetic fixture covers `+0x50` and `+0x10` sequences with differing test offsets from the title, matching and mismatched storage locations, unexpected intervening instructions, nonindexed/indexed and negative displacements, too-distant calls, malformed/foreign candidate metadata, duplicate contexts, oversized windows and truncated executable data. Every invalid-input failure preserves the prior output.

The scanner is **not** a complete disassembler or register/memory SSA engine. The early/late loads are matched as instruction-pattern candidates; no live registers, object pointer values, method return values, side effects or ownership transitions are inferred. We cannot safely implement the Xbox XG returned-object interface until its actual ABI and lifecycle are independently established.

See [XG register timeline](XG_REGISTER_TIMELINE.md), [XG dispatch arguments](XG_DISPATCH_ARGUMENTS.md), [XG stack slots](XG_DISPATCH_STACK_SLOTS.md) and [startup readiness](STARTUP_READINESS.md). The 25 pending Xbox HLE imports and `TITLE EXECUTION: BLOCKED` gate remain unchanged.
