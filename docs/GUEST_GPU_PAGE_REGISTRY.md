# Pinned XMem frames and GPU tile registry

**Status:** original host-side registry and real Windows D3D12 transfer path verified on the RTX 5060 Ti. **Not Xbox unified GPU/CPU physical memory, not an Xbox GPU page-table ABI, and not game execution.**

## Resource and ownership model

The original module `src/gpu/guest_gpu_page_registry.{h,cpp}` correlates four kinds of independently implemented ScarlettVI resources:

- **Guest frame:** synthetic page ID in 4 KiB units issued by the host `PhysicalMemory` backend; not an actual host or Xbox hardware physical address.
- **Two CPU aliases:** live 64 KiB pagefile-backed CPU views that must *both* be managed mappings of the exact synthetic frame.
- **Two GPU slots:** unique records of (logical GPU window, 64 KiB-aligned guest offset) that translate into distinct 4 GiB native D3D12 reserved-buffer segments.
- **GPU page stager:** one live `GpuPageStager` per bound frame. Each stager retains a COM reference to **one registry-owned 4 MiB D3D12 DEFAULT heap**, a unique 64 KiB tile offset inside that heap, two GPU tile aliases, and its own command allocator/list, upload/readback buffers and local fence. All stagers share **one registry-owned native D3D12 DIRECT queue** validated against the same device.

This is a practical multi-page host prototype, **not a scalable full game-VRAM backend**. One real 4 MiB host heap provides **64 independently assignable 64 KiB tile slots**. The registry reserves a distinct slot per bound frame, checks capacity before CPU pinning, and releases it only after fenced GPU NULL mappings. Freed slots can be safely reused without disturbing active pages. Page resources still have individual upload/readback buffers and command allocators; growth beyond 64 resident pages, batched tile-residency updates, eviction and console GPU page-table semantics remain future work.

## Pinned CPU mapping safety

`PhysicalMemory::pin_mapped_alias(address, frame)` checks under the existing physical-memory mutex that a 64 KiB-aligned CPU address maps to a live managed page with exactly that synthetic frame ID. The registry pins both CPU views before GPU registration and removes those pins only after the associated GPU tile mappings have been fenced and unmapped.

While pinned, the original XMem unmap and `VirtualFreeBridge(MEM_RELEASE)` paths reject releasing the CPU slot/region with `ERROR_BUSY`. Scatter unmap and contiguous unmap paths prevalidate **every slot** for pins before changing any mapping. Thus a late pinned slot never leaves an earlier unpinned slot half-unmapped.

Pinning protects the mapping's lifetime; it does **not** prevent CPU reads/writes from another thread. Callers still need their own synchronization for concurrent access to page contents.

## Reusable API

- `GuestGpuPageRegistry::bind(frame, cpu_view_1, cpu_view_2, gpu_slot_1, gpu_slot_2)` validates CPU ownership and GPU offsets, rejects duplicate frame and GPU slot assignments, pins both CPU views, opens a new real GPU stager, and publishes the binding only when the GPU mappings are ready. Failure rolls back pins and resources.
- `transfer(frame, input, scratch_output, writeback_cpu)` requires exact, nonoverlapping 65,536-byte input/output buffers; output must not overlap either pinned CPU alias. The GPU readback is returned only after its real D3D12 fence completes. When explicitly requested, it is then copied to the second pinned CPU alias.
- `transfer_batch(requests)` checks **every** requested frame, buffer extent, duplicate ID, output-to-output overlap, input/output overlap and pinned CPU alias overlap **before any GPU work**. It records one closed D3D12 command list per page, executes the complete array in **one `ExecuteCommandLists` call**, signals **one batch fence**, collects all GPU results into temporary scratch storage, and only then publishes the outputs and optional CPU alias writebacks. Invalid batch requests have no side effects; device loss and GPU execution errors can still abort a batch without writeback.
- `queue_submission_count()` counts native `ExecuteCommandLists` calls separately from per-page transfer results; `native_queue_count()` confirms one queue. `physical_heap_count()` reports the single live D3D12 backing heap, `used_heap_tile_count()` counts reserved 64 KiB heap slots, and `heap_tile_for(frame)` exposes the slot assignment for testing.
- `unbind(frame)` fences and removes both GPU aliases, frees the frame's 64 KiB slot for reuse **only on confirmed GPU retirement**, and then drops both CPU pins. `clear()` retains any bindings whose GPU fence failed, while retiring safe independent bindings; a later retry can finish. The shared 4 MiB heap and DIRECT queue remain live until all bindings are safely retired. See [GPU fence recovery](GPU_FENCE_RECOVERY.md).
- The registry must be destroyed before its owning `PhysicalMemory` and `SparseGpuWindows` objects.

