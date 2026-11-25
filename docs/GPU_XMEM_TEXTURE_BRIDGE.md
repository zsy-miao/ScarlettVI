# XMem-owned GPU texture bridge and expanded host formats

**Status:** original, generated host texture staging now connects
ScarlettVI-managed XMemAlloc data to a real Windows D3D12 DXIL compute
detiler and back into lifetime-pinned XMemAlloc output memory. The
12 supported **DXGI host formats** are not Xbox XG format/swizzle
descriptors; this remains a host compatibility building block only.

## Supported host DXGI formats

One shared table, `kHostTextureFormats` and
`host_texture_format_info()` in `src/gpu/texture_layout.{h,cpp}`,
feeds the CPU layout encoder, native D3D12 texture-copy fixture,
and real GPU detiling fixture.

| Format | Element footprint | Element bytes |
| --- | --- | ---: |
| R8_UNORM | 1 x 1 | 1 |
| R16_FLOAT | 1 x 1 | 2 |
| R32_FLOAT | 1 x 1 | 4 |
| R8G8B8A8_UNORM | 1 x 1 | 4 |
| R16G16_FLOAT | 1 x 1 | 4 |
| R16G16B16A16_FLOAT | 1 x 1 | 8 |
| BC1_UNORM | 4 x 4 | 8 |
| BC3_UNORM | 4 x 4 | 16 |
| BC4_UNORM | 4 x 4 | 8 |
| BC5_UNORM | 4 x 4 | 16 |
| BC6H_UF16 | 4 x 4 | 16 |
| BC7_UNORM | 4 x 4 | 16 |

BC blocks are **opaque bytes**: this layer preserves their bytes during
tiling and GPU copy rather than decompressing or fabricating pixels.
The layout supports mips, arrays, odd dimensions, bounds checks and
non-overlapping source/destination buffers. The synthetic 8 x 8 Morton
microtile is emphatically **not** the Xbox Series X|S swizzle.

## XMem to GPU to XMem

`DeviceContext::detile_host_texture_xmem_heap()` takes an already
validated original host `TextureLayout` and two opaque pointers.
It does not dereference either pointer before ownership checks.

1. Require nonempty input/output sizes at most **1 MiB** each.
2. Pin the destination as a live XMemAlloc allocation subspan through
   the existing reference-counted `HostXMemWritePin`. A concurrent
   `XMemFree` cannot tear it down while work is active.
3. Snapshot every source byte from a live XMemAlloc allocation under
   the heap registry mutex. Unknown, freed or truncated pointers are
   refused without reading the address.
4. Convert the private staged bytes through the actual D3D12 GPU
   pipeline, wait for GPU fence and readback, then write only the exact
   successful output into the pinned XMem allocation.
5. Release the pin on every success or failure branch. The output
   remains unchanged on rejected source/destination arguments and
   GPU errors before the final write.

This is a **synchronous** staging bridge. Xbox D3D12_XS command packets,
shader ISA, RDNA2 tiling metadata, resource descriptor translation and
frame presentation still require independent implementations.

## Measured Release fixtures

```powershell
.\build\x64\Release\ScarlettVI.exe gpu-texture-layout
.\build\x64\Release\ScarlettVI.exe gpu-texture-detile
.\build\x64\Release\ScarlettVI.exe gpu-xmem-texture
```

The tested Windows host GPU is an NVIDIA GeForce RTX 5060 Ti:

- **312** generated CPU texture subresources, **779,692** bytes exactly
  round-tripped through the synthetic CPU swizzle.
- **96** real D3D12 texture subresources, 12 copy submissions and exact
  readback of all valid texel/block bytes.
- **48** actual DXIL GPU detiling dispatch submissions across the 12
  formats and odd-sized mip chains.
- **24** additional XMemAlloc-input -> GPU -> pinned-XMemAlloc-output
  conversion submissions, comparing the complete output byte-for-byte
  with synthetic expected data.
- Negative cases reject arbitrary source/destination pointers and a
  texture larger than the 1 MiB bridge limit without GPU dispatch.
  Successful results preserve both leading/trailing buffer canaries,
  release the bridge's pin, and refuse a premature XMemFree while a
  separate pin remains live.

No real game binaries, textures, shaders or title callbacks are
executed, decoded, modified or redistributed. Import addresses remain
linked but **XGCreateTextureComputer and XGSuggestSwizzleMode remain
error-only**. Native title execution stays **BLOCKED** until real Xbox
GPU resource/command and shader behavior is available.
