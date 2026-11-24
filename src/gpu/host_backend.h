#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace scarlett::gpu {
// Represents host D3D12 capability, NOT console D3D12_XS compatibility.
struct DeviceReport {
    std::wstring adapter_name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint64_t dedicated_video_memory_bytes = 0;
    bool is_software = false;
    bool device_created = false;
    bool level_12_0 = false;
    bool queue_created = false;
    bool command_list_created = false;
    bool fence_created = false;
    bool upload_buffer_created = false;
    bool root_signature_created = false;
    uint32_t node_count = 0;
    HRESULT result = E_FAIL;
};
std::vector<DeviceReport> probe_host_adapters();
} // namespace scarlett::gpu
