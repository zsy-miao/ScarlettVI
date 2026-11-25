# Pinned XMem frames and GPU tile registry

**Status:** original host-side registry and real Windows D3D12 transfer path verified on the RTX 5060 Ti. **Not Xbox unified GPU/CPU physical memory, not an Xbox GPU page-table ABI, and not game execution.**

## Resource and ownership model

The original module `src/gpu/guest_gpu_page_registry.{h,cpp}` correlates four kinds of independently implemented ScarlettVI resources:

- **Guest frame:** synthetic page ID in 4 KiB units issued by the host `PhysicalMemory` backend; not an actual host or Xbox hardware physical address.
- **Two CPU aliases:** live 64 KiB pagefile-backed CPU views that must *both* be managed mappings of the exact synthetic frame.
- **Two GPU slots:** unique records of (logical GPU window, 64 KiB-aligned guest offset) that translate into distinct 4 GiB native D3D12 reserved-buffer segments.
- **GPU page stager:** one live independently owned `GpuPageStager` per bound frame. It owns a native DIRECT queue, real 64 KiB DEFAULT heap tile, the tile aliases, upload/readback buffers and GPU fences. Its [API](GPU_PAGE_STAGER.md) was verified in the previous milestone.

This is a practical multi-page host prototype, **not a scalable full game-VRAM backend**: each bound frame currently has a separate GPU queue/stager. A production runtime needs shared queues, batched tile mappings, residency policies and eviction.

## Pinned CPU mapping safety

`PhysicalMemory::pin_mapped_alias(address, frame)` checks under the existing physical-memory mutex that a 64 KiB-aligned CPU address maps to a live managed page with exactly that synthetic frame ID. The registry pins both CPU views before GPU registration and removes those pins only after the associated GPU tile mappings have been fenced and unmapped.

While pinned, the original XMem unmap and `VirtualFreeBridge(MEM_RELEASE)` paths reject releasing the CPU slot/region with `ERROR_BUSY`. Scatter unmap and contiguous unmap paths prevalidate **every slot** for pins before changing any mapping. Thus a late pinned slot never leaves an earlier unpinned slot half-unmapped.

Pinning protects the mapping's lifetime; it does **not** prevent CPU reads/writes from another thread. Callers still need their own synchronization for concurrent access to page contents.

## Reusable API

- `GuestGpuPageRegistry::bind(frame, cpu_view_1, cpu_view_2, gpu_slot_1, gpu_slot_2)` validates CPU ownership and GPU offsets, rejects duplicate frame and GPU slot assignments, pins both CPU views, opens a new real GPU stager, and publishes the binding only when the GPU mappings are ready. Failure rolls back pins and resources.
- `transfer(frame, input, scratch_output, writeback_cpu)` requires exact, nonoverlapping 65,536-byte input/output buffers; output must not overlap either pinned CPU alias. The GPU readback is returned only after its real D3D12 fence completes. When explicitly requested, it is then copied to the second pinned CPU alias.
- `unbind(frame)` closes the GPU stager and drops both CPU pins; `clear()` closes all registrations.
- The registry must be destroyed before its owning `PhysicalMemory` and `SparseGpuWindows` objects.

The GPU tile heap is physically separate from the host pagefile-backed CPU XMem section. There is **no automatic unified-memory coherency**. Only an explicit completed staging writeback changes the CPU aliases.

## End-to-end hardware fixture

Run these independent test modes:

```powershell
.\build\x64\Release\ScarlettVI.exe gpu-page-registry
.\build\x64\Debug\ScarlettVI.exe gpu-page-registry
```

The fixture allocates **four real XMem physical pages (64 KiB each)**, creates **eight CPU alias views**, reserves two logical 16 GiB guest GPU address windows, and registers **eight real D3D12 GPU tile mappings** (two per page) near logical offsets 4 GiB and 12 GiB.

It tests wrong CPU-frame association, duplicate frame and GPU slots, null/invalid requests and blocked CPU unmap/release. Every frame transfers two independently generated 65,536-byte payloads through a real GPU queue and back, eight GPU submissions total. The second payload is explicitly written into a CPU alias and verified from its physical sibling.

After unbinding frame 0 while the other three frames remain pinned, a scatter unmap with **unpinned first slot, pinned second slot** must fail *atomically*. A valid single-page unmap/remap of the unpinned slot then succeeds. Rebinding frame 0 works without changing other GPU tile mappings or CPU pages.

Final cleanup validates **zero pending GPU tile mappings, zero CPU pins and zero allocated physical frames**, including failures and deregistration paths.

**Measured on RTX 5060 Ti in x64 Release:** PASS, **4 frames, 8 CPU aliases, 8 real D3D12 GPU tile slots, 8 completed real GPU transfers**, zero leaked CPU pages and pinned mappings, HRESULT 0x0.

## Remaining blockers

This is an internal Windows host resource registry, not an Xbox page-table bridge. ScarlettVI still needs real Xbox D3D12_XS/XG GPU memory semantics, correct CPU/GPU coherency, texture swizzle/console shader translation, audio/storage services and safe process startup. The supplied game was never executed or modified. Its read-only readiness report continues to show **25 unresolved Xbox imports** and a **BLOCKED** title-execution gate.
