// SignatureScanner: Parse byte patterns and scan the code section

module;

#include <Windows.h>
#include <Psapi.h>

module DwmOverlay.Memory.SignatureScanner;

import std;

namespace DwmOverlay::Memory
{
namespace
{
#pragma region Types

struct PatternByte
{
    std::uint8_t Value      = 0;
    bool         IsWildcard = false;
};

#pragma endregion

#pragma region HelperDeclarations

int  ParseHexDigit(char character);
bool ParsePattern(std::string_view pattern, std::vector<PatternByte>& bytes);

#pragma endregion
} // namespace

#pragma region Methods

SignatureScanner::SignatureScanner(const wchar_t* moduleName)
{
    if (!moduleName)
        return;
    const HMODULE module = GetModuleHandleW(moduleName);
    MODULEINFO    information{};
    if (!module || !K32GetModuleInformation(GetCurrentProcess(), module, &information, sizeof(information)))
        return;
    const auto*       imageBase = static_cast<const std::uint8_t*>(information.lpBaseOfDll);
    const std::size_t size      = information.SizeOfImage;
    const auto        hasRange  = [size](std::size_t offset, std::size_t length)
    {
        return offset <= size && length <= size - offset;
    };
    if (!hasRange(0, sizeof(IMAGE_DOS_HEADER)))
        return;
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE ||
        dosHeader->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) ||
        !hasRange(static_cast<std::size_t>(dosHeader->e_lfanew), sizeof(IMAGE_NT_HEADERS64)))
        return;
    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE || ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        ntHeaders->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
        return;
    const std::size_t sectionOffset = static_cast<std::size_t>(dosHeader->e_lfanew) +
                                      FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) +
                                      ntHeaders->FileHeader.SizeOfOptionalHeader;
    if (!hasRange(sectionOffset,
                  static_cast<std::size_t>(ntHeaders->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER)))
        return;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(imageBase + sectionOffset);
    constexpr std::array<BYTE, IMAGE_SIZEOF_SHORT_NAME> TextSectionName{'.', 't', 'e', 'x', 't'};
    for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i)
    {
        const auto& section = sections[i];
        if (std::memcmp(section.Name, TextSectionName.data(), TextSectionName.size()) != 0)
            continue;
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || !section.Misc.VirtualSize ||
            !hasRange(section.VirtualAddress, section.Misc.VirtualSize))
            return;
        const auto start = reinterpret_cast<std::uintptr_t>(imageBase + section.VirtualAddress);
        const auto end   = start + section.Misc.VirtualSize;
        for (auto address = start; address < end;)
        {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) ||
                region.AllocationBase != module || region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD) ||
                !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                return;
            address = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        }
        moduleBase = reinterpret_cast<std::uintptr_t>(imageBase);
        moduleSize = size;
        text       = {imageBase + section.VirtualAddress, section.Misc.VirtualSize};
        return;
    }
}

bool SignatureScanner::IsValid() const
{
    return !text.empty();
}

std::uintptr_t SignatureScanner::GetModuleBase() const
{
    return moduleBase;
}

bool SignatureScanner::ScanAllText(std::string_view pattern, std::vector<std::uintptr_t>& addresses) const
{
    addresses.clear();
    if (!IsValid())
        return false;
    std::vector<PatternByte> bytes;
    if (!ParsePattern(pattern, bytes))
        return false;
    const auto anchor = std::find_if(bytes.begin(),
                                     bytes.end(),
                                     [](const PatternByte& byte)
                                     {
                                         return !byte.IsWildcard;
                                     });
    if (anchor == bytes.end())
        return false;
    if (bytes.size() > text.size())
        return true;
    const std::size_t anchorOffset = static_cast<std::size_t>(anchor - bytes.begin());
    const auto*       cursor       = text.data();
    const auto*       last         = text.data() + text.size() - bytes.size();
    const auto*       searchEnd    = last + anchorOffset + 1;
    while (cursor <= last)
    {
        const auto* found = std::find(cursor + anchorOffset, searchEnd, anchor->Value);
        if (found == searchEnd)
            break;
        const auto* candidate = found - anchorOffset;
        if (std::equal(bytes.begin(),
                       bytes.end(),
                       candidate,
                       [](const PatternByte& byte, std::uint8_t value)
                       {
                           return byte.IsWildcard || byte.Value == value;
                       }))
            addresses.push_back(reinterpret_cast<std::uintptr_t>(candidate));
        cursor = candidate + 1;
    }
    return true;
}

std::uintptr_t SignatureScanner::ResolveRelativeAddress(std::uintptr_t address,
                                                        std::size_t    instructionEndOffset,
                                                        std::size_t    displacementOffset) const
{
    const auto textBase = reinterpret_cast<std::uintptr_t>(text.data());
    if (!IsValid() || address < textBase || address - textBase >= text.size())
        return 0;
    const std::size_t remaining = text.size() - (address - textBase);
    if (instructionEndOffset > remaining || displacementOffset > instructionEndOffset ||
        instructionEndOffset - displacementOffset < sizeof(std::int32_t))
        return 0;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(address + displacementOffset), sizeof(displacement));
    const auto target = static_cast<std::intptr_t>(address + instructionEndOffset) + displacement;
    if (target < static_cast<std::intptr_t>(moduleBase) ||
        static_cast<std::uintptr_t>(target) - moduleBase >= moduleSize)
        return 0;
    return static_cast<std::uintptr_t>(target);
}

#pragma endregion

namespace
{
#pragma region Helpers

int ParseHexDigit(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

bool ParsePattern(std::string_view pattern, std::vector<PatternByte>& bytes)
{
    std::size_t position = 0;
    while (position < pattern.size())
    {
        if (std::isspace(static_cast<unsigned char>(pattern[position])))
        {
            ++position;
            continue;
        }
        if (pattern.size() - position < 2)
            return false;
        const auto token = pattern.substr(position, 2);
        if (token == "??")
            bytes.push_back({.IsWildcard = true});
        else
        {
            const int high = ParseHexDigit(token[0]);
            const int low  = ParseHexDigit(token[1]);
            if (high < 0 || low < 0)
                return false;
            bytes.push_back({static_cast<std::uint8_t>((high << 4) | low), false});
        }
        position += 2;
        if (position < pattern.size() && !std::isspace(static_cast<unsigned char>(pattern[position])))
            return false;
    }
    return !bytes.empty();
}

#pragma endregion
} // namespace
} // namespace DwmOverlay::Memory
