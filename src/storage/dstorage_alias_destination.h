#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace scarlett::storage {

// HOST-ONLY DirectStorage destination pin for a real XMem CPU alias.
// A trusted test must provide the exact synthetic 64KiB physical frame.
// Pinning rejects unknown addresses/frames and prevents the backing alias
// from being unmapped while an asynchronous read writes its bytes.
// Not a console GPU destination, page-list mode, or untrusted guest API.
class HostDStorageAliasDestination final {
public:
    static constexpr size_t page_bytes = 64 * 1024;

    static HRESULT pin(void* destination, uintptr_t physical_frame,
                       size_t bytes,
                       std::shared_ptr<HostDStorageAliasDestination>& output)
                       noexcept;
    ~HostDStorageAliasDestination() noexcept;
    HostDStorageAliasDestination(const HostDStorageAliasDestination&) = delete;
    HostDStorageAliasDestination& operator=(const HostDStorageAliasDestination&) = delete;

    size_t size() const noexcept { return bytes_; }
    HRESULT write(const uint8_t* source, size_t bytes) noexcept;

private:
    HostDStorageAliasDestination(void* page_base, uintptr_t frame,
                                 void* destination, size_t bytes) noexcept
        : page_base_(page_base), frame_(frame),
          destination_(destination), bytes_(bytes) {}
    void* page_base_ = nullptr;
    uintptr_t frame_ = 0;
    void* destination_ = nullptr;
    size_t bytes_ = 0;
    bool pinned_ = false;
};

} // namespace scarlett::storage
