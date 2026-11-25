# Preliminary XG texture descriptor field evidence

**Scope:** read-only disassembly of the user-supplied PE. This is **not** an Xbox XG ABI definition, supported swizzle algorithm or working import binding. Do not execute the title or copy its binary instructions/assets into this repository.

## Why this helper matters

Three of the four verified `XGCreateTextureComputer` call sites prepare their RCX descriptor through the same title-local routine, whose entry at VA `0x144ECC7F0` jumps to a body at `0x144ECC940`. The fourth call site copies a prepared descriptor from an existing caller-supplied buffer to stack storage before invoking XG. These paths show how the **title's compact texture input** is converted to a **prepared descriptor** that is later supplied to the unresolved XG import. They do *not* tell us how XG's implementation consumes that descriptor.

The local, Git-ignored scratch disassembly uses Capstone 5.0.6 against the original, unmodified PE file. The existing `xg-context` command independently verifies the six original E8 call-to-import-trampoline displacements and bounds all call windows within file-backed executable sections. The helper's branch analysis is **preliminary**: it must be independently cross-checked against a second disassembler and alternate branches before assigning semantic field names.

## Observed transfers in the primary nonzero category path

These offsets are relative to the helper's compact source pointer (RDX) and prepared destination pointer (RCX). Widths describe instructions **in this path**, not necessarily the final ABI's C types.

| Compact source offset | Observation | Prepared output offset |
| --- | --- | --- |
| `+0x00` | reads 16 bits, zero-extends to 64 bits | `+0x10` |
| `+0x02` | reads 16 bits, zero-extends to 32 bits | `+0x18` |
| `+0x04` | reads 16 bits | `+0x1C` |
| `+0x0A` | reads 8 bits, widens to 16 bits | `+0x1E` |
| `+0x07` | reads 8 bits, widens to 32 bits | `+0x20` |
| `+0x09` | lookup/bit-shift-derived value | `+0x24` |
| `+0x08` | controls a conditional value | `+0x2C` |

The helper also branches on source bytes at `+0x06`, `+0x0B`, `+0x0C`, and `+0x0D`, and uses other register arguments to derive a flag field at output `+0x30`. The routine initializes or clears several output fields, and certain format categories appear to adjust two dimensions to multiples of four. Those rules require full control-flow validation before implementation.

**Important distinction:** an observed store to `+0x30` establishes at least a 52-byte accessed extent in this path, but not an exact descriptor type or total size. Another caller performs overlapping AVX copies that cover at least 56 bytes of prepared data. No field meaning, accepted enum, padding rule or output-object lifetime contract is yet proven.

## Other observed call contracts

- All four `XGCreateTextureComputer` callers set RCX to prepared descriptor storage and RDX to an output-pointer location. Two capture EAX and branch on its sign after the call, consistent with—but not proof of—HRESULT-style failure handling.
- Both `XGSuggestSwizzleMode` callers zero EDX immediately before the import. One also copies RAX into R8 before calling. Each copies EAX into ECX after returning and passes control to a shared helper; this does **not** establish what the return value means.
- Returned texture-computer pointers are used for indirect method calls in some success paths. The dispatch-table layout, methods and memory ownership remain unverified; treating the object as ordinary desktop COM without proof would be unsafe.

## Decision and next checks

Keep both `xg_xs.dll` imports unresolved and `TITLE EXECUTION: BLOCKED`. A new [synthetic field-projection fixture](XG_FIELD_PROJECTION.md) now models only five directly observed source-to-output transfers, with 512 generated tests, conservative alternate-branch rejection, and no game execution. Next, independently verify remaining control-flow paths, identify compact format/category values and size limits, and then expand the fixture **only** for substantiated fields. Do not label the host's synthetic Morton tiling as Xbox XG swizzling.

See [XG_FIELD_PROJECTION.md](XG_FIELD_PROJECTION.md), [XG_CALLSITE_PROBE.md](XG_CALLSITE_PROBE.md), [XG_TEXTURE_LAYOUT.md](XG_TEXTURE_LAYOUT.md), and [STARTUP_READINESS.md](STARTUP_READINESS.md).
