// DWMHooks: DWM presentation, render targets, and full redraws

module;

#include <Windows.h>
#include <TlHelp32.h>
#include <d3d11.h>
#include <detours.h>
#include <wrl/client.h>

module DwmOverlay.Hook.DWMHooks;

import std;

import DwmOverlay.Core.Runtime;
import DwmOverlay.Graphics.Compositor;
import DwmOverlay.Hook.DWMSignatures;
import DwmOverlay.Memory.SignatureScanner;

namespace DwmOverlay
{
namespace
{
#pragma region Types

using Microsoft::WRL::ComPtr;

using SwapChainPresentFunction        = HRESULT        (*)(void*, const void*, UINT, UINT, const void*);
using OverlayPresentFunction          = HRESULT          (*)(void*, void*, UINT, UINT, bool);
using OverlayRectsPresentFunction     = HRESULT     (*)(void*, void*, UINT, const void*, UINT, bool);
using OverlayStatusPresentFunction    = HRESULT    (*)(void*, void*, UINT, const void*, UINT, bool*, bool);
using GetBufferFunction               = HRESULT               (*)(void*, UINT, UINT*, ID3D11Texture2D**);
using GetBufferIndexFunction          = UINT             (*)(void*, UINT);
using GetTargetFunction               = void*                 (*)(void*);
using GetViewFunction                 = ID3D11RenderTargetView* (*)(void*);
using PresentNeededFunction           = bool              (*)(void*);
using ManagerPresentFunction          = HRESULT          (*)(void*);
using ManagerPresentWithCountFunction = HRESULT (*)(void*, UINT, UINT*);
using ForceFullRenderFunction         = void            (*)(void*);
using ScheduleCompositionFunction     = void        (*)(UINT, UINT);

struct SymbolMatch
{
    void*               Address   = nullptr;
    const DWMSignature* Signature = nullptr;
};

using SymbolMatches = std::array<SymbolMatch, static_cast<std::size_t>(DWMSymbol::Count)>;

struct HandleCloser
{
    void operator()(void* handle) const
    {
        CloseHandle(handle);
    }
};

using Handle = std::unique_ptr<void, HandleCloser>;

#pragma endregion

#pragma region RuntimeState

const DWMSignature*               presentSignature              = nullptr;
std::atomic<void*>                originalPresent               = nullptr;
std::atomic<void*>                originalManagerPresent        = nullptr;
std::atomic<void*>                originalAlwaysNeedsRenderPass = nullptr;
std::array<std::atomic<void*>, 2> originalPresentNeeded{};
std::atomic<bool>                 isDWMDrawingEnabled = false;
std::mutex                        frameMutex;
ForceFullRenderFunction           forceFullRender               = nullptr;
ScheduleCompositionFunction       scheduleCompositionPass       = nullptr;

#pragma endregion

#pragma region HelperDeclarations

bool             FindDWMSymbols(SymbolMatches& matches);
ID3D11Texture2D* GetBackBuffer(void* swapChain) noexcept;
void             RenderDWMFrame(void* swapChain) noexcept;
void             RequestFullRender(void* manager);
HRESULT          HookedSwapChainPresent(
    void* swapChain, const void* region, UINT syncInterval, UINT flags, const void* parameters);
HRESULT HookedOverlayPresent(void* context, void* swapChain, UINT flags, UINT interval, bool disableMPO);
HRESULT HookedOverlayRectsPresent(
    void* context, void* swapChain, UINT flags, const void* rects, UINT interval, bool disableMPO);
HRESULT HookedOverlayStatusPresent(
    void* context, void* swapChain, UINT flags, const void* rects, UINT interval, bool* status, bool disableMPO);
HRESULT                           HookedManagerPresent(void* manager);
HRESULT                           HookedManagerPresentWithCount(void* manager, UINT interval, UINT* count);
bool                              HookedAlwaysNeedsRenderPass(void* manager);
template <std::size_t index> bool HookedPresentNeeded(void* target);

#pragma endregion

} // namespace

#pragma region PublicFunctions

bool InstallDWMHooks()
{
    SymbolMatches matches{};
    if (!FindDWMSymbols(matches))
        return false;
    const auto getMatch = [&matches](DWMSymbol symbol) -> const SymbolMatch&
    {
        return matches[static_cast<std::size_t>(symbol)];
    };

    void*                presentAddress = nullptr;
    void*                presentHook    = nullptr;
    constexpr std::array PresentSymbols{DWMSymbol::SwapChainPresent,
                                        DWMSymbol::OverlayPresent,
                                        DWMSymbol::OverlayRectsPresent,
                                        DWMSymbol::OverlayStatusPresent};
    const std::array     presentHooks{reinterpret_cast<void*>(HookedSwapChainPresent),
                                      reinterpret_cast<void*>(HookedOverlayPresent),
                                      reinterpret_cast<void*>(HookedOverlayRectsPresent),
                                      reinterpret_cast<void*>(HookedOverlayStatusPresent)};
    for (std::size_t i = 0; i < PresentSymbols.size(); ++i)
    {
        const auto& match = getMatch(PresentSymbols[i]);
        if (!match.Address)
            continue;
        if (presentAddress)
            return false;
        presentAddress   = match.Address;
        presentHook      = presentHooks[i];
        presentSignature = match.Signature;
    }
    if (!presentAddress)
        return false;

    scheduleCompositionPass =
        reinterpret_cast<ScheduleCompositionFunction>(getMatch(DWMSymbol::ScheduleComposition).Address);
    forceFullRender = reinterpret_cast<ForceFullRenderFunction>(getMatch(DWMSymbol::ForceFullRender).Address);
    auto* fullDirtyRendering = static_cast<bool*>(getMatch(DWMSymbol::FullDirtyFlag).Address);
    void*                managerPresent        = nullptr;
    void*                managerHook           = nullptr;
    void*                alwaysNeedsRenderPass = nullptr;
    std::array<void*, 2> presentNeeded{};
    if (fullDirtyRendering)
    {
        presentNeeded = {getMatch(DWMSymbol::LegacyPresentNeeded).Address,
                         getMatch(DWMSymbol::DDisplayPresentNeeded).Address};
        if (!scheduleCompositionPass || !presentNeeded[0] || !presentNeeded[1])
            return false;
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(fullDirtyRendering, &region, sizeof(region)) ||
            region.AllocationBase != GetModuleHandleW(L"dwmcore.dll") || region.State != MEM_COMMIT ||
            (region.Protect & PAGE_GUARD) ||
            !(region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        if (presentNeeded[0] == presentNeeded[1])
            presentNeeded[1] = nullptr;
    }
    else
    {
        const auto& present          = getMatch(DWMSymbol::ManagerPresent);
        const auto& presentWithCount = getMatch(DWMSymbol::ManagerPresentWithCount);
        if (!forceFullRender || (!!present.Address == !!presentWithCount.Address))
            return false;
        managerPresent = present.Address ? present.Address : presentWithCount.Address;
        managerHook    = present.Address ? reinterpret_cast<void*>(HookedManagerPresent)
                                         : reinterpret_cast<void*>(HookedManagerPresentWithCount);
        if (!scheduleCompositionPass)
        {
            alwaysNeedsRenderPass = getMatch(DWMSymbol::AlwaysNeedsRenderPass).Address;
            if (!alwaysNeedsRenderPass)
                return false;
        }
    }

    const HANDLE snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshotHandle == INVALID_HANDLE_VALUE)
        return false;
    Handle              snapshot(snapshotHandle);
    std::vector<Handle> threads;
    THREADENTRY32       entry{sizeof(entry)};
    if (!Thread32First(snapshot.get(), &entry))
        return false;
    const DWORD processID = GetCurrentProcessId();
    const DWORD threadID  = GetCurrentThreadId();
    do
    {
        if (entry.th32OwnerProcessID != processID || entry.th32ThreadID == threadID)
            continue;
        Handle thread(
            OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, entry.th32ThreadID));
        if (!thread)
        {
            if (GetLastError() == ERROR_INVALID_PARAMETER)
                continue;
            return false;
        }
        threads.push_back(std::move(thread));
    } while (Thread32Next(snapshot.get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES)
        return false;

    struct HookEntry
    {
        void*               Target;
        void*               Redirect;
        std::atomic<void*>* Original;
    };
    std::array hooks{
        HookEntry{presentAddress, presentHook, &originalPresent},
        HookEntry{managerPresent, managerHook, &originalManagerPresent},
        HookEntry{alwaysNeedsRenderPass,
                  reinterpret_cast<void*>(HookedAlwaysNeedsRenderPass),
                  &originalAlwaysNeedsRenderPass},
        HookEntry{presentNeeded[0], reinterpret_cast<void*>(HookedPresentNeeded<0>), &originalPresentNeeded[0]},
        HookEntry{presentNeeded[1], reinterpret_cast<void*>(HookedPresentNeeded<1>), &originalPresentNeeded[1]},
    };
    if (DetourTransactionBegin() != NO_ERROR)
        return false;
    for (auto& hook : hooks)
    {
        if (!hook.Target)
            continue;
        PDETOUR_TRAMPOLINE trampoline = nullptr;
        if (DetourAttachEx(&hook.Target, hook.Redirect, &trampoline, nullptr, nullptr) != NO_ERROR)
        {
            DetourTransactionAbort();
            return false;
        }
        hook.Original->store(trampoline, std::memory_order_release);
    }
    for (const auto& thread : threads)
    {
        if (DetourUpdateThread(thread.get()) != NO_ERROR)
        {
            DetourTransactionAbort();
            return false;
        }
    }
    if (DetourTransactionCommit() != NO_ERROR)
        return false;
    if (fullDirtyRendering)
        *fullDirtyRendering = true;
    isDWMDrawingEnabled.store(true, std::memory_order_release);
    RedrawWindow(GetDesktopWindow(), nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    return true;
}

#pragma endregion

namespace
{
#pragma region Helpers

bool FindDWMSymbols(SymbolMatches& matches)
{
    const Memory::SignatureScanner scanner(L"dwmcore.dll");
    if (!scanner.IsValid())
        return false;
    const auto moduleBase = scanner.GetModuleBase();
    for (const auto& signature : GetDWMSignatures())
    {
        std::vector<std::uintptr_t> addresses;
        if (!scanner.ScanAllText(signature.Pattern, addresses))
            return false;
        if (signature.RequiresFunctionEntry)
            std::erase_if(addresses,
                          [moduleBase](std::uintptr_t address)
                          {
                              DWORD64     imageBase     = 0;
                              const auto* functionEntry = RtlLookupFunctionEntry(address, &imageBase, nullptr);
                              return !functionEntry || imageBase != moduleBase ||
                                     imageBase + functionEntry->BeginAddress != address;
                          });
        if (addresses.size() > 1)
            return false;
        if (addresses.empty())
            continue;
        auto address = addresses.front();
        if (signature.Symbol == DWMSymbol::FullDirtyFlag)
        {
            address =
                scanner.ResolveRelativeAddress(address, signature.InstructionEndOffset, signature.DisplacementOffset);
            if (!address)
                return false;
        }
        auto* resolvedAddress = reinterpret_cast<void*>(address);
        auto& match           = matches[static_cast<std::size_t>(signature.Symbol)];
        if (match.Address &&
            (match.Address != resolvedAddress || match.Signature->BufferSlot != signature.BufferSlot ||
             match.Signature->ViewSlot != signature.ViewSlot || match.Signature->IndexSlot != signature.IndexSlot))
            return false;
        match = {resolvedAddress, &signature};
    }
    return true;
}

ID3D11Texture2D* GetBackBuffer(void* swapChain) noexcept
{
    __try
    {
        ID3D11Texture2D* texture = nullptr;
        const auto*      vtable  = *static_cast<void***>(swapChain);
        if (presentSignature->Symbol == DWMSymbol::SwapChainPresent)
        {
            const UINT index =
                presentSignature->IndexSlot
                    ? reinterpret_cast<GetBufferIndexFunction>(vtable[presentSignature->IndexSlot])(swapChain, 0)
                    : 0;
            UINT subresource = 0;
            if (FAILED(reinterpret_cast<GetBufferFunction>(vtable[presentSignature->BufferSlot])(
                    swapChain, index, &subresource, &texture)))
            {
                if (texture)
                    texture->Release();
                return nullptr;
            }
        }
        else
        {
            void* target = reinterpret_cast<GetTargetFunction>(vtable[presentSignature->BufferSlot])(swapChain);
            if (!target)
                return nullptr;
            const auto* targetVtable = *static_cast<void***>(target);
            auto*       view = reinterpret_cast<GetViewFunction>(targetVtable[presentSignature->ViewSlot])(target);
            if (!view)
                return nullptr;
            ID3D11Resource* resource = nullptr;
            view->GetResource(&resource);
            if (!resource)
                return nullptr;
            const HRESULT result = resource->QueryInterface(IID_PPV_ARGS(&texture));
            resource->Release();
            if (FAILED(result))
                return nullptr;
        }
        return texture;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        SetRuntimeStatus(RuntimeStatus::Failed);
        return nullptr;
    }
}

void RenderDWMFrame(void* swapChain) noexcept
{
    if (!swapChain || !isDWMDrawingEnabled.load(std::memory_order_acquire))
        return;
    try
    {
        std::unique_lock lock(frameMutex, std::try_to_lock);
        if (!lock.owns_lock() || !isDWMDrawingEnabled.load(std::memory_order_relaxed))
            return;
        ComPtr<ID3D11Texture2D> texture;
        texture.Attach(GetBackBuffer(swapChain));
        if (texture)
            CompositeFrame(texture.Get());
    }
    catch (...)
    {
        isDWMDrawingEnabled.store(false, std::memory_order_release);
        SetRuntimeStatus(RuntimeStatus::Failed);
    }
}

void RequestFullRender(void* manager)
{
    if (!isDWMDrawingEnabled.load(std::memory_order_acquire))
        return;
    // Request a full redraw after presentation to clear overlay pixels from the previous frame.
    forceFullRender(manager);
    if (scheduleCompositionPass)
        scheduleCompositionPass(0, MAXUINT);
}

HRESULT HookedSwapChainPresent(
    void* swapChain, const void* region, UINT syncInterval, UINT flags, const void* parameters)
{
    RenderDWMFrame(swapChain);
    return reinterpret_cast<SwapChainPresentFunction>(originalPresent.load(std::memory_order_acquire))(
        swapChain, region, syncInterval, flags, parameters);
}

HRESULT HookedOverlayPresent(void* context, void* swapChain, UINT flags, UINT interval, bool disableMPO)
{
    RenderDWMFrame(swapChain);
    return reinterpret_cast<OverlayPresentFunction>(originalPresent.load(std::memory_order_acquire))(
        context, swapChain, flags, interval, disableMPO);
}

HRESULT HookedOverlayRectsPresent(
    void* context, void* swapChain, UINT flags, const void* rects, UINT interval, bool disableMPO)
{
    RenderDWMFrame(swapChain);
    return reinterpret_cast<OverlayRectsPresentFunction>(originalPresent.load(std::memory_order_acquire))(
        context, swapChain, flags, rects, interval, disableMPO);
}

HRESULT HookedOverlayStatusPresent(
    void* context, void* swapChain, UINT flags, const void* rects, UINT interval, bool* status, bool disableMPO)
{
    RenderDWMFrame(swapChain);
    return reinterpret_cast<OverlayStatusPresentFunction>(originalPresent.load(std::memory_order_acquire))(
        context, swapChain, flags, rects, interval, status, disableMPO);
}

HRESULT HookedManagerPresent(void* manager)
{
    const HRESULT result =
        reinterpret_cast<ManagerPresentFunction>(originalManagerPresent.load(std::memory_order_acquire))(manager);
    if (SUCCEEDED(result))
        RequestFullRender(manager);
    return result;
}

HRESULT HookedManagerPresentWithCount(void* manager, UINT interval, UINT* count)
{
    const HRESULT result = reinterpret_cast<ManagerPresentWithCountFunction>(
        originalManagerPresent.load(std::memory_order_acquire))(manager, interval, count);
    if (SUCCEEDED(result))
        RequestFullRender(manager);
    return result;
}

bool HookedAlwaysNeedsRenderPass(void* manager)
{
    const bool isNeeded =
        reinterpret_cast<PresentNeededFunction>(originalAlwaysNeedsRenderPass.load(std::memory_order_acquire))(manager);
    return isDWMDrawingEnabled.load(std::memory_order_acquire) || isNeeded;
}

template <std::size_t index> bool HookedPresentNeeded(void* target)
{
    const bool isNeeded =
        reinterpret_cast<PresentNeededFunction>(originalPresentNeeded[index].load(std::memory_order_acquire))(target);
    if (!isDWMDrawingEnabled.load(std::memory_order_acquire))
        return isNeeded;
    scheduleCompositionPass(0, MAXUINT);
    return true;
}

#pragma endregion
} // namespace
} // namespace DwmOverlay
