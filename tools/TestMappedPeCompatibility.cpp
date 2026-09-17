#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cstring>

// Win32 独立回归：验证执行页恢复、工作线程执行及非代码页隔离。
#define E_PACKAGER_MAPPED_PE_CODE(...) __VA_ARGS__
#include "../src/MappedPeCompatibility.inl"
#undef E_PACKAGER_MAPPED_PE_CODE

int main()
{
    auto* base = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x5000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(base);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 2;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.SizeOfHeaders = 0x1000; nt->OptionalHeader.SizeOfImage = 0x5000;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    sections[0].VirtualAddress = 0x1000; sections[0].Misc.VirtualSize = 0xffffffff;
    sections[0].Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    sections[1].VirtualAddress = 0x3000; sections[1].Misc.VirtualSize = 0x2000;
    sections[1].Characteristics = IMAGE_SCN_MEM_READ;
    EXCEPTION_RECORD record {};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION; record.NumberParameters = 2;
    record.ExceptionInformation[0] = 8;
    EXCEPTION_POINTERS pointers { &record, nullptr };
    auto rejected = [&](unsigned offset) {
        record.ExceptionAddress = base + offset;
        record.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(record.ExceptionAddress);
        assert(mapped_pe_compatibility::FilterExecuteException(&pointers) == EXCEPTION_CONTINUE_SEARCH);
    };
    rejected(0x3000); rejected(0x80); rejected(0x5000);
    record.ExceptionInformation[0] = 0; rejected(0x1000);
    record.ExceptionInformation[0] = 1; rejected(0x1000);
    record.ExceptionInformation[0] = 8;
    dos->e_magic = 0; rejected(0x1000); dos->e_magic = IMAGE_DOS_SIGNATURE;
    nt->FileHeader.NumberOfSections = 65535; rejected(0x1000); nt->FileHeader.NumberOfSections = 2;
    DWORD old = 0;
    VirtualProtect(base + 0x1000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
    rejected(0x1000);
    VirtualProtect(base + 0x1000, 0x1000, PAGE_READWRITE, &old);
    const unsigned char entry[] = { 0xb8, 42, 0, 0, 0, 0xc3 };
    const unsigned char threadEntry[] = { 0xb8, 43, 0, 0, 0, 0xc2, 4, 0 };
    std::memcpy(base + 0x1000, entry, sizeof(entry));
    std::memcpy(base + 0x2000, threadEntry, sizeof(threadEntry));
    VirtualProtect(base + 0x1000, 0x1000, PAGE_READONLY, &old);
    assert(mapped_pe_compatibility::Call(reinterpret_cast<int (__cdecl*)()>(base + 0x1000)) == 42);
    MEMORY_BASIC_INFORMATION region {};
    VirtualQuery(base + 0x1000, &region, sizeof(region)); assert(region.Protect == PAGE_EXECUTE_READ);
    VirtualQuery(base + 0x2000, &region, sizeof(region)); assert(region.Protect == PAGE_EXECUTE_READWRITE);
    VirtualQuery(base + 0x3000, &region, sizeof(region)); assert(region.Protect == PAGE_READWRITE);
    HANDLE thread = CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(base + 0x2000), nullptr, 0, nullptr);
    assert(thread && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    DWORD result = 0; assert(GetExitCodeThread(thread, &result) && result == 43);
    CloseHandle(thread); VirtualFree(base, 0, MEM_RELEASE);
    std::puts("PASS mapped PE entry, worker thread, protection preservation and invalid-fault rejection");
}
