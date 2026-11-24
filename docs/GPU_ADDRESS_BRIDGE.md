# GPU virtual-address translation and CPU/GPU memory integration

**Status:** a verified, original Windows D3D12 buffer-address translation
and GPU execution path. It is **not** an implementation of the Xbox GPU
page table, console D3D12_XS COM interfaces, texture tiling or RDNA2 shaders.

## Why the guest/host address boundary is necessary

Read-only analysis of the supplied game executable at the
`D3D12ConfigureVirtualMemoryX` call (VA `0x141E1CFE0`) shows the game passes
a pointer to a local configuration area in RCX and tests EAX as HRESULT.
Its caller initializes a DWORD at offset 0x04 of the local configuration,
then makes subsequent virtual-memory reservations. The game also calls
`D3D12XboxCreateDevice` at VA `0x1424785B7`, passing:

| Register | Independently observed value |
| --- | --- |
| RCX | Null |
| RDX | Pointer to local configuration object, with fields initialized at offsets 0x00, 0x08, 0x20, 0x24, 0x28, 0x3C |
| R8 | Pointer to global data (likely an interface identifier; not yet independently decoded) |
| R9 | Pointer to a writable output device interface slot |

The game checks the device-create result as an HRESULT. These observations
are insufficient to assert the full configuration structure or the returned
Xbox device object's vtable. **ScarlettVI does not patch either import** and
does not cast Xbox objects to Windows D3D12 COM interfaces.

## Implemented host-side translation table

`src/gpu/guest_address_space.{h,cpp}` implements an independent
`GuestAddressSpace` for *linear buffers only*. The data structure maps a
64 KiB-aligned synthetic guest GPU address range to an owning
`ID3D12Resource` handle and the host buffer's real
`GetGPUVirtualAddress()`. The registry:

- Refuses zero-size, unaligned, overlapping and overflowing guest ranges.
- Refuses resources that are not D3D12 buffer resources, do not provide a
  GPU address, or are smaller than the requested guest range.
- Resolves valid interior byte offsets only when the entire requested
  subrange fits inside a registered allocation.
- Owns a COM reference to each host resource; resolution returns its own
  reference so the resource remains alive for the duration of GPU work.
- Allows deliberate release/unregister of a guest buffer translation.
- Uses locking to serialize concurrent registry changes and queries.

It is **not** yet used by any console device object; guest addresses are
synthetic test identifiers, not actual title GPU virtual addresses.

## Real GPU execution across the memory boundary

`DeviceContext::execute_guest_buffer` connects the XMem physical-page
manager with the existing, independently authored DXIL Shader Model 6 compute
pipeline:

1. XMem allocates two synthetic physical pages, each backed by a Windows
   pagefile section, and maps them at a contiguous 128 KiB CPU address range.
2. A second 64 KiB CPU virtual reservation aliases the output physical page.
3. The test generates 1,024 32-bit input integers in the first guest page.
4. Host D3D12 creates a 64 KiB upload buffer and a 64 KiB unordered-access
   default-heap output buffer, registers them against synthetic guest GPU
   addresses, and resolves their real host GPU addresses with nonzero offsets.
5. It copies guest CPU data into the host upload buffer, dispatches the
   existing original DXIL compute shader, executes a UAV-to-copy state
   transition and copies the GPU results into a readback buffer.
6. It synchronizes on a GPU fence, copies the results into the output guest
   CPU page, and validates every integer through the second virtual alias.
7. The procedure repeats with a different seed and input pattern while
   reusing the same D3D12 queue, pipeline and fence.
8. Negative calls for overlapping and misaligned synthetic GPU ranges must
   fail cleanly, with no additional GPU submission or leaked resource entry.
9. It unregisters both GPU mappings, unmaps the two XMem virtual regions and
   frees the underlying physical pages.

The generated test does not read, extract or use any GTA VI asset or
shader bytecode.

### Verified x64 Release result (NVIDIA RTX 5060 Ti)

```text
ScarlettVI | guest 64KiB pages -> host DXIL GPU compute -> guest aliases: PASS
Host GPU: NVIDIA GeForce RTX 5060 Ti
GPU dispatches: 2 | fence completed: 2 | verified guest output words: 2048
GPU VA mappings left: 0 | physical pages left: 0 | HRESULT: 0x0
```

Run:

```powershell
.\build\x64\Release\ScarlettVI.exe gpu-memory-bridge
```

## Remaining console requirements

- Derive the *complete* `D3D12ConfigureVirtualMemoryX` and
  `D3D12XboxCreateDevice` ABI and correct Xbox COM interfaces before
  attempting to patch their imports.
- Map real title GPU virtual address ranges and resource metadata to host
  buffers and texture resources, including lifetime and synchronization.
- Translate Xbox XG texture tiling and surface layouts, descriptors and
  platform resource flags; this linear-buffer registry rejects textures.
- Translate console RDNA2 shader ISA and metadata to valid host DXIL.
- Implement guest/host coherency, async queues, transfer fences, resource
  residency, and console page protection semantics.

This module establishes an actually executed **host-side XMem-to-GPU bridge**,
but not Xbox graphical compatibility or a bootable game.
