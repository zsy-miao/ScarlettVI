# ScarlettVI desktop GPU backend

**Status: native D3D12 initialization and GPU-executed texture upload/readback work; Xbox GPU command translation is not implemented.**

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

## Console API boundary

The console title imports `d3d12_xs.dll`. Our independent import router now
bridges its `D3D12SerializeVersionedRootSignature` symbol to the public
Windows `d3d12.dll` function, with compatible exported signature. The target
and bundled DLLs contain **two import references** to this symbol.

The other Xbox GPU functions are deliberately **not** redirected to unrelated
Windows functions:

- `D3D12XboxCreateDevice`: Xbox-specific parameter structure, device
  capabilities, and object ABI must be decoded and implemented first.
- `D3D12ConfigureVirtualMemoryX`: console GPU virtual memory layout and
  heaps require their own backend.
- `XGCreateTextureComputer`, `XGSuggestSwizzleMode`: Xbox-specific tiled
  texture/resource geometry needs independent format/layout translation.

All these remain unresolved. The native D3D12 device is an important
building block but cannot accept console GPU descriptors as-is.

## Implementation plan

1. **Implemented:** persistent host GPU context, adapter selection and
   lifetime, GPU copy commands, texture staging and readback/fence ownership.
2. Console interface contract mapping (public and reverse-engineered
   data layouts) with explicit unsupported-function detection.
3. GPU virtual address remapping into host resources and descriptor heaps.
4. Xbox texture layout, swizzle and compression format conversion.
5. Independently authored RDNA2 console shader translation into DXIL;
   validate against original generated shader fixtures.
6. Render-target synchronization and presentation path.

## Sources

- [Microsoft: Simple Device and SwapChain sample](https://learn.microsoft.com/en-us/samples/microsoft/xbox-gdk-samples/simpledeviceandswapchain/)
- [Microsoft: D3D12CreateDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice)
- [Microsoft: D3D12SerializeVersionedRootSignature](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12serializeversionedrootsignature)

No proprietary GPU binaries, SDK headers or shader assets are part of this
repository.
