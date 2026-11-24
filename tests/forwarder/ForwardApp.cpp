#include <Windows.h>
// Imports the forwarded function via ForwardBridge.dll. The loader should map
// FixtureBase.dll even though it does not appear in this EXE's import table.
extern "C" __declspec(dllimport) int __stdcall SvForward();
int main() {
    return SvForward() == 73 ? 0 : 1;
}
