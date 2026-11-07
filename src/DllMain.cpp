// DllMain: DLL entry point

#include <Windows.h>

import DwmOverlay.Core.Runtime;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        return DwmOverlay::InitializeRuntime(module) ? TRUE : FALSE;
    return TRUE;
}
