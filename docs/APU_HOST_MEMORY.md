# AcpHal APU host memory and heap-budget bridge

**Status: tested Windows CPU-memory subset, not an Xbox APU/DSP implementation.** ScarlettVI provides real aligned host allocations with independent accounting, but does not emulate console APU addresses, SHAPE, XMA/Opus codecs or sound output. Title execution remains BLOCKED.

## Three named Xbox imports now linked

| Name | Host-supported ABI and behavior |
| --- | --- |
| `ApuCreateHeap` | `HRESULT WINAPI ApuCreateHeap(uint32_t cached, uint32_t noncached)` — configure a process-wide host allocation ceiling; never preallocate Xbox hardware memory |
| `ApuAlloc` | `HRESULT WINAPI ApuAlloc(void** output, void* optionalDeviceAddressOutput, uint32_t bytes, uint32_t alignment, uint32_t flags)` — allocate real Windows CPU memory only when the optional device-address output is null and flags are zero |
| `ApuFree` | `void WINAPI ApuFree(void* pointer)` — release only exact owned host allocations |

### Evidence and caveats

Read-only MSVC disassembly confirms the title calls `ApuCreateHeap` at VA `0x1443AD933` with two 32-bit values: its first is a device-derived quantity scaled by `0x100000` and its second is `0x226E00` bytes. The return in EAX is checked as an HRESULT-style status (`test eax,eax; js`). Independent public interoperability notes describe the two values as cached/noncached heap budgets. That does not verify console residency, allocation priority or their hardware page type.

The title wrapper at VA `0x1443A6110` calls `ApuAlloc` with RCX=virtual output pointer, RDX=null, R8D=size, R9D=`0x800` alignment and a fifth DWORD in the Windows x64 outgoing stack argument. DirectXTK/Wwise examples use the older four-argument null-optional-output form. ScarlettVI now declares the five-argument form found in this title, but **supports only flags zero**. Unknown flags or a requested device-address output return `E_NOTIMPL` instead of inventing an APU physical address.

All three exports are resolved by the exact name in `AcpHal.dll`; no ordinal or unknown service is fabricated. No proprietary GDK headers or third-party GPL implementation code is copied.

Public references:
- https://github.com/xcl-dev/xcl/blob/main/HANDOFF.md
- https://github.com/OnlyMoisties/GTA6-BootTest/blob/main/REPORT.md
- https://git.peroxy.dev/kookroach/game-physics/src/commit/409a71248accd4b4cb585d7d2cce9667458b98ba/DirectXTK/Audio/SoundEffect.cpp

## Host memory ownership and budgeting

The implementation is original C++20 in `src/audio/apu_memory.{h,cpp}`. It uses `_aligned_malloc`/`_aligned_free` with a mutex-protected pointer registry and independent host byte counters.

- `ApuCreateHeap(0,0)` returns `E_INVALIDARG`. Valid nonzero budgets are interpreted as a **host quota**, not physical APU page allocations. Cached + noncached values are accumulated using 64-bit arithmetic and capped at 256 MiB for actual Windows process allocations.
- A repeated identical configuration is idempotent. A changed configuration with live allocations returns `HRESULT_FROM_WIN32(ERROR_BUSY)` and preserves the old quota.
- `ApuAlloc` validates non-null output, nonzero size, 4–65536-byte power-of-two alignment and a maximum single allocation of 64 MiB. Invalid requests return an error and set a valid output pointer to null.
- A successful allocation provides real zero-initialized writable host memory within the configured quota. The null-auxiliary-output, flags-zero subset works independently of whether the title has configured the budget yet; prior to configuration a conservative 256 MiB cap applies.
- `ApuFree` ignores null, non-owned, interior and already-freed pointers. The registry enforces ownership and prevents unrelated host allocations from being freed.

No audio data is submitted to an APU, decoded, rendered, or played.

## Synthetic reproducibility

```powershell
.\build\x64\Release\ScarlettVI.exe audio-memory-fixture
.\build\x64\Release\ScarlettVI.exe readiness '..\GTA6.Xbox.v1.0.2.1\game_scarlett_master.exe'
```

The host fixture tests signature and exact-name binding, eight sizes/alignments, real read/write storage and zeroing, invalid arguments, unknown flags and device-address output, foreign/interior/double freeing, four concurrent worker threads with 128 allocations each, empty/identical/reconfigured heap budgets, exhaustion rejection, unchanged quota after failed live reconfiguration, and max host cap. All bytes are generated for the test; guest code is never executed.

## Readiness and next real blockers

| Count | Before APU work | After alloc/free | Current after heap budgeting |
| --- | ---: | ---: | ---: |
| Pending `AcpHal.dll` references | 9 | 7 | **6** |
| Pending Xbox references overall | 23 | 21 | **20** |
| Experimental host APU linked exports | 0 | 2 | **3** |

The 20 unresolved references consist of `AcpHal.dll` 6, `chad.dll` 5, `xapu.dll` 5, `xg_xs.dll` 2, `XCRDAPI.dll` 1, `xmem.dll` 1.

**Next genuine engineering priority:** implement/test 32-bit guest APU device-address translation and actual APU/DSP command/result queues, with independently checked calling contracts. Binding the import is not proof that the game's audio startup will work.
