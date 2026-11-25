# Pinned multi-page GPU batch registration

**Status:** original host-only C++20 Windows D3D12 implementation, verified with generated data on the RTX 5060 Ti. **Not Xbox GPU page-table emulation, not unified CPU/GPU memory, and not title execution.**

## New registry API

`GuestGpuPageRegistry::bind_batch(std::span<const PageBinding> pages)` binds several existing XMem physical-page IDs to two independent CPU aliases each and two GPU aliases each. Each GPU alias group is a **contiguous range of 64 KiB guest tiles entirely within one native reserved D3D12 resource**. The two groups must refer to different native reserved resources. Xbox-sized guest windows are still split into native ≤4 GiB segments.

The existing single-frame `bind` and [independent GPU page stager](GPU_PAGE_STAGER.md) remain available and fully tested. The batch implementation lives in `src/gpu/guest_gpu_page_registry_batch.cpp`, with a deferred-tile-mapping option for creating fully owned per-frame D3D12 upload, readback, command-list and fence resources before any mapping is submitted.

### Lifecycle

1. **Preflight:** reject empty or larger-than-64 batches; duplicate physical frames; invalid, duplicated or occupied GPU slots; nonadjacent offsets, unaligned CPU aliases, invalid native resource identity and any native resource-segment crossing. Check that each guest frame can receive a distinct free physical GPU heap-tile index. No D3D12 mapping operation occurs in this stage.
2. **Pin all CPU aliases:** validate both actual XMem 64 KiB CPU mappings against their precise synthetic physical page ID. On a pin failure, immediately undo all acquired pins without submitting GPU work. The dedicated negative test checks a wrong alias-to-frame mapping in the middle of the batch and asserts zero remaining pins.
3. **Prepare real GPU resources:** lazily create the shared DIRECT queue, 4 MiB physical tile heap, batch fence/event and N independent GPU page stagers. Defer every stager's individual GPU tile updates. Errors before native enqueue unpin the frames and discard the prepared objects.
4. **Publish ownership:** reserve and insert every frame, CPU pin, two guest GPU slot IDs and the selected physical tile index into the registry **before** the first native D3D12 mapping update. Thus asynchronous failures cannot leave a GPU mapping without a reachable owner.
5. **Map in two calls:** call `SparseGpuWindows::map_tile_selection` once for the adjacent source slots and once for the adjacent second GPU aliases, using the **same distinct physical heap indices** in the two native reserved resources. For four pages, this reduces eight separately issued mapping calls to **two**.
6. **Fence before access:** signal and wait for one real shared GPU queue fence covering both mapping operations. Check the device-removed `UINT64_MAX` sentinel before treating a fence as complete. Only then are the prepared stagers made eligible for GPU copy/readback.
7. **Fail-closed recovery:** if enqueue, fence signal or fence wait fails after ownership publication, the stagers retain GPU resources and the XMem CPU aliases remain pinned. A failed native mapping request retires only pages whose stager cleanup actually confirms its fence; unresolved pages remain registered. A failed completion wait after both mappings leaves the cohort registered but not eligible for GPU transfer. The owner can retry `unbind` or `clear`; these methods fence GPU work and NULL unmaps before releasing page pins and heap slots. No false successful bind is reported.

This is host resource-lifetime management only. Both CPU XMem aliases are backed by pagefile sections, whereas the physical D3D12 heap tiles are separately backed; explicit GPU upload/readback and optional writeback are still necessary.

## Fenced grouped NULL tile unmapping

The companion `GuestGpuPageRegistry::unbind_batch(span<frame IDs>)` now
retires an ordered cohort using at most **one native NULL
`UpdateTileMappings` call for each of its two adjacent GPU alias
groups**. Like registration, each group must remain entirely within one
native D3D12 reserved resource.

- **Preflight** rejects empty, duplicate or out-of-order frame IDs and
  nonadjacent GPU slots without modifying a mapping or CPU pin.
- **Pre-unmap fences** wait on every page's real shared GPU queue work
  before a native tile map can change. A failed pre-fence leaves all
  mappings intact and GPU transfers usable.
- **Disable data transfers** for the cohort once it enters the native
  unmap phase. NULL mapping of the source four-page group enqueues one
  call; NULL mapping of the alias four-page group enqueues another.
  Successful enqueues are recorded per stager so retry never repeats a
  completed group.
- **Post-unmap fence** must confirm the queued work before the registry
  closes native resources, unpins CPU aliases, or reuses heap tile slots.
  A timeout after the first or both native NULL calls leaves all frames
  bound and their CPU aliases pinned. The next `unbind_batch` can
  finish the missing group and fence; alternatively `clear()` can
  safely complete per-frame retirement.
- Host native unmap call counts remain observable via
  `native_unmapping_count()`. No claim is made that a `void`
  `UpdateTileMappings` enqueue is proof of GPU completion.

This is **not** transactional GPU page-table replacement on Xbox, nor
device-loss recovery. The existing `unbind(frame)` and `clear()`
remain supported for arbitrary/noncontiguous resource layouts and
fail-closed fallback cleanup.

## New end-to-end test

```powershell
.\build\x64\Release\ScarlettVI.exe gpu-page-batch
.\build\x64\Debug\ScarlettVI.exe gpu-page-batch
```