The GPU tile heap is physically separate from the host pagefile-backed CPU XMem section. There is **no automatic unified-memory coherency**. Only an explicit completed staging writeback changes the CPU aliases.

## End-to-end hardware fixture

Run these independent test modes:

```powershell
.\build\x64\Release\ScarlettVI.exe gpu-page-registry
.\build\x64\Debug\ScarlettVI.exe gpu-page-registry
```

The fixture allocates **four real XMem physical pages (64 KiB each)**, creates **eight CPU alias views**, reserves two logical 16 GiB guest GPU address windows, and registers **eight real D3D12 GPU tile mappings** (two aliases per page) near logical offsets 4 GiB and 12 GiB. All eight GPU mappings use **four distinct 64 KiB tiles inside the same single 4 MiB host D3D12 heap**, with slots 0–3 independently assigned and verified.

It tests wrong CPU-frame association, duplicate frame and GPU slots, invalid requests and blocked CPU unmap/release. Each frame first transfers one generated 65,536-byte payload individually: **four** GPU queue submissions. Then all four pages transfer different payloads in **one native queue submission and one fence** with explicit CPU alias writeback. A *second* four-page batch submits four more command lists in **one queue submission**, verifies repeated alias transitions and readback, and **does not write back** to CPU: both CPU aliases must retain the earlier bytes. Invalid/duplicate/overlapping batch requests are rejected before any GPU work.

After unbinding frame 0 while the other three frames remain pinned, a scatter unmap with **unpinned first slot, pinned second slot** must fail *atomically*. A valid single-page unmap/remap of the unpinned slot succeeds. The native heap slot 0 is confirmed free while 1–3 remain assigned; rebinding frame 0 gets slot 0 back. A **third four-page batch** then verifies 65,536 generated bytes per page through the reused slot and other active tiles, with CPU writeback disabled.

The expanded [GPU fence-failure test](GPU_FENCE_RECOVERY.md) then injects synthetic timeouts after a real individual GPU submission, after an actual four-page batch submission, and at the second cleanup fence after queued NULL mappings. It verifies failed transfers never publish CPU output, **CPU aliases and GPU heap slots remain pinned until a real fence can retire them**, and a retry releases the remaining resources. Final cleanup validates **zero pending GPU tile mappings, zero CPU pins and zero allocated physical frames**.

**Measured on RTX 5060 Ti in x64 Release:** PASS, **4 frames, 8 CPU aliases, 8 real GPU tile mappings backed by 4 distinct 64 KiB tiles in one 4 MiB D3D12 heap**, 17 successfully completed 64 KiB GPU transfers in 10 `ExecuteCommandLists` calls (two deliberately interrupted after submission) on 1 shared queue, verified heap-slot reuse, fault-retirement retries, zero leaked CPU pages/pins, zero remaining GPU mappings and heap allocations after clear, HRESULT 0x0.

## Remaining blockers

This is an internal Windows host resource registry, not an Xbox page-table bridge. ScarlettVI still needs real Xbox D3D12_XS/XG GPU memory semantics, correct CPU/GPU coherency, texture swizzle/console shader translation, audio/storage services and safe process startup. The supplied game was never executed or modified. Its read-only readiness report continues to show **25 unresolved Xbox imports** and a **BLOCKED** title-execution gate.
