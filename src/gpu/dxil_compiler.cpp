#include "dxil_compiler.h"
#include <objbase.h>
#include <objidl.h>
#include <oleauto.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <system_error>

namespace scarlett::gpu {
namespace {
using Microsoft::WRL::ComPtr;
using CreateCompiler = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
struct DxcLibrary {
    HMODULE dll = nullptr;
    std::wstring path;
    std::string failure;
    ~DxcLibrary() { if (dll) FreeLibrary(dll); }
    DxcLibrary(const DxcLibrary&) = delete;
    DxcLibrary& operator=(const DxcLibrary&) = delete;
    DxcLibrary() = default;

    bool load() {
        wchar_t program_files[32768]{};
        const DWORD size = GetEnvironmentVariableW(
            L"ProgramFiles(x86)", program_files, static_cast<DWORD>(std::size(program_files)));
        if (!size || size >= std::size(program_files)) {
            failure = "ProgramFiles(x86) environment variable unavailable";
            return false;
        }
        std::filesystem::path sdk_root =
            std::filesystem::path(program_files) / L"Windows Kits" / L"10" / L"bin";
        std::error_code ec;
        if (!std::filesystem::is_directory(sdk_root, ec)) {
            failure = "Windows Kits/10/bin SDK directory not accessible";
            return false;
        }
        std::vector<std::filesystem::path> candidates;
        WIN32_FIND_DATAW entry{};
        const auto pattern = sdk_root / L"*";
        HANDLE finder = FindFirstFileW(pattern.c_str(), &entry);
        if (finder == INVALID_HANDLE_VALUE) {
            failure = "SDK version enumeration failed, Win32 error " +
                      std::to_string(GetLastError());
            return false;
        }
        do {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                entry.cFileName[0] == L'.')
                continue;
            const auto candidate_path =
                sdk_root / entry.cFileName / L"x64" / L"dxcompiler.dll";
            const DWORD attributes = GetFileAttributesW(candidate_path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                candidates.push_back(candidate_path);
        } while (FindNextFileW(finder, &entry));
        FindClose(finder);
        std::sort(candidates.rbegin(), candidates.rend());
        if (candidates.empty()) {
            failure = "No x64 dxcompiler.dll in installed Windows SDK versions";
            return false;
        }
        DWORD last_load_error = ERROR_MOD_NOT_FOUND;
        for (const auto& candidate : candidates) {
            dll = LoadLibraryExW(candidate.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (dll) {
                path = candidate.wstring();
                return true;
            }
            last_load_error = GetLastError();
        }
        failure = "Windows SDK dxcompiler.dll LoadLibraryEx failed with Win32 error " +
                  std::to_string(last_load_error);
        return false;
    }
};
}

DxilCompileResult compile_compute_dxil(std::string_view source) {
    DxilCompileResult output;
    if (source.empty() || source.size() > 1024 * 1024) {
        output.status = E_INVALIDARG;
        output.diagnostics = "HLSL source was empty or exceeded 1 MiB";
        return output;
    }
    DxcLibrary dll;
    if (!dll.load()) {
        output.status = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        output.diagnostics = dll.failure;
        return output;
    }
    output.compiler_path = dll.path;
    const auto create = reinterpret_cast<CreateCompiler>(
        GetProcAddress(dll.dll, "DxcCreateInstance"));
    if (!create) {
        output.status = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        output.diagnostics = "Installed DXC lacks DxcCreateInstance";
        return output;
    }
    ComPtr<IDxcCompiler3> compiler;
    output.status = create(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf()));
    if (FAILED(output.status)) return output;

    const DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
    LPCWSTR args[] = {
        L"-E", L"main", L"-T", L"cs_6_0", L"-O3", L"-HV", L"2021"
    };
    ComPtr<IDxcResult> result;
    output.status = compiler->Compile(
        &buffer, args, static_cast<UINT32>(std::size(args)), nullptr,
        IID_PPV_ARGS(result.GetAddressOf()));
    if (FAILED(output.status)) return output;

    ComPtr<IDxcBlobUtf8> diagnostics;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS,
                                    IID_PPV_ARGS(diagnostics.GetAddressOf()), nullptr)) &&
        diagnostics && diagnostics->GetStringLength()) {
        output.diagnostics.assign(diagnostics->GetStringPointer(),
                                  diagnostics->GetStringLength());
    }
    HRESULT compiler_status = E_FAIL;
    output.status = result->GetStatus(&compiler_status);
    if (FAILED(output.status)) return output;
    output.status = compiler_status;
    if (FAILED(output.status)) return output;

    ComPtr<IDxcBlob> compiled;
    output.status = result->GetOutput(
        DXC_OUT_OBJECT, IID_PPV_ARGS(compiled.GetAddressOf()), nullptr);
    if (FAILED(output.status) || !compiled || compiled->GetBufferSize() == 0) {
        output.status = E_FAIL;
        return output;
    }
    const auto* bytes = static_cast<const uint8_t*>(compiled->GetBufferPointer());
    output.binary.assign(bytes, bytes + compiled->GetBufferSize());
    return output;
}
} // namespace scarlett::gpu