The fixture uses **four genuine 64 KiB XMem physical frames**, **eight independently pinned CPU alias views**, two logical **16 GiB guest GPU windows**, one real **4 MiB shared D3D12 heap**, and a real RTX 5060 Ti DIRECT queue.

- Validate empty/duplicate frame requests, identical source/mirror CPU VAs, wrong physical-frame associations, nonadjacent guest slots and a truly contiguous guest region that crosses the 8 GiB native reserved-resource boundary. All reject without mapping calls or leaked CPU pins.
- Register the four frames in **two** native `UpdateTileMappings` calls (source region near 4 GiB + 5 pages; alias region near 12 GiB + 9 pages). Confirm four unique real heap tiles and eight GPU virtual mappings, all backed by one shared 4 MiB heap. Duplicated live registrations reject without mapping.
- Prevent CPU alias unmap and whole-region release while pinned. Upload generated input for four frames, execute four GPU page command lists in **one** `ExecuteCommandLists` call, and verify all **262,144** bytes via the second GPU alias group.
- Run a second four-page transfer batch with **different generated contents** and explicitly write the GPU readback into the second CPU alias. Verify both CPU aliases see those bytes.
- Unbind one frame, prove it drops precisely two CPU pins and two GPU mappings without disturbing the other three; rebind through the original single-page `bind` and verify physical GPU heap-tile 0 is safely reused. Transfer all four frames again through one GPU batch.
- Retire all frames, inject a **synthetic mapping-completion timeout after two real native mapping calls** in a fresh four-frame registration, and verify all eight CPU pins and GPU virtual mappings remain owned. Attempted transfer is rejected while the GPU mapping fence has not been confirmed.
- `clear` then waits for real host GPU completion, NULL-unmaps both aliases per frame, frees the shared heap and CPU pins, and returns to zero live frames. A full four-frame registration and GPU copy after this recovery verifies the same resources are reusable.
- Reject duplicate and out-of-order unbind frame lists. Inject a **synthetic pre-unmap fence timeout**: no NULL mappings may be queued and all CPU pins remain intact. The GPU transfer test then runs successfully.
- Inject a synthetic interruption **after the real source-group NULL update**. Four GPU mappings remain in the alias resource, all eight CPU aliases remain pinned, and further GPU transfers are rejected. Retrying `unbind_batch` queues only the missing alias-group NULL update, waits on a real GPU fence and releases all page pins. Exactly **two native NULL mapping calls** replace eight per-frame calls.
- Bind and GPU-copy the entire cohort again. Inject a synthetic interruption **after both real grouped NULL updates**, before the completion fence: the mapping index reaches zero but pins, heap tiles and bindings remain owned. `clear()` then confirms GPU completion and releases all resources without duplicate NULL updates.

**Observed RTX 5060 Ti Release result:** PASS, **20 verified 65,536-byte GPU page transfers** across **five actual GPU command submissions**. The fixture reports ten native GPU mapping calls across multiple successful and injected-error registrations, and **four grouped NULL mapping calls** across two four-page unbind attempts. Each four-page cohort uses two calls instead of eight independent native NULL calls. At exit: **zero allocated XMem pages, zero CPU pins, zero GPU tile mappings and zero shared GPU heap allocations**.

## Scattered guest GPU page runs

The optional [scattered page planner](GPU_PAGE_SCATTERED.md) now groups
unordered physical-frame/GPU-slot pairs across the native 4 GiB resource
boundaries. It sorts and validates each source/alias pair, holds **outer
CPU XMem pins across all runs**, invokes `bind_batch` separately per
native-resource run, and rolls back previous successful runs with real
fences if a later GPU mapping fails. `unbind_scattered` reuses the
existing grouped NULL-unmap contract for the currently registered runs.
The dedicated RTX 5060 Ti `gpu-page-scattered` fixture verified **eight
real XMem frames, 16 CPU pins, 24 completed 64 KiB GPU page transfers
in three submissions**, third-run fault rollback and no leaks.
Scattered native GPU updates are **not globally atomic**; interrupted
unmapping may retire earlier runs, so callers must retry only the still
registered frames.

## Limits

- Each batch source/alias group must consist of adjacent 64 KiB guest tiles **within one native reserved buffer resource**. Grouping nonadjacent locations or mappings across native segments requires multiple calls and additional transaction management.
- `SparseGpuWindows::map_tile_selection` returns a host HRESULT for validation/enqueue only; native `UpdateTileMappings` returns `void`, so real GPU fences and readback are essential.
- The batch path does not merge all GPU command buffers, heap allocations or window resources into one Xbox GPU paging system; it reuses the existing [GPU page registry](GUEST_GPU_PAGE_REGISTRY.md) and [fence-safe cleanup rules](GPU_FENCE_RECOVERY.md).
- Synthetic completion timeout injection verifies software lifetime handling but does **not** simulate an actual GPU hang or device removal. A removed GPU still needs a separate recreation strategy; unresolved teardown may deliberately fail-stop rather than free live GPU resources.
- Neither the game executable nor its local DLLs, title shaders, TLS callbacks, DLL entry points or assets are executed or modified. Startup remains **BLOCKED** with 25 unresolved Xbox import references.
