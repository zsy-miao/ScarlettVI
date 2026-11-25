# Public Xbox Series compatibility references and ScarlettVI integration plan

**Research date:** 2026-11-25. This inventory describes public projects, **not** verified compatibility with this user's game build. Every genuine console interface still requires a ScarlettVI-specific ABI contract and original synthetic verification.

## Relevant public sources

| Project | Public strengths / relevance | Integration limit |
| --- | --- | --- |
| [XCL](https://github.com/xcl-dev/xcl) | Its authors report working native Xbox Series title execution; its docs cover XMem, graphics, console DirectStorage, shader translation, audio, TLS and virtual filesystem. | GPL-2.0 licensed. Do not copy implementation code into ScarlettVI without proper licensing and a separately reviewed integration plan. Its supported-title table does not verify this GTA VI build. |
| [ScarlettX](https://github.com/Freminet/ScarlettX) | C++ experimental Xbox Series PE, D3D12, AcpHal and sample service adapters with synthetic tests. | Many components are partial, several tests remain failing; console-specific ABI assumptions are not automatically valid for ScarlettVI. Check code licensing before any reuse. |
| [Xbox2Windows](https://github.com/R0Y00/Xbox2Windows) | Public experiments document caller-argument shapes for XMem queries, Windows GPU/PE research and conservative error paths. | An experimental research notebook, not an Xbox compatibility contract. Confirm locally before binding. |
| [Microsoft DirectStorage for Xbox](https://learn.microsoft.com/en-us/xbox/gdk/docs/features/console/storage/directstorage/directstorage-overview) | Publicly documents Xbox `IDStorageFactoryX` / `X1` / `X2`, queues and distinct async semantics. | The Xbox factory is **not** the same COM interface as PC `IDStorageFactory`; direct export forwarding is unsafe. |

XCL's public handoff specifically reports an older middleware stack corruption caused by writing a 0x78-byte XMem working-set structure into a smaller buffer. ScarlettVI's new capability policy explicitly **does not write** a guessed output layout. The XCL project also reports importing console middleware with similar names; this is **not evidence that GTA VI boots or renders correctly here**.

## Current ScarlettVI blockers

From the original game build's **read-only readiness inventory** after the safe working-set failure-path change:

| Workstream | Pending import references | Concrete technical gate |
| --- | ---: | --- |
| Xbox APU and DSP (`AcpHal.dll` / `xapu.dll` / `chad.dll`) | 13 | [APU host heap](APU_HOST_MEMORY.md) and [owned address translator](APU_ADDRESS_TRANSLATION.md) now bound; external/XMem APU ranges, SHAPE, DSP queues and lifecycle remain |
| Xbox texture/graphics (`xg_xs.dll`) | 2 | Guest texture-layout/swizzle ABI, object dispatch, GPU memory and shader interfaces |
| Xbox DirectStorage (`XCRDAPI.dll`) | 1 | Xbox factory and queue COM vtables, file/memory request parsing, queue completion |
| Xbox memory (`xmem.dll`) | 1 | `XMemTransferMemory` partition/budget-transfer semantics; do not manufacture success |
| **Total** | **17** | |

An unresolved-import count of 0 **would not** be an execution test or bootability guarantee. The runtime still lacks parts of D3D12_XS/RDNA2 resource and shader translation, audio hardware behavior, file system/process initialization, static TLS lifecycle and fault isolation.

## Implement in this order

1. **Memory transfer:** independently corroborate the real call arguments and supported partition transfers, then implement a measured, capacity-checked host memory-budget service; otherwise keep `XMemTransferMemory` unbound.
2. **Audio HLE:** [bounded host APU heap](APU_HOST_MEMORY.md) and [32-bit APU mappings](APU_ADDRESS_TRANSLATION.md) now pass allocation, alignment, round-trip, ownership and concurrent release tests. Next establish APU visibility and lifetimes for externally owned guest/XMem memory and implement SHAPE/DSP command and result queues with verified contracts. The owned host address map is not console physical APU RAM.
3. **DirectStorage:** use the public Xbox `IDStorageFactoryX2` API specification to build an original guest-facing COM adapter, with exact IID and vtable/queue ABI tests. Do not pass the desktop factory pointer through unchanged.
4. **Graphics:** implement the guest XG object interfaces and texture/GFX10 swizzle with validated formats and PC D3D12-backed allocations, then work on precompiled RDNA2 shader translation and command semantics.
5. **Real execution gate:** test a **synthetic** generated PE title end-to-end through process attach, DLL TLS, imported services and fault diagnostics before considering any change to the existing title execution safeguard.

Keep the original proprietary build entirely outside the ScarlettVI source tree and read-only. No proprietary binaries or SDK headers belong in Git.
