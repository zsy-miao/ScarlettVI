# Synthetic XG compact-field projection research fixture

**Status:** verified original C++20 data-only helper. This is deliberately **not** an implementation of `XGCreateTextureComputer`, `XGSuggestSwizzleMode`, a complete Xbox texture descriptor, or an Xbox tile/swizzle algorithm.

## Motivation

The read-only [descriptor evidence notes](XG_DESCRIPTOR_EVIDENCE.md) identify direct assignments from a compact title-side texture structure into a prepared structure through three category-dependent entry paths and a shared write block. The observed offsets, widths and category branches were independently cross-checked with both Capstone 5.0.6 and Microsoft Dumpbin. Semantic field names, full descriptor size, conditional fields, flag behavior and the actual Xbox XG ABI remain unknown.

Rather than guessing, `src/analysis/xg_field_projection.{h,cpp}` implements a **pure synthetic projection** of only these assignments and two directly observed category-specific constants. Input and output bytes belong to the test harness; no title code is executed or linked, and no game texture content is read.

## Explicitly modeled offsets

All offsets are hexadecimal. The source must contain at least `0x0B` bytes, and the output at least `0x24` bytes.

| Input compact offset | Read width | Prepared offset | Write width | Value |
| --- | ---: | --- | ---: | --- |
| `+0x06` | 8-bit category | `+0x00` | 32-bit LE | Category `0` → `2`, `3` → `4`, otherwise `3` |
| `+0x00` | 16-bit LE | `+0x10` | 64-bit LE | Zero-extended word |
| `+0x02` | 16-bit LE | `+0x18` | 32-bit LE | Category `0` → constant `1`; otherwise zero-extended word |
| `+0x04` | 16-bit LE | `+0x1C` | 16-bit LE | Same word |
| `+0x0A` | 8-bit | `+0x1E` | 16-bit LE | Zero-extended byte |
| `+0x07` | 8-bit | `+0x20` | 32-bit LE | Zero-extended byte |

The output only changes bytes `+0x00` through `+0x03` and `+0x10` through `+0x23`. All unmodeled bytes, including any prior contents of the supplied buffer, are preserved. The helper does not derive output fields at `+0x24`, `+0x2C`, or `+0x30`; their lookup tables, conditions, and flags are not yet established.

Category `0` and `3` take separate, observed paths before rejoining the shared write block. Dumpbin independently confirmed that category `0` supplies constants `1` and `2`, while category `3` supplies a token of `4` and uses the compact 16-bit value at `+0x02`. The ordinary path supplies token `3`. These are opaque *observed tokens*, not established Xbox descriptor enum names.

## Validation

From the ScarlettVI project directory:

```powershell
.\build\x64\Release\ScarlettVI.exe xg-field-fixture
```

The `xg-field-fixture` command runs 512 deterministic synthetic input/output vectors, with independently constructed expected values and sentinel-preservation checks. It covers all three observed category paths (including `0` and `3`), verifies truncated input/output rejection and overlapping-span rejection, and works with an unaligned destination and adjacent nonoverlapping spans. Negative tests check **no destination changes on failure**.

This is a host-only confidence test for the five field *transfers*. It does not verify the actual Xbox XG SDK implementation, data formats or runtime behavior. The underlying XG imports remain unresolved and title execution stays **BLOCKED**.

## Next step

Independently validate the helper's remaining format/flag-dependent writes, layout tables and object-method contract before extending the projection. Only then can a separate Xbox-facing adapter be designed with verified ABI data. Do not use this partial projection to instantiate an XG texture computer.
