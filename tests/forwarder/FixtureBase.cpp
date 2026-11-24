#include <Windows.h>
// Private, generated test DLL. No Xbox or title code is used.
extern "C" __declspec(dllexport) int __stdcall SvOriginal() {
    return 73;
}
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
