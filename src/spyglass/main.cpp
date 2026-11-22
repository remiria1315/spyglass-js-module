#include <Windows.h>

#include "spyglass/install.h"

BOOL WINAPI DllMain(const HINSTANCE module, const DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        CloseHandle(CreateThread(
            nullptr, 0, [](LPVOID) -> DWORD { spyglass::install(); return 0; }, nullptr, 0, nullptr));
    }
    return TRUE;
}
