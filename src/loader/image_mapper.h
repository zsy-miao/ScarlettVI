#pragma once
#include "../pe/pe_file.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scarlett {
struct ImageFinalizeReport {
    size_t sections_protected = 0;
    size_t unwind_functions_registered = 0;
    size_t tls_slots_prepared = 0;
    size_t tls_template_bytes = 0;
    size_t deferred_tls_callbacks = 0;
};
class MappedImage final {
public:
    explicit MappedImage(const PeFile& pe);
    ~MappedImage();
    MappedImage(const MappedImage&) = delete;
    MappedImage& operator=(const MappedImage&) = delete;
    uint64_t base_address() const noexcept { return reinterpret_cast<uint64_t>(base_); }
    size_t applied_relocations() const noexcept { return relocations_; }
    uint32_t entry_rva() const noexcept { return entry_rva_; }
    void patch_iat(uint32_t rva, uint64_t absolute_address);
    // Call only after every import has been patched. Sets up current-thread
    // static TLS, section page protections and x64 unwind metadata.
    // TLS callbacks and title code are never executed.
    ImageFinalizeReport finalize_image();
private:
    uint8_t* base_ = nullptr;
    size_t size_ = 0;
    size_t relocations_ = 0;
    uint32_t entry_rva_ = 0;
    uint32_t exception_rva_ = 0;
    uint32_t exception_size_ = 0;
    uint32_t tls_rva_ = 0;
    DWORD tls_index_ = TLS_OUT_OF_INDEXES;
    std::vector<uint8_t> tls_template_;
    std::vector<SectionInfo> sections_;
    PRUNTIME_FUNCTION registered_unwind_ = nullptr;
    ImageFinalizeReport finalized_result_{};
    bool finalized_ = false;
    void prepare_initial_thread_tls();
    uint8_t* va_in_image(uint64_t address, size_t byte_count) const;
};
} // namespace scarlett
