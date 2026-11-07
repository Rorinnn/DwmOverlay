// DWMSignatures: DWM function signatures and interface layouts

export module DwmOverlay.Hook.DWMSignatures;

import std;

export namespace DwmOverlay
{
enum class DWMSymbol : std::uint8_t
{
    SwapChainPresent,
    OverlayPresent,
    OverlayRectsPresent,
    OverlayStatusPresent,
    ManagerPresent,
    ManagerPresentWithCount,
    ForceFullRender,
    ScheduleComposition,
    AlwaysNeedsRenderPass,
    LegacyPresentNeeded,
    DDisplayPresentNeeded,
    FullDirtyFlag,
    Count
};

struct DWMSignature
{
    DWMSymbol        Symbol;
    std::string_view Pattern;
    std::uint16_t    BufferSlot            = 0;
    std::uint16_t    ViewSlot              = 0;
    std::uint16_t    IndexSlot             = 0;
    std::uint16_t    DisplacementOffset    = 0;
    std::uint16_t    InstructionEndOffset  = 0;
    bool             RequiresFunctionEntry = true;
};

std::span<const DWMSignature> GetDWMSignatures();
} // namespace DwmOverlay
