// Runtime: DLL initialization and runtime status

module;

#include <Windows.h>

export module DwmOverlay.Core.Runtime;

export namespace DwmOverlay
{
enum class RuntimeStatus : DWORD
{
    NotStarted,
    Initializing,
    WaitingForFrame,
    Rendering,
    Unsupported,
    Failed,
};

bool          InitializeRuntime(HMODULE module);
void          SetRuntimeStatus(RuntimeStatus status);
RuntimeStatus GetRuntimeStatus();
} // namespace DwmOverlay
