#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scarlett::gpu {
struct DxilCompileResult {
    HRESULT status = E_FAIL;
    std::vector<uint8_t> binary;
    std::string diagnostics;
    std::wstring compiler_path;
};

// Compiles independently authored HLSL to Shader Model 6 DXIL with Microsoft's
// DirectXShaderCompiler from the installed Windows SDK. Only safe SDK lookup
// directories are searched; no title DLLs, proprietary shaders or binaries.
DxilCompileResult compile_compute_dxil(std::string_view source);
} // namespace scarlett::gpu
