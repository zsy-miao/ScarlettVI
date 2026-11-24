#include "image_mapper.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace scarlett {
MappedImage::MappedImage(const PeFile& pe) {
    const auto& d = pe.details();
    size_ = d.image_size;
    entry_rva_ = d.entry_rva;
    exception_rva_ = d.exception_rva;
    exception_size_ = d.exception_size;
    tls_rva_ = d.tls_rva;
    sections_ = d.sections;
    // Reserve preferred image base when it is free; otherwise relocate.
    base_ = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(d.preferred_base),
                                                size_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base_) base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base_) throw std::runtime_error("VirtualAlloc failed to reserve PE image address space");
    try {
        memcpy(base_, pe.bytes().data(), d.headers_size);
        for (const auto& s : d.sections) {
            if (!s.file_size) continue;
            memcpy(base_ + s.rva, pe.bytes().data() + s.file_offset, s.file_size);
        }
        const int64_t delta = static_cast<int64_t>(reinterpret_cast<uint64_t>(base_) - d.preferred_base);
        if (delta) {
            if (!d.relocation_rva || !d.relocation_size)
                throw std::runtime_error("Image is not relocatable, and preferred base was unavailable");
            uint64_t pos = d.relocation_rva;
            const uint64_t end = pos + d.relocation_size;
            if (end > size_) throw std::runtime_error("Relocation directory lies outside mapped PE");
            while (pos < end) {
                if (end - pos < sizeof(IMAGE_BASE_RELOCATION))
                    throw std::runtime_error("Truncated base-relocation block");
                const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(base_ + pos);
                if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || block->SizeOfBlock > end - pos ||
                    (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) % sizeof(uint16_t))
                    throw std::runtime_error("Invalid base-relocation block size");
                const auto* entries = reinterpret_cast<const uint16_t*>(base_ + pos + sizeof(IMAGE_BASE_RELOCATION));
                const uint32_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
                for (uint32_t i = 0; i < count; ++i) {
                    const uint16_t type = entries[i] >> 12;
                    const uint32_t offset = entries[i] & 0x0fff;
                    if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
                    if (type != IMAGE_REL_BASED_DIR64)
                        throw std::runtime_error("Unsupported x64 relocation type " + std::to_string(type));
                    const uint64_t target = uint64_t(block->VirtualAddress) + offset;
                    if (target + sizeof(uint64_t) > size_)
                        throw std::runtime_error("Base relocation targets outside image");
                    auto* value = reinterpret_cast<uint64_t*>(base_ + target);
                    *value = static_cast<uint64_t>(static_cast<int64_t>(*value) + delta);
                    ++relocations_;
                }
                pos += block->SizeOfBlock;
            }
        }
        // This is intentionally a data-only mapping: title code, TLS callbacks and
        // DllMain are NEVER executed until import bridging and exception handling exist.
    } catch (...) {
        VirtualFree(base_, 0, MEM_RELEASE);
        base_ = nullptr;
        throw;
    }
}
uint8_t* MappedImage::va_in_image(uint64_t address, size_t bytes) const {
    if (address < base_address() || address - base_address() > size_ ||
        bytes > size_ - static_cast<size_t>(address - base_address()))
        throw std::runtime_error("Mapped PE TLS address is outside its image");
    return base_ + (address - base_address());
}

