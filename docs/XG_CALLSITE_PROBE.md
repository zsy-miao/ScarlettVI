# Read-only XG import call-site probe

**Status:** original C++20 PE static analysis, verified against the user-owned title's executable bytes with no game execution. This module does **not** implement XG, Xbox texture swizzling, graphics object vtables, or any unresolved import.

## Purpose

The user-owned Scarlett PE imports two Xbox graphics-layout functions from `xg_xs.dll`: `XGCreateTextureComputer` and `XGSuggestSwizzleMode`. Accurate emulation requires verifying their *real* descriptors and return-object contracts, not inventing a Windows-like ABI just because their names sound familiar.

The new original `src/analysis/xg_callsite_probe.{h,cpp}` analyzes the **original PE file bytes only**. It does not run code, load the title or its DLLs, resolve a console function, call TLS, or interpret game texture assets.

## Evidence chain

1. Parse the PE32+ import directory using ScarlettVI's existing `PeFile` parser. Identify the exact import **IAT RVAs** from module `xg_xs.dll` and the two named functions (case-insensitive match).
2. Scan only the PE sections marked executable, never writable data or arbitrary file-padding sections. Within their file-backed bytes, identify x64 `FF 25 disp32` RIP-relative jump-stub candidates whose calculated address matches an XG import's *IAT location*, without reading the IAT pointer or calling it.
3. Scan executable bytes for `E8 disp32` relative-call candidates that target the discovered XG jump-stub RVAs. Compute targets with signed 64-bit intermediate arithmetic and reject out-of-range values. Sort and deduplicate results.
4. Report image base, IAT RVA, jump-stub VA, and each candidate direct-call VA. The report does not modify the image, bind Xbox imports or change title-execution gates.

**Caution:** matching `FF25` and `E8` byte patterns does **not** by itself prove an instruction begins at that offset. A complete x64 decoder would be required to exclude instruction-data coincidences. The six reported candidate call positions were also corroborated separately by MSVC `dumpbin /disasm`; the scanner is explicitly labeled *byte-pattern evidence*, not a full disassembler.

## Repeatable commands

From the ScarlettVI project directory:

```powershell
.\build\x64\Release\ScarlettVI.exe xg-calls-fixture
.\build\x64\Release\ScarlettVI.exe xg-calls '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
.\build\x64\Release\ScarlettVI.exe xg-context '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

The generated `xg-calls-fixture` test constructs a synthetic 160-byte x64 section containing two RIP-relative import jump stubs and three relative calls, then verifies all discovered RVAs. It also corrupts one stub displacement to ensure unrelated calls are no longer attributed to that import, and rejects a section whose range would wrap 32-bit address space. It now also tests a **bounded call-context extractor** using a separate generated 96-byte fixture: exact pre/post-call clipping, correct relative-call target, truncated 5-byte calls, overlapping executable spans, oversized window requests and all-or-nothing failure behavior. **No proprietary game instructions appear in the source or fixtures.**

`xg-context` reuses the verified XG PE scanner and prints a bounded **96 bytes before** and **48 bytes after** each five-byte E8 call, clipped to the containing file-backed executable section. It verifies each original relative displacement against a discovered XG import trampoline before reporting any window. It performs no instruction decoding or automatic argument inference; the byte windows are intended for independent local disassembly. **Never copy these game-specific byte windows into the public repository.**

## Verified actual title data-only result

The Release scanner read **99,875,174 bytes of executable PE sections** and identified exactly the following, consistent with earlier manually verified disassembly. The new Release context command independently validated all **six** corresponding call-to-trampoline displacements and returned bounded context windows, without executing game code:

| Imported Xbox name | IAT RVA | Import-stub VA | Candidate call-site VAs |
| --- | --- | --- | --- |
| `XGCreateTextureComputer` | `0x6793F60` | `0x145F3FF40` | `0x140344235`, `0x140979C75`, `0x1416F7141`, `0x145121A5E` |
| `XGSuggestSwizzleMode` | `0x6793F68` | `0x145F3FF50` | `0x140C4B3CE`, `0x144ECCCA2` |

Both import jump stubs are `FF25` RIP-relative IAT jumps. For example, the actual disassembly of `0x140344235` shows a five-byte relative call to `0x145F3FF40`; the jump stub loads the IAT slot at `0x146793F60`. The second Xbox import jump stub resolves to IAT VA `0x146793F68`.

These findings do **not** establish the size, alignment, field offsets or supported values of the Xbox texture descriptor, the lifetime or vtable contents of the returned texture computer, or actual Xbox Series X|S tiling/swizzle algorithms.

## Relationship to the host backend

The existing [host TextureLayout](XG_TEXTURE_LAYOUT.md) implements independent R8, RGBA8, BC1, BC3 and BC5 linear block pitches, array/mip layout and reversible *synthetic* 8×8 Morton microtiles. Its 130 CPU and 40 real D3D12 GPU subresource tests are valid **Windows host tests** but not evidence of Xbox-compatible texture addressing.

An independent read-only disassembly of the shared title-side texture descriptor preparation helper has produced [preliminary field-offset evidence](XG_DESCRIPTOR_EVIDENCE.md), including several compact-input to prepared-output transfers and two swizzle-call register patterns. A separate [synthetic field fixture](XG_FIELD_PROJECTION.md) tests five of those transfers plus confirmed category-dependent constants
with generated input data. **These observations do not establish the Xbox ABI**. The next graphics task is to independently validate all control-flow paths, compare against public documentation where appropriate, then build a tested adapter only after the ABI is established. No proprietary Xbox SDK source is required or copied.

The original GTA VI executable remains **not bootable or rendering**. The readiness report retains **25 unresolved Xbox imports**, including these two XG names, with all startup execution gates BLOCKED.
