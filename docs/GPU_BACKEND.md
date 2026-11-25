# ScarlettVI desktop GPU backend

**Status: native D3D12 initialization, GPU texture copies, DXIL shader compilation, buffer compute dispatch and SRV/UAV texture shader processing work; Xbox GPU command translation is not implemented.**

## Verified desktop result

The C++20 `ScarlettVI.exe gpu` command enumerates actual DXGI adapters and
attempts to construct a functioning D3D12 12_0 command path on each hardware
adapter. On the development machine:

- NVIDIA GeForce RTX 5060 Ti; hardware D3D12 feature level 12_0.
- Successfully created a device, graphics command queue, command allocator,
  command list, GPU fence, and a 64 KiB upload buffer.
- Mapped and wrote original two-byte test data into the upload buffer.
- Serialized a zero-parameter D3D12 root signature and instantiated it.
- The software-only Microsoft Basic Render Driver was excluded from the
  hardware-backend success count.

### Persistent context and GPU-executed transfers

`src/gpu/device_context.cpp` now owns an independently implemented, persistent
D3D12 device, graphics queue, allocator, list, fence and completion event.
The `ScarlettVI.exe gpu-transfer` command uses that same context for **two**
separate command submissions. Each submission:

1. Builds original 128×64 RGBA8 pixel data on the CPU.
2. Determines the correct D3D12 copyable texture footprints and row pitch.
3. Writes into a CPU-mapped upload heap, copies to a GPU default-heap texture,
   transitions its resource state, and copies back to a readback heap.
4. Submits the command list, signals a GPU fence, waits for completion with a
   timeout, and checks every byte of **8,192 returned pixels**.

This command was run on an NVIDIA RTX 5060 Ti with exit status 0: two queued
submissions, completed fence value 2, exact full-texture readback match.

This proves actual host-side **GPU copy command execution and synchronization**.
No Xbox shader programs, GPU tiling instructions or game code are executed, and
none of the generated pixels are taken from the supplied game.
The backend does not yet present actual title frames.

### Shader Model 6 compilation and GPU compute execution

The original C++ `src/gpu/dxil_compiler.cpp` loads Microsoft's
`dxcompiler.dll` from an **installed Windows SDK**, found through Win32 SDK
version discovery. It never loads a compiler from the game directory, and
neither DXC binaries nor precompiled shaders are committed. It uses
`IDxcCompiler3` to compile original embedded HLSL to `cs_6_0` DXIL.

`src/gpu/device_context.cpp` maintains a compute PSO and root signature
independently of the texture-copy path. `gpu-compute` executes:

1. HLSL `StructuredBuffer<uint>` reads and `RWStructuredBuffer<uint>`
   writes using GPU VA root SRV/UAV descriptors.
2. A real compute dispatch of 16 × 64 GPU threads, with two DWORD root
   constants for count and seed.
3. A UAV-to-COPY_SOURCE resource transition, GPU copy into a readback buffer,
   fence synchronization, and exact verification of 1,024 unsigned results.
4. A separate 128×64 texture-copy submission on the same queue.
5. A second compute dispatch using the same compiled DXIL pipeline with
   different constants, for 2,048 total verified results.

**Observed success:** DXIL 3,644 bytes; 2,048/2,048 buffer values verified,
8,192/8,192 texture pixels verified, three submissions and fence value 3.

### Shader-visible descriptor tables and texture compute

The `gpu-texture-shader` command adds a second independently compiled HLSL
pipeline using `Texture2D<float4>` and `RWTexture2D<float4>`.

The test builds a shader-visible CBV/SRV/UAV descriptor heap, binds a texture
SRV at t0 and a separate texture UAV at u0, supplies dimensions as root
constants b0, then dispatches workgroups over an original 128×64 RGBA8
texture. The compute shader writes each destination pixel's complemented
RGB channels, preserving alpha, with strict CPU readback verification.
The input/output textures are **real GPU default-heap allocations**.

**Observed success:** texture DXIL 3,780 bytes, 16,384/16,384 generated
RGBA8 pixels verified over two texture-shader dispatches, one interleaved
buffer compute dispatch verifying 1,024 results; three completed GPU
submissions/fence value 3 on NVIDIA RTX 5060 Ti.

