#include "isaac_sync/runtime_target.hpp"
#include "isaac_sync/runtime_hooks.hpp"
#include "isaac_sync/version.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace isaac_sync::runtime {
namespace {

bool MatchPattern(const std::uint8_t* address, const int* pattern, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        if (pattern[index] >= 0 && address[index] != static_cast<std::uint8_t>(pattern[index]))
            return false;
    }
    return true;
}

bool IsImageRangeValid(const std::uint8_t* main_base, std::uint32_t rva, std::size_t size)
{
    __try
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(main_base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(main_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            return false;
        const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
        return rva <= image_size && size <= image_size - rva;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void AppendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

} // namespace

bool IsTargetImage()
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
    if (!length || length >= _countof(path))
        return false;
    const wchar_t* filename = wcsrchr(path, L'\\');
    filename = filename ? filename + 1 : path;
    return _wcsicmp(filename, L"isaac-ng.exe") == 0;
}

bool ValidateCriticalSites(const std::uint8_t* main_base)
{
    if (!main_base || !IsImageRangeValid(main_base, kFrameCallRva, kFrameCallBytes.size()) ||
        kDispatchCallRva < 4 ||
        !IsImageRangeValid(main_base, kDispatchCallRva - 4, kDispatchFeature.size()) ||
        !IsImageRangeValid(main_base, kLuaRegistrationRva, kLuaRegistrationFeature.size()) ||
        !IsImageRangeValid(main_base, kLuaNewstateCallRva, 6) ||
        !IsImageRangeValid(main_base, kLuaLNewstateCallRva, 6) ||
        !IsImageRangeValid(main_base, kLuaCloseCallRva, 6) ||
        !IsImageRangeValid(main_base, kLuaSimulationPcallCallRva, 6) ||
        !IsImageRangeValid(main_base, kManagerPointerRva, sizeof(void*)) ||
        !IsImageRangeValid(main_base, kSteamNetworkingContextRva, sizeof(void*)) ||
        !IsImageRangeValid(main_base, kNetworkBackendObjectRva, sizeof(void*)))
        return false;
    bool descriptor_matches = false;
    __try
    {
        descriptor_matches = *reinterpret_cast<void* const*>(main_base + kSteamNetworkingContextRva) ==
                             main_base + kSteamNetworkingContextInitRva;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        descriptor_matches = false;
    }
    HMODULE module = reinterpret_cast<HMODULE>(const_cast<std::uint8_t*>(main_base));
    void** const newstate_slot = FindIatSlot(module, "lua_newstate");
    void** const lnewstate_slot = FindIatSlot(module, "luaL_newstate");
    void** const close_slot = FindIatSlot(module, "lua_close");
    void** const pcallk_slot = FindIatSlot(module, "lua_pcallk");
    return descriptor_matches && newstate_slot && lnewstate_slot && close_slot && pcallk_slot &&
           ValidateIatCallsite(main_base + kLuaNewstateCallRva, newstate_slot) &&
           ValidateIatCallsite(main_base + kLuaLNewstateCallRva, lnewstate_slot) &&
           ValidateIatCallsite(main_base + kLuaCloseCallRva, close_slot) &&
           ValidateIatCallsite(main_base + kLuaSimulationPcallCallRva, pcallk_slot) &&
           std::memcmp(main_base + kFrameCallRva, kFrameCallBytes.data(),
                       kFrameCallBytes.size()) == 0 &&
           std::memcmp(main_base + kDispatchCallRva - 4, kDispatchFeature.data(),
                       kDispatchFeature.size()) == 0 &&
           MatchPattern(main_base + kLuaRegistrationRva, kLuaRegistrationFeature.data(),
                        kLuaRegistrationFeature.size());
}

bool ComputeBuildFingerprint(Hash256& output)
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
    if (!length || length >= _countof(path))
        return false;
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input)
        return false;
    std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
    if (image.size() < sizeof(IMAGE_DOS_HEADER))
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > image.size())
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const std::size_t section_offset = reinterpret_cast<const std::uint8_t*>(sections) - image.data();
    if (nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96 ||
        section_offset > image.size() ||
        static_cast<std::size_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) >
            image.size() - section_offset)
        return false;
    const std::uint8_t* text = nullptr;
    std::uint32_t text_size = 0;
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index)
    {
        char name[9]{};
        std::memcpy(name, sections[index].Name, 8);
        if (std::strcmp(name, ".text") != 0)
            continue;
        const std::size_t offset = sections[index].PointerToRawData;
        text_size = sections[index].SizeOfRawData;
        if (offset + text_size > image.size())
            return false;
        text = image.data() + offset;
        break;
    }
    if (!text || !text_size)
        return false;
    std::vector<std::uint8_t> bytes;
    AppendU32(bytes, nt->FileHeader.TimeDateStamp);
    AppendU32(bytes, nt->OptionalHeader.SizeOfImage);
    bytes.insert(bytes.end(), text, text + text_size);
    AppendU16(bytes, kProtocolVersion);
    AppendU16(bytes, static_cast<std::uint16_t>(kIpcAbiVersion));
    bytes.push_back(kCustomChannel);
    AppendU16(bytes, kLeadFrames);
    AppendU16(bytes, kMaxBatchRecords);
    AppendU16(bytes, static_cast<std::uint16_t>(sizeof(CustomFrameOpV2)));
    AppendU32(bytes, kSupportedPayloadKindMask);
    AppendU32(bytes, static_cast<std::uint32_t>(kMaximumWirePacketSize));
    AppendU16(bytes, kMaximumPeerCount);
    AppendU16(bytes, kFullMeshTopologyId);
    AppendU32(bytes, kRequiredCapabilities);
    constexpr char product[] = "IsaacSync-" ISAAC_SYNC_PRODUCT_VERSION;
    bytes.insert(bytes.end(), product, product + sizeof(product) - 1);
    bytes.insert(bytes.end(), kImplementationRevision,
                 kImplementationRevision + sizeof(kImplementationRevision) - 1);
    constexpr std::uint32_t critical_rvas[] = {
        kFrameCallRva, kDispatchCallRva, kLuaRegistrationRva,
        kLuaNewstateCallRva, kLuaLNewstateCallRva, kLuaCloseCallRva,
        kLuaSimulationPcallCallRva, kLuaSimulationPcallReturnRva, kManagerPointerRva,
        kSteamNetworkingContextRva, kSteamNetworkingContextInitRva,
        kNetworkBackendObjectRva, kSteamBackendVtableRva,
    };
    for (const auto rva : critical_rvas)
        AppendU32(bytes, rva);
    return Sha256(bytes.data(), bytes.size(), output);
}

} // namespace isaac_sync::runtime
