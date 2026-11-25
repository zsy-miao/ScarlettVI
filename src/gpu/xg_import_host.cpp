#include "xg_import_host.h"
#include <array>
#include <cstddef>

namespace scarlett::gpu {

HRESULT WINAPI xg_create_texture_computer_host(
    const void* /*resource_desc*/, void** /*texture_computer*/) noexcept {
    // The title calls COM-like vtable +0x50 on a successful result.
    // Until the true Xbox texture layout and complete returned vtable are
    // available, returning any object or S_OK would corrupt the guest.
    // The guest output address may not be mapped or trusted: DO NOT write.
    return E_NOTIMPL;
}

HRESULT WINAPI xg_suggest_swizzle_mode_host(
    const void* /*resource_desc*/, uint32_t /*flags*/,
    void* /*texture_layout*/) noexcept {
    // XG_TEXTURE_LAYOUT and actual swizzle modes are Xbox-specific.
    // Windows D3D12 layout enums and the host Morton microtile are
    // NOT substitutes; refuse instead of fabricating a layout value.
    return E_NOTIMPL;
}

FARPROC find_xg_xs_error_only_export(
    const std::string& name) noexcept {
    if (name == "XGCreateTextureComputer")
        return reinterpret_cast<FARPROC>(&xg_create_texture_computer_host);
    if (name == "XGSuggestSwizzleMode")
        return reinterpret_cast<FARPROC>(&xg_suggest_swizzle_mode_host);
    return nullptr;
}

bool verify_xg_xs_error_only_fixture() noexcept {
    using CreateFn = HRESULT (WINAPI*)(const void*, void**) noexcept;
    using SuggestFn = HRESULT (WINAPI*)(
        const void*, uint32_t, void*) noexcept;
    const auto* create = reinterpret_cast<CreateFn>(
        find_xg_xs_error_only_export("XGCreateTextureComputer"));
    const auto* suggest = reinterpret_cast<SuggestFn>(
        find_xg_xs_error_only_export("XGSuggestSwizzleMode"));
    if (!create || !suggest ||
        find_xg_xs_error_only_export("XGCreateTexture2DComputer") ||
        find_xg_xs_error_only_export("XGComputeOptimalSwizzleMode") ||
        find_xg_xs_error_only_export("xgcreateswizzlecomputer"))
        return false;

    // Real host output memory must NOT be written even on E_NOTIMPL.
    // This includes pointer-to-pointer outputs, null outputs, invalid
    // descriptor addresses and unverified guest memory addresses.
    void* computer = reinterpret_cast<void*>(uintptr_t{0x11223344});
    std::array<uint8_t, 96> layout;
    layout.fill(0xa7);
    const void* untrusted =
        reinterpret_cast<const void*>(uintptr_t{0x1234});
    if (create(untrusted, &computer) != E_NOTIMPL ||
        computer != reinterpret_cast<void*>(uintptr_t{0x11223344}) ||
        create(nullptr, nullptr) != E_NOTIMPL ||
        create(untrusted,
            reinterpret_cast<void**>(uintptr_t{0x4567})) != E_NOTIMPL ||
        suggest(untrusted, 1, layout.data()) != E_NOTIMPL ||
        suggest(nullptr, 0, nullptr) != E_NOTIMPL ||
        suggest(untrusted, UINT32_MAX,
            reinterpret_cast<void*>(uintptr_t{0x4567})) != E_NOTIMPL)
        return false;
    for (const uint8_t byte : layout)
        if (byte != 0xa7) return false;
    return true;
}
} // namespace scarlett::gpu