These implementations are original, useful Windows D3D12 building blocks.

### Connecting virtual Xbox physical pages to real host GPU execution

The new original [guest GPU address-space and CPU/GPU memory bridge](GPU_ADDRESS_BRIDGE.md)
adds 64 KiB-aligned synthetic GPU range registration, overlap/bounds validation,
COM resource ownership, subrange-to-host-GPU-VA translation, and integration
with ScarlettVI's existing XMem 64 KiB physical-memory aliases.

`ScarlettVI.exe gpu-memory-bridge` verifies GPU execution using guest-mapped
physical input pages, followed by readback to guest-mapped output pages and
read-through of a second aliased virtual mapping. Two GPU dispatches produced
**2,048/2,048** correct 32-bit results, with all host GPU address mappings
and guest physical pages cleaned up. This is the first working integration
between ScarlettVI's console memory and real Windows D3D12 compute resources.

### Sparse native GPU tile residency with large guest address windows

The original [D3D12 GPU tile mapper](GPU_TILES.md) divides each logical
16/24 GiB GPU VA window into **4 GiB native reserved buffer segments**.
This corrects a measured RTX 5060 Ti mapping boundary: a tile at offset
4 GiB inside one native buffer returned zeros, while the tile at 4 GiB minus
64 KiB returned correct data. Segmentation keeps that address range available
by placing each region in its own D3D12 resource, and preserving continuous
synthetic guest addresses in the translation table.

`gpu-tile-map` now calls actual `UpdateTileMappings` with a real native
64 KiB heap tile mapped to two reserved buffer resources, at logical guest
offsets **4 GiB and 12 GiB**. After a D3D12 aliasing barrier, the GPU
returned **65,536/65,536** exact generated bytes via the second resource,
with queue fences and explicit tile unmapping. This is verified **host tile
residency**, not console memory mapping or a running game shader.

### XMem physical-page to real GPU tile staging

The original [XMem and sparse GPU tile integration](XMEM_GPU_STAGING.md)
bridges the existing pagefile-section-backed CPU physical aliases and
the native D3D12 64 KiB heap tile backend by **explicit staging**.
The refactored [GpuPageStager host API](GPU_PAGE_STAGER.md) owns its own
D3D12 queue, real heap tile, upload/readback resources, alias barriers and
fences. gpu-xmem-tiles now verifies **three independently generated 65,536
byte payloads over two GPU sessions**, using two distinct native segment-local
offsets on either side of a 4 GiB boundary. It validates rejected open,
occupied mapping and short/overlapping buffer requests, then explicitly
copies GPU readback into CPU XMem aliases. Three real GPU submissions,
all bytes verified, zero leaked physical pages. CPU and GPU physical
backing remain separate; this does not emulate XMemTransferMemory.

Read-only XMem call-site research also shows a no-argument
XMemIsToolsMemAvailable check, two working-set selectors and
XMemTransferMemory called with a memory size/selector, rather than
a byte-buffer copy API. All three Xbox imports remain unresolved;
their console-specific memory quota and output structures are unknown.

### Pinned multi-page guest GPU frame registry

The original [GuestGpuPageRegistry](GUEST_GPU_PAGE_REGISTRY.md) now binds
live synthetic XMem physical-frame IDs to two validated **pinned CPU
alias views** and two real D3D12 GPU tile slots. Four frames and eight
GPU tile mappings are tested simultaneously. Each page has an independent
native staging queue/heap and explicit GPU readback/writeback. Eight real
GPU transfers verify different 64 KiB payloads; after unbinding, GPU
tile mappings, CPU pins and physical frames all return to **zero**.

Pinned aliases cannot be unmapped or their region released through the
XMem bridge until their GPU mapping is closed. Cross-slot unmap requests
prevalidate every pinned address before changing any mapping. This is a
host lifetime and staging capability, not an Xbox page table or unified
CPU/GPU physical memory implementation.

### Original host texture formats and XG call-site research

The original [XG call-site and texture-layout research](XG_TEXTURE_LAYOUT.md)
documents four XG texture-computer calls and two swizzle-mode calls in the
user-owned game, without executing game code. The full Xbox object
and descriptor ABI remains unverified, so neither XG import is bound.

