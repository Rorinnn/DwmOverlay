// SignatureScanner: Signature scanning for loaded modules

export module DwmOverlay.Memory.SignatureScanner;

import std;

export namespace DwmOverlay::Memory
{
class SignatureScanner final
{
  public:
    explicit SignatureScanner(const wchar_t* moduleName);

    bool           IsValid() const;
    std::uintptr_t GetModuleBase() const;
    bool           ScanAllText(std::string_view pattern, std::vector<std::uintptr_t>& addresses) const;
    std::uintptr_t ResolveRelativeAddress(std::uintptr_t address,
                                          std::size_t    instructionEndOffset,
                                          std::size_t    displacementOffset) const;

  private:
    std::uintptr_t                moduleBase = 0;
    std::size_t                   moduleSize = 0;
    std::span<const std::uint8_t> text;
};
} // namespace DwmOverlay::Memory
