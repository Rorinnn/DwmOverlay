// Runtime: DWM initialization and process lifetime

module;

#include <Windows.h>
#include <process.h>

module DwmOverlay.Core.Runtime;

import std;

import DwmOverlay.Core.FrameRuntime;
import DwmOverlay.Hook.DWMHooks;
import DwmOverlay.Input.WindowInput;

namespace DwmOverlay
{
namespace
{
#pragma region RuntimeState

std::atomic<RuntimeStatus> runtimeStatus = RuntimeStatus::NotStarted;

#pragma endregion

#pragma region HelperDeclarations

bool                   IsDWMProcess();
unsigned int __stdcall RunRuntimeThread(void*) noexcept;

#pragma endregion
} // namespace

#pragma region PublicFunctions

bool InitializeRuntime(HMODULE module)
{
    if (!IsDWMProcess())
    {
        SetRuntimeStatus(RuntimeStatus::Unsupported);
        return false;
    }
    auto previous = RuntimeStatus::NotStarted;
    if (!runtimeStatus.compare_exchange_strong(previous, RuntimeStatus::Initializing))
        return false;

    HMODULE           registeredModule = nullptr;
    PRUNTIME_FUNCTION functionTable    = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(module),
                           &registeredModule))
    {
        // DWM hook entry points must remain valid until process exit.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(module),
                                &registeredModule))
        {
            SetRuntimeStatus(RuntimeStatus::Failed);
            return false;
        }
    }
    else
    {
        // Manually mapped modules need their x64 unwind table registered before the runtime thread starts.
        auto*       imageBase = reinterpret_cast<BYTE*>(module);
        const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE ||
            dosHeader->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)))
        {
            SetRuntimeStatus(RuntimeStatus::Failed);
            return false;
        }
        const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dosHeader->e_lfanew);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
            ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            ntHeaders->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
        {
            SetRuntimeStatus(RuntimeStatus::Failed);
            return false;
        }
        const auto& directory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!directory.VirtualAddress || !directory.Size ||
            directory.VirtualAddress >= ntHeaders->OptionalHeader.SizeOfImage ||
            directory.Size > ntHeaders->OptionalHeader.SizeOfImage - directory.VirtualAddress ||
            directory.Size % sizeof(RUNTIME_FUNCTION))
        {
            SetRuntimeStatus(RuntimeStatus::Failed);
            return false;
        }
        functionTable = reinterpret_cast<PRUNTIME_FUNCTION>(imageBase + directory.VirtualAddress);
        if (!RtlAddFunctionTable(functionTable,
                                 static_cast<DWORD>(directory.Size / sizeof(RUNTIME_FUNCTION)),
                                 reinterpret_cast<DWORD64>(imageBase)))
        {
            SetRuntimeStatus(RuntimeStatus::Failed);
            return false;
        }
    }
    const HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, RunRuntimeThread, nullptr, 0, nullptr));
    if (!thread)
    {
        if (functionTable)
            RtlDeleteFunctionTable(functionTable);
        SetRuntimeStatus(RuntimeStatus::Failed);
        return false;
    }
    CloseHandle(thread);
    return true;
}

void SetRuntimeStatus(RuntimeStatus status)
{
    if (status == RuntimeStatus::Rendering)
    {
        auto previous = RuntimeStatus::WaitingForFrame;
        runtimeStatus.compare_exchange_strong(previous, status, std::memory_order_relaxed);
        return;
    }
    runtimeStatus.store(status, std::memory_order_release);
}

RuntimeStatus GetRuntimeStatus()
{
    return runtimeStatus.load(std::memory_order_acquire);
}

#pragma endregion

namespace
{
#pragma region Helpers

bool IsDWMProcess()
{
    std::array<wchar_t, 32768> path{};
    const DWORD                length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        return false;
    const auto* fileName = std::wcsrchr(path.data(), L'\\');
    return fileName && _wcsicmp(fileName + 1, L"dwm.exe") == 0;
}

bool RunRuntime()
{
    while (!GetModuleHandleW(L"dwmcore.dll"))
        Sleep(100);
    if (!InitializeWindowInput())
        return false;
    SetRuntimeStatus(RuntimeStatus::WaitingForFrame);
    if (!InstallDWMHooks())
        return false;
    return RunFrameLoop();
}

unsigned int __stdcall RunRuntimeThread(void*) noexcept
{
    try
    {
        if (!RunRuntime())
            SetRuntimeStatus(RuntimeStatus::Failed);
    }
    catch (...)
    {
        SetRuntimeStatus(RuntimeStatus::Failed);
    }

    if (TerminateProcess(GetCurrentProcess(), 0))
        return 0;
    const DWORD error = GetLastError();
    SetRuntimeStatus(RuntimeStatus::Failed);
    return error;
}

#pragma endregion
} // namespace
} // namespace DwmOverlay