The host TextureLayout module independently supports R8, RGBA8 and
BC1/BC3/BC5 block-aware dimensions, row pitches, array layers and mips.
Its reversible synthetic 8x8 Morton microtiles are NOT Xbox swizzle.
The gpu-texture-layout command verified 130 CPU subresources (211,460 bytes)
and 40 actual Windows GPU texture subresources in five D3D12 queue submissions.

These implementations **do not decode Xbox RDNA2 ISA**, reproduce Xbox tile
swizzles, expose Xbox-only GPU/COM semantics or render the Xbox title.

## Console API boundary

The console title imports `d3d12_xs.dll`. Our independent import router now
bridges its `D3D12SerializeVersionedRootSignature` symbol to the public
Windows `d3d12.dll` function, with compatible exported signature. The target
and bundled DLLs contain **two import references** to this symbol.

Other Xbox GPU functions are handled conservatively, without treating
console-only semantics as interchangeable with Windows:

- `D3D12XboxCreateDevice`: **experimental partial bridge implemented** for
  the exact observed request for Windows `ID3D12Device11`; creates real
  native COM interfaces, but does not translate console creation parameters
  or memory/ring behavior. See [Xbox Device11 contract](XBOX_DEVICE11.md).
- `D3D12ConfigureVirtualMemoryX`: **experimental, limited host bridge** for
  the two observed 32-byte configuration modes. It creates two real sparse
  D3D12 GPU VA reservations per mode and validates native VA capacity;
  true Xbox page-table and tile-mapping rules remain unsupported. See
  [GPU virtual memory research](GPU_VIRTUAL_MEMORY.md).
- `XGCreateTextureComputer`, `XGSuggestSwizzleMode`: Xbox-specific tiled
  texture/resource geometry needs independent format/layout translation.

The Xbox device factory and GPU VA configuration imports are **partly
bound**, while host D3D12 tile backing works as an independent test.
Xbox GPU page-table **API translation**, console resource formats,
texture tiling, CPU/GPU coherency and custom extensions remain unsupported. The
native COM object cannot accept console GPU descriptors as-is.

## Implementation plan

1. **Implemented:** persistent host GPU context, adapter selection and
   lifetime, GPU copy commands, buffer/texture compute PSOs, original HLSL to
   DXIL compilation, shader-visible SRV/UAV descriptor tables, texture
   staging, GPU resource transitions, readback and fences.
2. **Partly implemented:** verified the exact Xbox title device IID against
   Microsoft's public Windows SDK and implemented a constrained, original
   Device11 factory bridge. Remaining: Xbox configuration structure behavior,
   console-specific interface extensions and correct graphics lifecycle.
3. **Partly implemented:** synthetic guest GPU VA translation, XMem CPU-page
   readback, segmented 16/24 GiB sparse Windows GPU VA reservations,
   and **real 64 KiB host GPU tile mapping with verified cross-resource
   aliasing and fences**. **Still required:** actual Xbox GPU page-table
   semantics and API routing, unified physical backing, GPU page/heap
   residency policy, console descriptors and texture translation.
4. Xbox texture layout, swizzle and compression format conversion.
5. Independently authored RDNA2 console shader translation into DXIL;
   validate against original generated shader fixtures.
6. Render-target synchronization and presentation path.

## Sources

- [Microsoft: Simple Device and SwapChain sample](https://learn.microsoft.com/en-us/samples/microsoft/xbox-gdk-samples/simpledeviceandswapchain/)
- [Microsoft: D3D12CreateDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice)
- [Microsoft: D3D12SerializeVersionedRootSignature](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12serializeversionedrootsignature)
- [Microsoft: IDxcCompiler3::Compile](https://learn.microsoft.com/en-us/windows/win32/api/dxcapi/nf-dxcapi-idxccompiler3-compile)
- [Microsoft: Using descriptors directly in root signatures](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-descriptors-directly-in-the-root-signature)
- [Microsoft: Resource binding overview](https://learn.microsoft.com/en-us/windows/win32/direct3d12/resource-binding-flow-of-control)

No proprietary GPU binaries, SDK headers or shader assets are part of this
repository.
