#include "isaac_sync/runtime_steam.hpp"

#include <Windows.h>

#include <cwchar>

namespace isaac_sync::runtime {
namespace {

bool IsExecutableAddress(void* address)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    const DWORD protection = info.Protect & 0xFFu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool IsAddressOwnedBySteam(void* address)
{
    HMODULE owner = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCWSTR>(address), &owner))
        return false;
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(owner, path, _countof(path));
    if (!length || length >= _countof(path))
        return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"steamclient.dll") == 0;
}

bool OwnsAllFunctions(const std::array<void*, 3>& functions)
{
    return IsAddressOwnedBySteam(functions[0]) && IsAddressOwnedBySteam(functions[1]) &&
           IsAddressOwnedBySteam(functions[2]);
}

} // namespace

bool ValidateSteamNetworking006(const SteamNetworking006Context& context)
{
    if (!context.interface_pointer || !context.vtable)
        return false;
    __try
    {
        return *reinterpret_cast<void***>(context.interface_pointer) == context.vtable &&
               context.vtable[0] == context.functions[0] &&
               context.vtable[1] == context.functions[1] &&
               context.vtable[2] == context.functions[2] && OwnsAllFunctions(context.functions);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

SteamContextCaptureResult CaptureSteamNetworking006(void* context,
                                                    SteamNetworking006Context& output)
{
    output = {};
    if (!context)
        return SteamContextCaptureResult::Invalid;
    __try
    {
        void* interface_pointer = *reinterpret_cast<void**>(context);
        if (!interface_pointer)
            return SteamContextCaptureResult::Invalid;
        void** vtable = *reinterpret_cast<void***>(interface_pointer);
        if (!vtable || !IsExecutableAddress(vtable[0]) || !IsExecutableAddress(vtable[1]) ||
            !IsExecutableAddress(vtable[2]))
            return SteamContextCaptureResult::Invalid;
        SteamNetworking006Context candidate{
            interface_pointer,
            vtable,
            {vtable[0], vtable[1], vtable[2]},
        };
        if (!OwnsAllFunctions(candidate.functions))
            return SteamContextCaptureResult::OwnerMismatch;
        output = candidate;
        return SteamContextCaptureResult::Captured;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return SteamContextCaptureResult::Invalid;
    }
}

} // namespace isaac_sync::runtime