void MappedImage::prepare_initial_thread_tls() {
    if (!tls_rva_ || tls_index_ != TLS_OUT_OF_INDEXES) return;
    if (uint64_t(tls_rva_) + sizeof(IMAGE_TLS_DIRECTORY64) > size_)
        throw std::runtime_error("PE TLS directory is outside mapped image");
    const auto tls = *reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base_ + tls_rva_);
    if (!tls.AddressOfIndex) throw std::runtime_error("PE TLS index address missing");
    uint8_t* index_location = va_in_image(tls.AddressOfIndex, sizeof(DWORD));
    if (tls.EndAddressOfRawData < tls.StartAddressOfRawData)
        throw std::runtime_error("Invalid PE TLS raw data bounds");
    const uint64_t template_size = tls.EndAddressOfRawData - tls.StartAddressOfRawData;
    const uint64_t total = template_size + tls.SizeOfZeroFill;
    if (total > 32u * 1024u * 1024u)
        throw std::runtime_error("PE TLS template exceeds supported size");
    if (template_size) va_in_image(tls.StartAddressOfRawData, static_cast<size_t>(template_size));

    size_t callbacks = 0;
    if (tls.AddressOfCallBacks) {
        for (; callbacks < 4096; ++callbacks) {
            const auto* address = reinterpret_cast<const uint64_t*>(
                va_in_image(tls.AddressOfCallBacks + callbacks * sizeof(uint64_t),
                            sizeof(uint64_t)));
            if (*address == 0) break;
            va_in_image(*address, 1); // only validate; NEVER execute callbacks
        }
        if (callbacks == 4096) throw std::runtime_error("Unterminated PE TLS callback array");
    }
    tls_index_ = TlsAlloc();
    if (tls_index_ == TLS_OUT_OF_INDEXES)
        throw std::runtime_error("Cannot allocate Windows TLS slot for mapped image");
    tls_template_.resize(static_cast<size_t>(std::max<uint64_t>(1, total)), 0);
    if (template_size)
        memcpy(tls_template_.data(),
               va_in_image(tls.StartAddressOfRawData, static_cast<size_t>(template_size)),
               static_cast<size_t>(template_size));
    if (!TlsSetValue(tls_index_, tls_template_.data()))
        throw std::runtime_error("Cannot install initial mapped PE TLS data");
    memcpy(index_location, &tls_index_, sizeof(DWORD));
    finalized_result_.tls_slots_prepared = 1;
    finalized_result_.tls_template_bytes = static_cast<size_t>(total);
    finalized_result_.deferred_tls_callbacks = callbacks;
}

ImageFinalizeReport MappedImage::finalize_image() {
    if (finalized_) return finalized_result_;
    prepare_initial_thread_tls();
    // RUNTIME_FUNCTION contains 3 x64 relative DWORDs, as required by
    // RtlAddFunctionTable. We do not execute any mapped title code here.
    if (exception_rva_ && exception_size_) {
        if (exception_size_ % sizeof(RUNTIME_FUNCTION) != 0 ||
            uint64_t(exception_rva_) + exception_size_ > size_)
            throw std::runtime_error("Invalid x64 PE exception directory");
        auto* functions = reinterpret_cast<PRUNTIME_FUNCTION>(base_ + exception_rva_);
        const DWORD count = exception_size_ / sizeof(RUNTIME_FUNCTION);
        for (DWORD i = 0; i < count; ++i) {
            const auto& entry = functions[i];
            if (entry.BeginAddress >= entry.EndAddress ||
                entry.EndAddress > size_ || entry.UnwindData >= size_)
                throw std::runtime_error("PE exception function entry lies outside image");
        }
        if (!RtlAddFunctionTable(functions, count, base_address()))
            throw std::runtime_error("RtlAddFunctionTable failed for PE image");
        registered_unwind_ = functions;
        finalized_result_.unwind_functions_registered = count;
    }

    for (const auto& section : sections_) {
        const uint64_t length = std::max(section.virtual_size, section.file_size);
        if (!length) continue;
        const bool can_execute = (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        const bool can_read = (section.characteristics & IMAGE_SCN_MEM_READ) != 0;
        const bool can_write = (section.characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        DWORD protection = PAGE_NOACCESS;
        if (can_execute)
            protection = can_write ? PAGE_EXECUTE_READWRITE : (can_read ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
        else
            protection = can_write ? PAGE_READWRITE : (can_read ? PAGE_READONLY : PAGE_NOACCESS);
        DWORD old_protection = 0;
        if (!VirtualProtect(base_ + section.rva, static_cast<SIZE_T>(length),
                            protection, &old_protection))
            throw std::runtime_error("VirtualProtect failed finalizing PE section " + section.name);
        ++finalized_result_.sections_protected;
    }
    // Refresh instruction caches only after all writes and protection changes.
    if (!FlushInstructionCache(GetCurrentProcess(), base_, size_))
        throw std::runtime_error("FlushInstructionCache failed");
    finalized_ = true;
    return finalized_result_;
}

void MappedImage::patch_iat(uint32_t rva, uint64_t absolute_address) {
    if (finalized_)
        throw std::logic_error("Cannot bind PE imports after finalizing section protections");
    if (uint64_t(rva) + sizeof(uint64_t) > size_)
        throw std::runtime_error("Import address table slot is outside the mapped image");
    memcpy(base_ + rva, &absolute_address, sizeof(absolute_address));
}
MappedImage::~MappedImage() {
    if (registered_unwind_) RtlDeleteFunctionTable(registered_unwind_);
    if (tls_index_ != TLS_OUT_OF_INDEXES) TlsFree(tls_index_);
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
}
} // namespace scarlett
