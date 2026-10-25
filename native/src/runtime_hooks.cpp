#include "isaac_sync/runtime_hooks.hpp"

#include <TlHelp32.h>

#include <cstring>
#include <limits>
#include <vector>

namespace isaac_sync::runtime {
namespace {

std::vector<HANDLE> SuspendOtherThreads()
{
    std::vector<HANDLE> threads;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return threads;
    THREADENTRY32 entry{sizeof(entry)};
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() ||
                entry.th32ThreadID == GetCurrentThreadId())
                continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            if (SuspendThread(thread) == static_cast<DWORD>(-1))
            {
                CloseHandle(thread);
                continue;
            }
            threads.push_back(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return threads;
}

void ResumeThreads(std::vector<HANDLE>& threads)
{
    for (HANDLE thread : threads)
    {
        ResumeThread(thread);
        CloseHandle(thread);
    }
}

void** FindIatSlotImpl(HMODULE module, const char* import_name)
{
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return nullptr;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        auto* original = descriptor->OriginalFirstThunk
                             ? reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->OriginalFirstThunk)
                             : reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        for (; original->u1.AddressOfData; ++original, ++thunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL32(original->u1.Ordinal))
                continue;
            const auto* entry =
                reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + original->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(entry->Name), import_name) == 0)
                return reinterpret_cast<void**>(&thunk->u1.Function);
        }
    }
    return nullptr;
}

} // namespace

bool InstallRelativeCallPatch(std::uint8_t* call, const std::uint8_t* expected,
                              std::size_t expected_size, void* replacement, CallPatch& patch,
                              void* volatile* published_original)
{
    if (!call || !expected || expected_size != 5 || !replacement || !published_original ||
        std::memcmp(call, expected, expected_size) != 0)
        return false;
    const auto original_displacement = *reinterpret_cast<std::int32_t*>(call + 1);
    const auto next = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(call + 5));
    const auto target = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(replacement));
    const std::int64_t displacement = target - next;
    if (displacement < (std::numeric_limits<std::int32_t>::min)() ||
        displacement > (std::numeric_limits<std::int32_t>::max)())
        return false;
    CallPatch candidate{call, original_displacement, static_cast<std::int32_t>(displacement), false};
    auto threads = SuspendOtherThreads();
    DWORD old_protection = 0;
    const bool writable = VirtualProtect(call + 1, sizeof(std::int32_t), PAGE_EXECUTE_READWRITE,
                                         &old_protection) != FALSE;
    if (writable)
    {
        InterlockedExchangePointer(published_original, call + 5 + original_displacement);
        *reinterpret_cast<std::int32_t*>(call + 1) = candidate.replacement_displacement;
        DWORD ignored = 0;
        VirtualProtect(call + 1, sizeof(std::int32_t), old_protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        candidate.installed = true;
        patch = candidate;
    }
    ResumeThreads(threads);
    return writable;
}

bool RestoreRelativeCallPatch(CallPatch& patch)
{
    if (!patch.installed || !patch.call)
        return true;
    auto threads = SuspendOtherThreads();
    auto* displacement = reinterpret_cast<std::int32_t*>(patch.call + 1);
    if (*displacement != patch.replacement_displacement)
    {
        ResumeThreads(threads);
        return false;
    }
    DWORD old_protection = 0;
    bool restored = false;
    if (VirtualProtect(displacement, sizeof(*displacement), PAGE_EXECUTE_READWRITE, &old_protection))
    {
        *displacement = patch.original_displacement;
        DWORD ignored = 0;
        VirtualProtect(displacement, sizeof(*displacement), old_protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), patch.call, 5);
        restored = *displacement == patch.original_displacement;
    }
    if (restored)
        patch.installed = false;
    ResumeThreads(threads);
    return restored;
}

void** FindIatSlot(HMODULE module, const char* name)
{
    return module && name ? FindIatSlotImpl(module, name) : nullptr;
}

bool ValidateIatCallsite(const std::uint8_t* call, void** expected_slot)
{
    if (!call || !expected_slot)
        return false;
    __try
    {
        return call[0] == 0xFF && call[1] == 0x15 &&
               *reinterpret_cast<void* const*>(call + 2) == expected_slot;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool InstallIatPatch(HMODULE module, const char* name, void* replacement, IatPatch& patch,
                     void* volatile* published_original, void* expected_original)
{
    void** slot = FindIatSlotImpl(module, name);
    if (!slot || !replacement || !published_original)
        return false;
    DWORD old_protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protection))
        return false;
    void* const original = *slot;
    if (!original || original == replacement || (expected_original && original != expected_original))
    {
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), old_protection, &ignored);
        return false;
    }
    patch = {slot, original, replacement, true};
    InterlockedExchangePointer(published_original, original);
    InterlockedExchangePointer(slot, replacement);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old_protection, &ignored);
    return *slot == replacement;
}

bool RestoreIatPatch(IatPatch& patch)
{
    if (!patch.installed || !patch.slot)
        return true;
    if (*patch.slot != patch.replacement)
    {
        return false;
    }
    DWORD old_protection = 0;
    bool restored = false;
    if (VirtualProtect(patch.slot, sizeof(void*), PAGE_READWRITE, &old_protection))
    {
        InterlockedExchangePointer(patch.slot, patch.original);
        DWORD ignored = 0;
        VirtualProtect(patch.slot, sizeof(void*), old_protection, &ignored);
        restored = *patch.slot == patch.original;
    }
    if (restored)
        patch.installed = false;
    return restored;
}

} // namespace isaac_sync::runtime
