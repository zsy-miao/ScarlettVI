# Architecture comparison: VIctory AMD fork and ztrx

**Researched 2026-11-25.** The user provided two public GitHub references to guide ScarlettVI. Their GPL-3.0-or-later source was **not copied or vendored**; ScarlettVI stays an independently implemented C++20 project. The notes below are architectural observations and self-reported upstream results, not tests of ScarlettVI.

## Public projects

- `Belobug/VIctory-Belobug-AMD`: https://github.com/Belobug/VIctory-Belobug-AMD — AMD-focused Rust fork of VIctory. Its README reports menus and world rendering on a Radeon RX 9070 XT at around 3–5 FPS, and flags substantial performance, lighting and memory limits. Those are upstream developer reports.
- `Zettrox/ztrx`: https://github.com/Zettrox/ztrx — NVIDIA-focused Rust fork. Its README reports boot and some gameplay on a GeForce RTX 3050 at around 1–3 FPS, with broken indirect lighting and other rendering problems. No performance result is verified here.
- `Marvisak/VIctory`: https://github.com/Marvisak/VIctory — originating architecture; its public docs describe a native x86-64 image loader, guest console services, GPU command translation to D3D12, shader recomposition into DXIL and content-addressed caches.

## Local applicability

Connected Windows workstation reports an NVIDIA RTX 5060 Ti via `Win32_VideoController`. The NVIDIA ztrx reference is therefore more relevant than an AMD-specific rendering workaround, but that alone establishes neither hardware support nor successful boot. ScarlettVI's native GPU tile, staging and texture-layout fixtures are not a shader translator.

## What the comparison changes in ScarlettVI's priorities

1. **Native-loader startup, not import counts alone:** the [generated PE64 and DLL startup fixtures](SYNTHETIC_PE_ENTRY.md) now prove original known-code entry, Windows imports, DIR64 relocation, explicit generated DLL callback/entrypoint ordering and separate TLS storage for managed host threads. Next implement real guest DLL dependency attach, compiler TLS/TEB conventions, automatic game-created-thread notifications and crash reporting. Real title execution remains blocked.
2. **Guest audio HAL and result queues:** the [AcpHal host object](APU_HAL_FACTORY.md) is partial (`+0x10` configuration unsupported). The [XAPU result FIFO](XAPU_QUEUE_HOST.md) implements real host connection and result ownership, reducing remaining XAPU/CHAD imports to six; `XApuEnqueueCommand` and all five CHAD services still need independently verified command-processing semantics.
3. **Xbox GPU program translation:** RDNA2 shader instruction/descriptor lowering to valid DXIL and shader caching are mandatory for graphics beyond synthetic D3D12 tests.
4. **Texture layout and streaming:** native swizzle and resource copying must reflect all guest memory aliases, and assets need independent archive and decompression verification.
5. **Xbox DirectStorage and online/offline services:** define guest COM interfaces, asynchronous completion and a contained virtual file system before any title entrypoint can run reliably.

Do not add placeholders that merely return success, enable guest execution prematurely, bundle the user's game executable, or copy implementation files from GPL projects without understanding licence requirements. The upstream repositories report a working boot while ScarlettVI currently reports `TITLE EXECUTION: BLOCKED`; the two are not equivalent.
