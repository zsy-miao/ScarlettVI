# GPU fence failure and deferred page retirement

**Status:** original Windows D3D12 host code verified with an RTX 5060 Ti and deterministic **synthetic failure injection**. This is not an actual GPU hang, real device removal, Xbox unified memory, or game execution.

## Why this matters

Previously `GpuPageStager::close()` could destroy command buffers, heaps, and tile mapping references even when a GPU fence wait failed. `GuestGpuPageRegistry::clear()` could then release its pinned CPU XMem aliases and GPU heap tile assignments before the GPU had confirmed completion. Reusing those memory resources while native GPU work may still reference them is unsafe.

The new cleanup is **retryable and fail-closed**:

- A failed pre-unmap fence returns its HRESULT while retaining the page's command queue, CPU pins, GPU heap tile, upload/readback buffers, and both live GPU mappings.
- After an acknowledged fence, the stager enqueues NULL GPU mappings. It records each successful native unmap so a partial failure can retry without duplicate unmap requests.
- The *second* fence must confirm the queued NULL mapping operations before any GPU heap memory, upload/readback buffer, or owning resource can be released.
- `unbind(frame)` keeps CPU XMem pins and the GPU tile assignment when its stager fails to close. `clear()` safely retires unaffected frames but **keeps failed entries and the shared heap/queue** until a successful retry.
- The stager and registry destructors use a deliberate **fail-stop** policy for unresolved retirement. Rather than freeing possibly still-used resources, they terminate the process if the caller destroys a registry without first completing cleanup. This is **not** a full GPU reset/recovery system.

## Distinguish a removed device from a completed fence

Microsoft's [ID3D12Fence::GetCompletedValue reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue) specifies that a removed D3D12 device makes `GetCompletedValue()` return **UINT64_MAX**. That sentinel cannot be treated as a successful "fence >= expected" comparison. [GetDeviceRemovedReason](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getdeviceremovedreason) supplies the corresponding failure reason.

The new original `src/gpu/fence_safety.h` classifies fence observations as pending, completed, or device removed. Compile-time assertions test all three cases. The host page stager and the registry's batch fence now check for device removal before reporting completed GPU readbacks or safe tile retirement.

## Fault-injection hardware verification

The `gpu-page-registry` command runs real native Direct3D 12 submissions and maps four 64 KiB guest pages into four physically distinct tiles of one shared 4 MiB D3D12 heap. The test validates the original 16 successful GPU page transfers, slot reuse, alias coherency checks and page lifetime.

It then exercises three separate synthetic failure cases, without triggering an actual GPU hang:

1. **Single-page wait failure after submission:** an actual GPU command list is executed but a simulated fence timeout prevents readback and CPU writeback. The native submission is counted but the page is not counted as a successful transfer. A repeat attempt returns `ERROR_BUSY` while it remains in flight. A second injected failure during `unbind` retains all pins/mappings, and a later real fence permits safe retry and one additional successful transfer.
2. **Four-page batch wait failure after submission:** four real GPU command lists execute together, but a simulated failure occurs before the shared batch fence signal. None of the four output buffers or XMem CPU aliases is published or modified. A second transfer attempt returns `ERROR_BUSY`.
3. **Failure after NULL tile mappings were queued:** the first teardown fence succeeds and both GPU VA aliases are unmapped in the host mapping index. The injected *second* fence failure keeps the frame's CPU pins, heap tile, shared queue and GPU resources alive despite a zero GPU-mapping count. A second `clear()` confirms the queued unmaps with a real fence and frees everything.

**Verified result:** 17 correctly read-back **65,536-byte** payloads; **10 actual GPU queue submissions** (including two deliberately unverified submissions); one shared D3D12 DIRECT queue; zero leaked CPU frames, pins and GPU mappings after retry. Both Release and Debug x64 are tested with the real RTX 5060 Ti.

One initial expanded test exceeded the default Windows stack with another 256 KiB local array; the extra batch result storage was moved to a heap-allocated vector.

## Fenced multi-page NULL unmap

The optional [pinned GPU cohort teardown](GPU_PAGE_BATCH_REGISTRATION.md)
extends this fail-closed policy to `GuestGpuPageRegistry::unbind_batch`.
For four adjacent registered frames, it fences all previous GPU work,
enqueues **two NULL `UpdateTileMappings` calls** (one per native GPU
alias resource, rather than eight per-page calls), then waits on a
shared completion fence **before releasing any CPU XMem pins or GPU tile
slots**. Synthetic failures before unmap, after the first NULL update
or after both NULL updates are covered by the separate `gpu-page-batch`
test. Pending groups can be retried without repeating already enqueued
NULL mappings, or `clear()` can safely retire the remaining GPU objects.
Actual GPU device removal/recreation is still unsupported.

## Limits

Synthetic test failure is *not* physical device removal or a real ten-second GPU timeout. An actual removed GPU cannot currently be recreated by ScarlettVI; unresolved native work must be reported to the caller and may require the fail-stop policy. CPU XMem pagefile allocations and GPU D3D12 heaps are still **physically separate**, with explicit readback/writeback rather than Xbox unified coherency.

The user's game executable and bundled DLLs remain read-only. No title entry point, TLS callback, DLL initializer, original Xbox shader or commercial game resource is executed. All 25 unresolved Xbox imports and title execution gates remain blocked.
