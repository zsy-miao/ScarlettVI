# Xbox XG imports: verified failure-only binding

**Status: two imports linked, zero functional Xbox texture/swizzle
operations. Native title startup remains blocked.**

ScarlettVI now owns the two currently missing `xg_xs.dll` function
entry points used by the analysed title:

| Symbol | Host entrypoint | Behaviour |
| --- | --- | --- |
| `XGCreateTextureComputer` | `xg_create_texture_computer_host` | Always returns `E_NOTIMPL`, never allocates/returns a texture computer |
| `XGSuggestSwizzleMode` | `xg_suggest_swizzle_mode_host` | Always returns `E_NOTIMPL`, never returns a guessed layout |

These are **explicit failure-only** interfaces, not emulation of the
Xbox graphics library. They enable import resolution without returning
fake success or causing a guest to dereference an invalid fabricated
COM-like vtable. Neither function reads or writes its supplied pointers.
A caller might not handle `E_NOTIMPL`; import binding alone does
**not** establish that a title can proceed past a call.

## Public ABI context

[Microsoft DirectXTex discussion #498](https://github.com/microsoft/DirectXTex/issues/498)
demonstrates the three-argument `XGSuggestSwizzleMode` call with a
resource descriptor, a flags value and an output `XG_TEXTURE_LAYOUT`.
The [Microsoft Xbox DirectXTex sample](https://github.com/microsoft/Xbox-ATG-Samples/blob/main/Kits/DirectXTex/DirectXTexXboxTile.cpp)
demonstrates XG computer creation and distinct Xbox tiling/mipmap
calculations. Our [read-only title callsite analysis](XG_TEXTURE_LAYOUT.md)
observed `XGCreateTextureComputer` at four callsites with an input
descriptor pointer in RCX and an output computer pointer in RDX.
The concrete Xbox COM/vtable semantics, layout structures, and swizzle
algorithms are **not** inferred from public docs or replaced with
Windows D3D12 layout values.

## Safety checks

`ScarlettVI.exe xg-import-fixture` verifies both exact symbol-name
lookups, rejection of unknown names, stable `E_NOTIMPL` results for
null or intentionally invalid pointers, and no output writes on failure
(including pointer-to-pointer and byte-array canaries).

The import resolver counts them separately under
`experimental_xg_error_only_bound`. They no longer appear as
unresolved import references, but remain **functional blockers**;
readiness keeps `title_execution_ready=false` and retains the
graphics runtime milestone. Xbox texture layout, swizzle, CPU/GPU
memory coherence, GPU commands and shader translation are not done.

No game assets are modified, decoded, or executed for this fixture.
