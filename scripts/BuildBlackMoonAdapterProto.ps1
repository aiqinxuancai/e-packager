param(
    [string]$SourceRoot = 'D:\git\BlackMoonKernelStaticLib\krnln',
    [string]$OutputRoot = 'D:\git\e-packager\temp\bm-adapt-proto',
    [string]$ModernCoreRoot = 'D:\git\ycIDE-electron',
    [string]$EligibleSourcesPath = '',
    [string]$TargetSymbolMapPath = ''
)

$ErrorActionPreference = 'Stop'
$encoding936 = [Text.Encoding]::GetEncoding(936)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
Copy-Item -Path (Join-Path $SourceRoot '*') -Destination $OutputRoot -Recurse -Force

$stdAfxPath = Join-Path $OutputRoot 'StdAfx.h'
$stdAfx = $encoding936.GetString([IO.File]::ReadAllBytes($stdAfxPath))
$stdAfx = $stdAfx.Replace(
    '#define LIBAPI(rType, fnName) extern "C" rType _cdecl fnName(INT nArgCount,MDATA_INF ArgInf,...)',
    '#define LIBAPI(rType, fnName) extern "C" rType _cdecl fnName(INT nArgCount,MDATA_INF& ArgInf,...)')
$stdAfx = $stdAfx.Replace(
    'krnln_BJCase (INT nArgCount,MDATA_INF ArgInf,...)',
    'krnln_BJCase (INT nArgCount,MDATA_INF& ArgInf,...)')
$stdAfx = $stdAfx.Replace(
    'krnln_LTrim (INT nArgCount,MDATA_INF ArgInf,...)',
    'krnln_LTrim (INT nArgCount,MDATA_INF& ArgInf,...)')
$stdAfx = $stdAfx.Replace(
    '#include "lib2.h"',
    "#include `"lib2.h`"`r`n#include `"bm_runtime.h`"")
[IO.File]::WriteAllBytes($stdAfxPath, $encoding936.GetBytes($stdAfx))

# Keep all legacy helper names private to the adapter archive.  The generated
# e-packager host provides its own symbols with the public names below.
$runtimeHeader = @'
#pragma once
#include <cstring>
#include <vector>
#define E_MAlloc bm_E_MAlloc
#define E_MAlloc_Nzero bm_E_MAlloc_Nzero
#define E_MRealloc bm_E_MRealloc
#define E_MFree bm_E_MFree
#define E_ReportError bm_E_ReportError
#define E_End bm_E_End
#define E_NULLARRAY bm_E_NULLARRAY
#define CloneBinData bm_CloneBinData
#define CloneTextData bm_CloneTextData
#define GetAryElementInf bm_GetAryElementInf
#define FreeAryElement bm_FreeAryElement
#define GetDataTypeType bm_GetDataTypeType
#define GetSysDataTypeDataSize bm_GetSysDataTypeDataSize
#define mystrlen bm_mystrlen
#define LTrimZeroChr bm_LTrimZeroChr

// Old x86 implementations returned scalar values through EAX/EDX.  Keep
// this conversion local to the adapter and preserve the raw register bits,
// so a float result is not accidentally converted to an integer value.
inline void bm_copy_register_pair(MDATA_INF& destination, const MDATA_INF& source) {
    std::memcpy(&destination.m_unit, &source.m_unit, sizeof(destination.m_unit));
}

template<class T>
inline void bm_copy_register_word(MDATA_INF& destination, const T& source) {
    static_assert(sizeof(T) <= sizeof(DWORD));
    std::memset(&destination.m_unit, 0, sizeof(destination.m_unit));
    std::memcpy(&destination.m_unit, &source, sizeof(T));
}

inline void bm_copy_dynamic_return(MDATA_INF& destination, const MDATA_INF& source) {
    destination = source;
}

INT bm_register_opaque_handle(void* value);
void* bm_resolve_opaque_handle(INT token);
void bm_release_opaque_handle(INT token);

class bm_pointer_array {
public:
    void Add(void* value) { values_.push_back(value); }
    INT Count() const { return static_cast<INT>(values_.size()); }
    const void* GetPtr() const { return values_.empty() ? nullptr : values_.data(); }
private:
    std::vector<void*> values_;
};
'@
[IO.File]::WriteAllText((Join-Path $OutputRoot 'bm_runtime.h'), $runtimeHeader, [Text.Encoding]::ASCII)

# The original headers use 32-bit fields for the subroutine address.  The
# x64 adapter keeps the wire record size at 12 bytes but uses pointer-width
# storage where the field is actually an address.
foreach ($headerName in @('lib.h', 'lib2.h')) {
    $headerPath = Join-Path $OutputRoot $headerName
    if (-not (Test-Path -Path $headerPath)) { continue }
    $header = $encoding936.GetString([IO.File]::ReadAllBytes($headerPath))
    $header = $header.Replace('DWORD         m_dwSubCodeAdr', 'ULONG_PTR     m_dwSubCodeAdr')
    $header = $header.Replace('DWORD  m_dwSubCodeAdr', 'ULONG_PTR  m_dwSubCodeAdr')
    $header = $header.Replace('DWORD*  m_pdwSubCodeAdr', 'ULONG_PTR*  m_pdwSubCodeAdr')
    $header = $header.Replace('DWORD*            m_pdwSubCodeAdr', 'ULONG_PTR*            m_pdwSubCodeAdr')
    [IO.File]::WriteAllBytes($headerPath, $encoding936.GetBytes($header))
}

$pinyinObject = Join-Path $SourceRoot 'PY.OBJ'
$pinyinGenerator = Join-Path $PSScriptRoot 'BuildBlackMoonPinyinSource.ps1'
if (-not (Test-Path -LiteralPath $pinyinGenerator -PathType Leaf)) {
    throw "未找到黑月拼音数据生成器: $pinyinGenerator"
}
& $pinyinGenerator -ObjectPath $pinyinObject -OutputPath (Join-Path $OutputRoot 'krnln_Pinyin.cpp') | Out-Null
if (-not $?) {
    throw '生成 BlackMoon 拼音 x64 源码失败。'
}

function Find-CppFunctionBodyEnd {
    param(
        [string]$Text,
        [int]$StartOffset
    )

    $state = 'code'
    $braceDepth = 0
    $bodyStarted = $false
    for ($index = $StartOffset; $index -lt $Text.Length; ++$index) {
        $current = $Text[$index]
        $next = if ($index + 1 -lt $Text.Length) { $Text[$index + 1] } else { [char]0 }
        if ($state -eq 'line-comment') {
            if ($current -eq "`n") { $state = 'code' }
            continue
        }
        if ($state -eq 'block-comment') {
            if ($current -eq '*' -and $next -eq '/') { $state = 'code'; ++$index }
            continue
        }
        if ($state -eq 'string') {
            if ($current -eq [char]92) { ++$index; continue }
            if ($current -eq '"') { $state = 'code' }
            continue
        }
        if ($state -eq 'character') {
            if ($current -eq [char]92) { ++$index; continue }
            if ($current -eq [char]39) { $state = 'code' }
            continue
        }
        if ($current -eq '/' -and $next -eq '/') { $state = 'line-comment'; ++$index; continue }
        if ($current -eq '/' -and $next -eq '*') { $state = 'block-comment'; ++$index; continue }
        if ($current -eq '"') { $state = 'string'; continue }
        if ($current -eq [char]39) { $state = 'character'; continue }
        if ($current -eq '{') {
            $bodyStarted = $true
            ++$braceDepth
            continue
        }
        if ($current -eq '}' -and $bodyStarted) {
            --$braceDepth
            if ($braceDepth -eq 0) { return $index + 1 }
        }
    }
    throw "无法定位 C++ 函数结束位置: offset=$StartOffset"
}

function Remove-CppCommentsAndStrings {
    param([string]$Text)
    return [regex]::Replace($Text, '(?s)/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*''', '')
}

function Convert-LegacyPointerArrays {
    param([string]$Text)

    # CMyDWordArray was also used as a pointer vector in several x86 command
    # implementations. Detect that use from an explicit DWORD pointer cast,
    # then change only that local container to a native-width vector.
    $pointerContainerNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($match in [regex]::Matches($Text, '\bCMyDWordArray\s+(?<name>[A-Za-z_]\w*)\s*;')) {
        $name = $match.Groups['name'].Value
        $addPattern = '\b' + [regex]::Escape($name) + '\s*\.\s*Add\s*\(\s*\(\s*DWORD\s*\)'
        if ([regex]::IsMatch($Text, $addPattern)) {
            [void]$pointerContainerNames.Add($name)
        }
    }
    foreach ($name in $pointerContainerNames) {
        $escapedName = [regex]::Escape($name)
        $Text = [regex]::Replace($Text, '\bCMyDWordArray\s+' + $escapedName + '\b', 'bm_pointer_array ' + $name)
        $Text = [regex]::Replace($Text, '(\b' + $escapedName + '\s*\.\s*Add\s*\(\s*)\(\s*DWORD\s*\)\s*', '$1')
        $Text = [regex]::Replace($Text, '\b' + $escapedName + '\s*\.\s*GetDWordCount\s*\(\s*\)', $name + '.Count()')
        $Text = [regex]::Replace($Text, '(\b' + $escapedName + '\s*\.\s*Count\s*\(\s*\)\s*\*\s*)sizeof\s*\(\s*DWORD\s*\)', '$1sizeof(void*)')
        $Text = [regex]::Replace($Text, '(sizeof\s*\(\s*DWORD\s*\)\s*\*\s*\b' + $escapedName + '\s*\.\s*Count\s*\(\s*\))', 'sizeof(void*) * ' + $name + '.Count()')
    }

    # Array-returning legacy code commonly writes pointers through LPINT.
    # The assignment's explicit INT cast proves that this is a pointer payload,
    # so replace its storage slots and matching allocation-size expressions.
    $pointerSlotFound = $false
    foreach ($match in [regex]::Matches($Text, '(?m)LPINT\s+(?<slot>[A-Za-z_]\w*)\s*=\s*\(LPINT\)\s*\(\s*(?<base>[^;\r\n]+?)\s*\)\s*;')) {
        $slot = $match.Groups['slot'].Value
        $storePattern = '\*\s*' + [regex]::Escape($slot) + '\s*=\s*\(\s*INT\s*\)\s*'
        if (-not [regex]::IsMatch($Text, $storePattern)) { continue }
        $declarationPattern = '(?m)LPINT\s+' + [regex]::Escape($slot) + '\s*=\s*\(LPINT\)\s*\(\s*' + [regex]::Escape($match.Groups['base'].Value.Trim()) + '\s*\)\s*;'
        $Text = [regex]::Replace($Text, $declarationPattern, ('void** ' + $slot + ' = reinterpret_cast<void**>(' + $match.Groups['base'].Value.Trim() + ');'))
        $Text = [regex]::Replace($Text, $storePattern, ('*' + $slot + ' = '))
        $pointerSlotFound = $true
    }
    if ($pointerSlotFound) {
        $Text = [regex]::Replace($Text, 'sizeof\s*\(\s*DWORD\s*\)', 'sizeof(void*)')
    }
    return $Text
}

foreach ($source in Get-ChildItem -LiteralPath $OutputRoot -Filter '*.cpp') {
    $text = $encoding936.GetString([IO.File]::ReadAllBytes($source.FullName))
    $text = [regex]::Replace($text, 'MDATA_INF\s+ArgInf\s*(?=,|\))', 'MDATA_INF& ArgInf')
    # Translate only the two verified x86 return conventions.  All other
    # inline assembly remains in place and is rejected by the x64 probe;
    # this avoids silently deleting algorithmic assembly from a legacy file.
    $text = [regex]::Replace(
        $text,
        '(?is)(?:__asm|_asm)\s*\{\s*mov\s+eax\s*,\s*(?<source>[A-Za-z_]\w*)\.m_unit\.m_dwFormID\s*;?\s*mov\s+edx\s*,\s*\k<source>\.m_unit\.m_dwUnitID\s*;?\s*mov\s+ecx\s*,\s*\k<source>\.m_dtDataType\s*;?\s*\}',
        { param($match) "bm_copy_dynamic_return(ArgInf, $($match.Groups['source'].Value));" })
    $text = [regex]::Replace(
        $text,
        '(?is)(?:__asm|_asm)\s*\{\s*mov\s+eax\s*,\s*(?<source>[A-Za-z_]\w*)\.m_unit\.m_dwFormID\s*;?\s*mov\s+edx\s*,\s*\k<source>\.m_unit\.m_dwUnitID\s*;?\s*\}',
        { param($match) "bm_copy_register_pair(ArgInf, $($match.Groups['source'].Value));" })
    $text = [regex]::Replace(
        $text,
        '(?im)^\s*(?:__asm|_asm)\s+mov\s+eax\s*,\s*(?<source>[A-Za-z_]\w*)\s*;?\s*\r?\n\s*(?:__asm|_asm)\s+xor\s+edx\s*,\s*edx\s*;?',
        { param($match) "bm_copy_register_word(ArgInf, $($match.Groups['source'].Value));" })
    $text = [regex]::Replace(
        $text,
        '(?is)(?:__asm|_asm)\s*\{\s*(?:(?:mov|xor)\s+(?:eax|edx|ecx)\s*,\s*[^\r\n{};]+\s*;?\s*)+\}',
        '')
    $text = Convert-LegacyPointerArrays -Text $text
    # SAFEARRAY stores BSTR and interface references as pointers. The legacy
    # source used DWORD because it was built only for x86; use native-width
    # elements whenever the VARTYPE itself identifies a pointer payload.
    $text = [regex]::Replace(
        $text,
        '(?is)(nVtype\s*=\s*VT_(?:BSTR|DISPATCH)\s*;\s*nDataSize\s*=\s*)sizeof\s*\(\s*DWORD\s*\)',
        '$1sizeof(void*)')
    [IO.File]::WriteAllBytes($source.FullName, $encoding936.GetBytes($text))
}

# Decompiler-only krnln_fn* helpers sometimes preserve a 32-bit stack layout
# but are not referenced by any command or helper source. Remove only helpers
# with a single code occurrence across the generated source tree; referenced
# helpers remain intact and must pass the normal x64 probe.
$allGeneratedSourceCode = (Get-ChildItem -LiteralPath $OutputRoot -Filter '*.cpp' | ForEach-Object {
    Remove-CppCommentsAndStrings -Text ($encoding936.GetString([IO.File]::ReadAllBytes($_.FullName)))
}) -join "`n"
foreach ($source in Get-ChildItem -LiteralPath $OutputRoot -Filter '*.cpp') {
    $text = $encoding936.GetString([IO.File]::ReadAllBytes($source.FullName))
    $matches = [regex]::Matches(
        $text,
        '(?m)^[ \t]*(?:[A-Za-z_][A-Za-z0-9_:<>, \t\*&]+)\s+(?<name>krnln_fn[A-Za-z0-9_]+)\s*\([^;{}]*\)\s*\{')
    for ($matchIndex = $matches.Count - 1; $matchIndex -ge 0; --$matchIndex) {
        $match = $matches[$matchIndex]
        $name = $match.Groups['name'].Value
        if ([regex]::Matches($allGeneratedSourceCode, '\b' + [regex]::Escape($name) + '\b').Count -ne 1) { continue }
        $endOffset = Find-CppFunctionBodyEnd -Text $text -StartOffset $match.Index
        $text = $text.Remove($match.Index, $endOffset - $match.Index).
            Insert($match.Index, "`r`n/* Unreferenced legacy helper omitted by x64 adapter. */`r`n")
    }
    [IO.File]::WriteAllBytes($source.FullName, $encoding936.GetBytes($text))
}

# Build a source list after transformation.  These files contain algorithmic
# x86 assembly or the old executable/window entry point and are not part of a
# callable x64 support-library archive.
$excluded = @(
    'BlackMoonCallPropertyVaule.cpp', 'BlackMoonExe.cpp', 'DllEntryFunc.cpp',
    'BlackMoonDll.cpp', 'BlackMoonDll2.cpp', 'BlackMoonResDll.cpp',
    'BlackMoonCallUserDll.cpp', 'BlackMoonLibNotifySys.cpp', 'eHelpFunc.cpp',
    'EyInit.cpp', 'EyComInit.cpp', 'md5t.cpp', 'mem.cpp', 'Myfunctions.cpp',
    'krnln_GetAllPY.cpp', 'krnln_InputBox.cpp', 'krnln_Dispatch.cpp'
)
$runtimeOwnedHelperSources = @(
    'CloneBinData.cpp', 'CloneTextData.cpp', 'FreeAryElement.cpp',
    'GetAryElementInf.cpp', 'GetDataTypeType.cpp',
    'GetSysDataTypeDataSize.cpp', 'LTrimZeroChr.cpp'
)
$generatedAdapterSources = @(
    'bm_runtime.cpp', 'bm_wrappers.cpp', 'bm_com_lifecycle.cpp', 'fallback_cmd_impl.cpp'
)
$allSourceFiles = @(Get-ChildItem -LiteralPath $OutputRoot -Filter '*.cpp')
$commandSources = @($allSourceFiles |
    Where-Object { $_.Name -like 'krnln_*.cpp' } |
    Where-Object { $excluded -notcontains $_.Name } |
    ForEach-Object { $_.Name })
$helperSources = @($allSourceFiles |
    Where-Object { $_.Name -notlike 'krnln_*.cpp' } |
    Where-Object { $_.Name -ne 'StdAfx.cpp' } |
    Where-Object { $generatedAdapterSources -notcontains $_.Name } |
    Where-Object { $excluded -notcontains $_.Name } |
    Where-Object { $runtimeOwnedHelperSources -notcontains $_.Name } |
    ForEach-Object { $_.Name })

# Generate a small private runtime for the legacy implementations.
$runtimeSource = @'
#include "stdafx.h"
#include "bm_runtime.h"
#include "Myfunctions.h"
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <windows.h>

extern "C" void* bm_E_MAlloc(DWORD size) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1); }
extern "C" void* bm_E_MAlloc_Nzero(DWORD size) { return HeapAlloc(GetProcessHeap(), 0, size ? size : 1); }
extern "C" void* bm_E_MRealloc(void* value, DWORD size) { return value ? HeapReAlloc(GetProcessHeap(), 0, value, size ? size : 1) : bm_E_MAlloc(size); }
extern "C" void bm_E_MFree(void* value) { if (value) HeapFree(GetProcessHeap(), 0, value); }
extern "C" void bm_E_ReportError(DWORD, DWORD, DWORD) {}
extern "C" void bm_E_End(DWORD code) { ExitProcess(code); }
extern "C" void* bm_E_NULLARRAY() { return nullptr; }
namespace {
std::mutex g_bmOpaqueHandleMutex;
std::unordered_map<INT, void*> g_bmOpaqueHandles;
std::uint32_t g_bmNextOpaqueHandle = 0x40000000u;
}
INT bm_register_opaque_handle(void* value) {
    if (!value) return 0;
    std::lock_guard<std::mutex> lock(g_bmOpaqueHandleMutex);
    for (std::size_t attempt = 0; attempt < 0x3fffffffu; ++attempt) {
        const INT token = static_cast<INT>(g_bmNextOpaqueHandle++);
        if (g_bmNextOpaqueHandle >= 0x80000000u) g_bmNextOpaqueHandle = 0x40000000u;
        if (token != 0 && !g_bmOpaqueHandles.contains(token)) {
            g_bmOpaqueHandles.emplace(token, value);
            return token;
        }
    }
    return 0;
}
void* bm_resolve_opaque_handle(INT token) {
    std::lock_guard<std::mutex> lock(g_bmOpaqueHandleMutex);
    const auto found = g_bmOpaqueHandles.find(token);
    return found == g_bmOpaqueHandles.end() ? nullptr : found->second;
}
void bm_release_opaque_handle(INT token) {
    if (token == 0) return;
    std::lock_guard<std::mutex> lock(g_bmOpaqueHandleMutex);
    g_bmOpaqueHandles.erase(token);
}
static size_t bm_mystrlen_impl(const char* text) { return text ? std::strlen(text) : 0; }
MYSTRLEN bm_mystrlen = bm_mystrlen_impl;
void bm_LTrimZeroChr(char*) {}
char* bm_CloneTextData(char* text, int length) {
    if (!text) return nullptr; if (length <= 0) length = bm_mystrlen(text); if (length <= 0) return nullptr;
    char* result = static_cast<char*>(bm_E_MAlloc_Nzero(static_cast<unsigned int>(length + 1)));
    std::memcpy(result, text, length); result[length] = 0; return result;
}
char* bm_CloneTextData(char* text) { return bm_CloneTextData(text, 0); }
LPBYTE bm_CloneBinData(LPBYTE data, INT length) {
    if (!data || length <= 0) return nullptr;
    auto* result = static_cast<unsigned char*>(bm_E_MAlloc(static_cast<DWORD>(sizeof(int) * 2 + length)));
    reinterpret_cast<int*>(result)[0] = 1; reinterpret_cast<int*>(result)[1] = length;
    std::memcpy(result + sizeof(int) * 2, data, length); return result;
}
void* bm_GetAryElementInf(void* value, DWORD& count) {
    count = 0; if (!value) return nullptr; auto* p = static_cast<int*>(value); int dimensions = *p++;
    int total = 1; for (int i = 0; i < dimensions; ++i) total *= *p++;
    count = total > 0 ? static_cast<DWORD>(total) : 0; return p;
}
LPBYTE bm_GetAryElementInf(void* value, LPINT count) {
    DWORD n = 0; auto* result = static_cast<LPBYTE>(bm_GetAryElementInf(value, n));
    if (count) *count = static_cast<INT>(n); return result;
}
void bm_FreeAryElement(void* value) {
    if (!value) return; auto* p = static_cast<int*>(value); int dimensions = *p++; int total = 1;
    for (int i = 0; i < dimensions; ++i) total *= *p++;
    if (total > 0) HeapFree(GetProcessHeap(), 0, value);
}
INT bm_GetDataTypeType(DATA_TYPE type) {
    if (type == 0) return 0; return (type & 0xC0000000u) == 0x40000000u ? 2 : ((type & 0xC0000000u) == 0x80000000u ? 1 : 3);
}
INT bm_GetSysDataTypeDataSize(DATA_TYPE type) {
    switch (type) { case SDT_BYTE: return 1; case SDT_SHORT: return 2; case SDT_INT: case SDT_BOOL: return 4;
    case SDT_INT64: case SDT_DOUBLE: case SDT_DATE_TIME: return 8; case SDT_FLOAT: return 4; case SDT_SUB_PTR: return static_cast<int>(sizeof(void*)); default: return 0; }
}
INT FindByte(LPBYTE data, INT count, BYTE value) {
    if (!data || count <= 0) return -1;
    for (INT index = 0; index < count; ++index) {
        if (data[index] == value) return index;
    }
    return -1;
}

// Shared helpers from the historical core were implemented with x86 inline
// assembly. These C++ implementations preserve their public helper ABI while
// keeping pointer operations native-width on x64.
unsigned char lowtable[256];
unsigned char uptable[256];
namespace {
struct BmCharacterTables {
    BmCharacterTables() {
        for (int index = 0; index < 256; ++index) {
            lowtable[index] = static_cast<unsigned char>(index);
            uptable[index] = static_cast<unsigned char>(index);
        }
        for (int index = 'A'; index <= 'Z'; ++index) lowtable[index] = static_cast<unsigned char>(index - 'A' + 'a');
        for (int index = 'a'; index <= 'z'; ++index) uptable[index] = static_cast<unsigned char>(index - 'a' + 'A');
    }
} g_bmCharacterTables;
}

int __fastcall mymemchr(unsigned char* source, int length, unsigned char value) {
    if (!source || length <= 0) return -1;
    for (int index = 0; index < length; ++index) if (source[index] == value) return index;
    return -1;
}
int boyer_moore(unsigned char* source, int sourceLength, unsigned char* pattern, int patternLength) {
    if (!source || !pattern || patternLength <= 0 || patternLength > sourceLength) return -1;
    for (int offset = 0; offset <= sourceLength - patternLength; ++offset) {
        if (std::memcmp(source + offset, pattern, static_cast<size_t>(patternLength)) == 0) return offset;
    }
    return -1;
}
int __fastcall myinstring(unsigned char* source, int sourceLength, unsigned char* pattern, int patternLength) {
    return boyer_moore(source, sourceLength, pattern, patternLength);
}
INT __fastcall mystristr(char* text, char* pattern) {
    if (!text || !pattern) return -1;
    if (!*pattern) return 0;
    for (char* start = text; *start; ++start) {
        char* left = start; char* right = pattern;
        while (*left && *right && lowtable[static_cast<unsigned char>(*left)] == lowtable[static_cast<unsigned char>(*right)]) { ++left; ++right; }
        if (!*right) return static_cast<INT>(start - text);
    }
    return -1;
}
INT __fastcall mystrstr(char* text, char* pattern) {
    if (!text || !pattern) return -1;
    if (!*pattern) return 0;
    const char* found = std::strstr(text, pattern);
    return found ? static_cast<INT>(found - text) : -1;
}
void swap_hex(unsigned char* value, int length) {
    if (!value || length <= 1) return;
    for (int left = 0, right = length - 1; left < right; ++left, --right) {
        const unsigned char saved = value[left]; value[left] = value[right]; value[right] = saved;
    }
}
void E_RC4_init(unsigned char* table, unsigned char* key, int keyLength) {
    if (!table || !key || keyLength <= 0) return;
    unsigned char keyBytes[256]{};
    for (int index = 0; index < 256; ++index) { table[index] = static_cast<unsigned char>(index); keyBytes[index] = key[index % keyLength]; }
    int cursor = 0;
    for (int index = 0; index < 256; ++index) {
        cursor = (cursor + table[index] + keyBytes[index]) & 0xff;
        const unsigned char saved = table[index]; table[index] = table[cursor]; table[cursor] = saved;
    }
    table[256] = 0; table[257] = 0;
}
void E_RC4_updatetable(int length, unsigned char* table) {
    if (!table || length <= 0) return;
    unsigned char left = table[256], right = table[257];
    for (int index = 0; index < length; ++index) {
        left = static_cast<unsigned char>(left + 1); right = static_cast<unsigned char>(right + table[left]);
        const unsigned char saved = table[left]; table[left] = table[right]; table[right] = saved;
    }
    table[256] = left; table[257] = right;
}
void E_RC4(unsigned char* data, int length, unsigned char* table) {
    if (!data || !table || length <= 0) return;
    unsigned char left = table[256], right = table[257];
    for (int index = 0; index < length; ++index) {
        left = static_cast<unsigned char>(left + 1); right = static_cast<unsigned char>(right + table[left]);
        const unsigned char saved = table[left]; table[left] = table[right]; table[right] = saved;
        data[index] ^= table[static_cast<unsigned char>(table[left] + table[right])];
    }
    table[256] = left; table[257] = right;
}
BOOL E_RC4_Calc(int position, unsigned char* data, int length, unsigned char* table, int cryptStart, unsigned char* digest) {
    if (!data || length <= 0 || !table || !digest) return FALSE;
    if (position < cryptStart) {
        const int skip = cryptStart - position;
        if (skip >= length) return FALSE;
        data += skip; length -= skip; position = cryptStart;
    }
    unsigned char initial[258]{}; std::memcpy(initial, table, sizeof(initial));
    constexpr int kChunk = 4096;
    E_RC4_updatetable(4 * (position / kChunk), initial);
    int chunkIndex = position / kChunk;
    int offset = position % kChunk;
    while (length > 0) {
        unsigned char seed[40]{};
        const int random = static_cast<int>(initial[0]) | (static_cast<int>(initial[1]) << 8) |
            (static_cast<int>(initial[2]) << 16) | (static_cast<int>(initial[3]) << 24);
        const int mixed = chunkIndex ^ random;
        std::memcpy(seed, &random, sizeof(random)); std::memcpy(seed + 4, digest, 32); std::memcpy(seed + 36, &mixed, sizeof(mixed));
        E_RC4(initial, 4, initial);
        unsigned char stream[258]{}; E_RC4_init(stream, seed, sizeof(seed)); E_RC4_updatetable(offset + 36, stream);
        const int current = (length < kChunk - offset) ? length : kChunk - offset;
        E_RC4(data, current, stream);
        data += current; length -= current; ++chunkIndex; offset = 0;
    }
    return TRUE;
}

TBR::TBR() : m_nCount(0), m_nTCount(0), m_nTLen(0), m_data(nullptr) {}
TBR::~TBR() { std::free(m_data); }
void TBR::add(PVOID address, size_t length) {
    if (!address || length == 0) return;
    if (m_nCount == m_nTCount) {
        const int next = m_nTCount == 0 ? 64 : m_nTCount * 2;
        void* allocated = std::realloc(m_data, static_cast<size_t>(next) * sizeof(TBRECORD));
        if (!allocated) return;
        m_data = static_cast<PTBRECORD>(allocated); m_nTCount = next;
    }
    m_data[m_nCount++] = { address, static_cast<DWORD>(length) }; m_nTLen += length;
}
char* TBR::toString() {
    if (!m_data || m_nCount <= 0 || m_nTLen == 0) return nullptr;
    char* result = static_cast<char*>(E_MAlloc_Nzero(static_cast<DWORD>(m_nTLen + 1))); if (!result) return nullptr;
    char* cursor = result;
    for (int index = 0; index < m_nCount; ++index) { std::memcpy(cursor, m_data[index].addr, m_data[index].len); cursor += m_data[index].len; }
    result[m_nTLen] = 0; return result;
}

extern "C" PDESTROY HFileDestroyAddress = nullptr;
'@
[IO.File]::WriteAllText((Join-Path $OutputRoot 'bm_runtime.cpp'), $runtimeSource, [Text.Encoding]::UTF8)

# Read legacy return types and modern command indices from source metadata.
$legacyFunctions = @{}
$legacyFunctionSources = @{}
$legacyFunctionBodies = @{}
$legacyFunctionChineseNames = @{}
$legacyFunctionAbiNames = @{}

function Register-LegacyFunction {
    param(
        [string]$Name,
        [string]$ReturnType,
        [string]$SourceName,
        [string]$SourceText,
        [int]$DefinitionOffset
    )

    if ($legacyFunctions.ContainsKey($Name)) { return }
    $legacyFunctions[$Name] = $ReturnType.Trim()
    $legacyFunctionSources[$Name] = $SourceName
    $legacyFunctionBodies[$Name] = $SourceText

    $prefix = $SourceText.Substring(0, [Math]::Min($DefinitionOffset, $SourceText.Length))
    $comments = [regex]::Matches($prefix, '(?s)/\*.*?\*/')
    if ($comments.Count -eq 0) { return }
    $metadata = $comments[$comments.Count - 1].Value
    $englishName = [regex]::Match($metadata, '英文名称\s*[:：]\s*(?<name>[A-Za-z_][A-Za-z0-9_]*)')
    if ($englishName.Success) {
        $legacyFunctionAbiNames[$Name] = $englishName.Groups['name'].Value
    }
    $callFormat = [regex]::Match(
        $metadata,
        '调用格式\s*[:：]\s*〈[^〉]+〉\s*(?<name>[^（\(\r\n]+)')
    if ($callFormat.Success) {
        $legacyFunctionChineseNames[$Name] = $callFormat.Groups['name'].Value.Trim()
    }
}

foreach ($sourceName in $commandSources) {
    $sourceText = $encoding936.GetString([IO.File]::ReadAllBytes((Join-Path $OutputRoot $sourceName)))
    foreach ($match in [regex]::Matches($sourceText, 'LIBAPI\s*\(\s*([^,]+?)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)')) {
        Register-LegacyFunction -Name $match.Groups[2].Value -ReturnType $match.Groups[1].Value -SourceName $sourceName -SourceText $sourceText -DefinitionOffset $match.Index
    }
    # Some BlackMoon sources export commands as explicit C ABI functions
    # instead of LIBAPI. Only functions with a preceding public metadata
    # comment are registered, which keeps private helpers out of the ABI map.
    foreach ($match in [regex]::Matches(
        $sourceText,
        '(?m)^[ \t]*(?<type>[A-Za-z_][A-Za-z0-9_]*(?:[ \t]*\*)?)\s+_cdecl\s+(?<name>krnln_[A-Za-z0-9_]+)\s*\(\s*INT\s+[A-Za-z_]\w*\s*,\s*MDATA_INF(?:\s*&)?\s+ArgInf\s*,\s*\.\.\.\s*\)')) {
        Register-LegacyFunction -Name $match.Groups['name'].Value -ReturnType $match.Groups['type'].Value -SourceName $sourceName -SourceText $sourceText -DefinitionOffset $match.Index
    }
}
$modernTypedefPath = Get-ChildItem -Path $ModernCoreRoot -Recurse -Filter 'krnln_cmd_typedef.h' -File | Select-Object -First 1 -ExpandProperty FullName
if ([string]::IsNullOrEmpty($modernTypedefPath)) { throw "krnln_cmd_typedef.h not found below $ModernCoreRoot" }
$modernTypedef = Get-Content -LiteralPath $modernTypedefPath -Raw -Encoding UTF8
$modernCommands = @{}
$modernReturnTypes = @{}
$modernCategories = @{}
$modernChineseNames = @{}
$modernFunctionNames = @{}
$targetExecuteSymbols = @{}
if (-not [string]::IsNullOrWhiteSpace($TargetSymbolMapPath) -and (Test-Path -Path $TargetSymbolMapPath)) {
    $targetSymbolText = Get-Content -LiteralPath $TargetSymbolMapPath -Raw -Encoding UTF8
    foreach ($match in [regex]::Matches($targetSymbolText, '(?m)^\s*[0-9A-F]+\s+(krnln_[A-Za-z0-9_]+_(\d+)_krnln)\s*$')) {
        $commandIndex = [int]$match.Groups[2].Value
        if (-not $targetExecuteSymbols.ContainsKey($commandIndex)) {
            $targetExecuteSymbols[$commandIndex] = $match.Groups[1].Value
        }
    }
}
$normalizedModernCommands = @{}
function Get-CommandAbiName([string]$name) {
    $normalized = $name.ToLowerInvariant()
    # The generated FNE C++ identifier appends an underscore when the source
    # command name conflicts with a C/C++ keyword (for example int -> int_).
    # Keep this as a fallback only: an exact identifier always wins.
    if ($normalized.Length -gt 1 -and $normalized.EndsWith('_')) {
        $normalized = $normalized.Substring(0, $normalized.Length - 1)
    }
    # The SDK uses W/A suffixes for a small group of Unicode-capable commands.
    # Legacy BlackMoon implementations expose the architecture-neutral name.
    # Only use this normalization as an unambiguous fallback after exact name
    # matching, so unrelated commands are never merged by convention.
    if ($normalized.Length -gt 1 -and ($normalized.EndsWith('w') -or $normalized.EndsWith('a'))) {
        return $normalized.Substring(0, $normalized.Length - 1)
    }
    return $normalized
}
foreach ($match in [regex]::Matches($modernTypedef, '_MAKE\(\s*(\d+)\s*,\s*"([^"]*)"\s*,\s*"[^"]*"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)')) {
    $name = $match.Groups[2].Value
    $index = [int]$match.Groups[1].Value
    $modernCommands[$match.Groups[3].Value.ToLowerInvariant()] = $index
    $modernChineseNames[$index] = $name
    $modernFunctionNames[$index] = $match.Groups[3].Value
    $canonicalName = Get-CommandAbiName $match.Groups[3].Value
    if (-not $normalizedModernCommands.ContainsKey($canonicalName)) {
        $normalizedModernCommands[$canonicalName] = [System.Collections.Generic.List[int]]::new()
    }
    $normalizedModernCommands[$canonicalName].Add($index)
}
foreach ($match in [regex]::Matches($modernTypedef, '_MAKE\(\s*(\d+)\s*,\s*"[^"]*"\s*,\s*"[^"]*"\s*,\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*"(?:\\.|[^"])*"\s*,\s*[^,]+\s*,\s*[^,]+\s*,\s*([^,]+)\s*,')) {
    $modernReturnTypes[[int]$match.Groups[1].Value] = $match.Groups[2].Value.Trim()
}
foreach ($match in [regex]::Matches($modernTypedef, '_MAKE\(\s*(\d+)\s*,\s*"[^"]*"\s*,\s*"[^"]*"\s*,\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*"(?:\\.|[^"])*"\s*,\s*(-?\d+)\s*,')) {
    $modernCategories[[int]$match.Groups[1].Value] = [int]$match.Groups[2].Value
}

function Resolve-LegacyCommandIndex {
    param([string]$LegacyName)

    $commandKey = if ($legacyFunctionAbiNames.ContainsKey($LegacyName)) {
        $legacyFunctionAbiNames[$LegacyName]
    }
    else {
        $LegacyName
    }
    if ($commandKey.StartsWith('krnln_', [StringComparison]::OrdinalIgnoreCase)) {
        $commandKey = $commandKey.Substring(6)
    }
    if ($legacyFunctionChineseNames.ContainsKey($LegacyName)) {
        $commentCandidates = @($modernChineseNames.GetEnumerator() |
            Where-Object { $_.Value -eq $legacyFunctionChineseNames[$LegacyName] })
        if ($commentCandidates.Count -eq 1) {
            return [int]$commentCandidates[0].Key
        }
    }
    $direct = $modernCommands[$commandKey.ToLowerInvariant()]
    if ($null -ne $direct) { return [int]$direct }
    $candidates = $normalizedModernCommands[(Get-CommandAbiName $commandKey)]
    if ($candidates -ne $null -and $candidates.Count -eq 1) {
        return [int]$candidates[0]
    }
    return $null
}

function Get-DataTypeMemberCommandIndexes {
    param([string]$TypeName)

    $dataTypeFile = Get-ChildItem -Path $ModernCoreRoot -Recurse -Filter 'krnln_dtType.cpp' -File |
        Select-Object -First 1 -ExpandProperty FullName
    if ([string]::IsNullOrEmpty($dataTypeFile)) { return @() }
    $source = Get-Content -LiteralPath $dataTypeFile -Raw -Encoding UTF8
    $pattern = '(?s)static\s+INT\s+s_dtCmdIndex_' + [regex]::Escape($TypeName) + '\s*\[\]\s*=\s*\{(?<body>.*?)\};'
    $match = [regex]::Match($source, $pattern)
    if (-not $match.Success) { return @() }
    return @([regex]::Matches($match.Groups['body'].Value, '\d+') | ForEach-Object { [int]$_.Value })
}

# COM object commands share the same legacy dispatch implementation. Build
# their lifecycle hook from the FNE owner table rather than selecting a
# duplicate Clear command by name alone.
$comLifecycleSourceName = ''
$comClearCommandIndex = Get-DataTypeMemberCommandIndexes 'ComObject' |
    Where-Object { $modernFunctionNames.ContainsKey($_) -and $modernFunctionNames[$_] -eq 'Clear' } |
    Select-Object -First 1
if ($null -ne $comClearCommandIndex -and $targetExecuteSymbols.ContainsKey($comClearCommandIndex)) {
    $comLifecycleSourceName = 'bm_com_lifecycle.cpp'
    $comLifecycleSource = @"
#include "stdafx.h"

extern "C" void $($targetExecuteSymbols[$comClearCommandIndex])(PMDATA_INF out, INT count, PMDATA_INF args) {
    (void)out;
    if (!args || count <= 0) return;
    auto* target = static_cast<PEYDISPATCH>(args[0].m_pCompoundData);
    if (!target) return;
    if (target->pDisp) {
        target->pDisp->Release();
        target->pDisp = nullptr;
    }
    target->hRet = 0;
}
"@
    [IO.File]::WriteAllText((Join-Path $OutputRoot $comLifecycleSourceName), $comLifecycleSource, [Text.Encoding]::ASCII)
}

$wrapperDirectory = Join-Path $OutputRoot 'wrappers'
if (Test-Path -Path $wrapperDirectory) {
    Remove-Item -Path $wrapperDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $wrapperDirectory -Force | Out-Null
$wrapperSupport = @'
#pragma once
#include "stdafx.h"
#include <type_traits>
#include <vector>

template<class R> static void bm_assign_legacy(R value, PMDATA_INF out) {
    if (!out) return;
    if constexpr (std::is_pointer_v<R>) out->m_pCompoundData = reinterpret_cast<void*>(value);
    else if constexpr (std::is_floating_point_v<R>) out->m_double = static_cast<double>(value);
    else if constexpr (sizeof(R) <= sizeof(INT)) out->m_int = static_cast<INT>(value);
    else out->m_int64 = static_cast<INT64>(value);
}

template<class R> static void bm_call_legacy_with_args(R (__cdecl* fn)(INT, MDATA_INF&, ...), PMDATA_INF out, INT count, PMDATA_INF args) {
    MDATA_INF dummy = {};
    MDATA_INF& first = (args && count > 0) ? args[0] : dummy;
    if constexpr (std::is_void_v<R>) {
        fn(count, first);
        if (out && out->m_dtDataType != _SDT_NULL) {
            const DATA_TYPE expected = out->m_dtDataType;
            *out = first;
            if (expected != _SDT_ALL) out->m_dtDataType = expected;
        }
    } else {
        bm_assign_legacy(fn(count, first), out);
    }
}

static std::vector<MDATA_INF> bm_copy_opaque_handle_args(INT count, PMDATA_INF args) {
    const INT safeCount = count > 0 ? count : 0;
    std::vector<MDATA_INF> copied(static_cast<std::size_t>(safeCount));
    for (INT index = 0; index < safeCount; ++index) {
        copied[static_cast<std::size_t>(index)] = args[index];
        if (copied[static_cast<std::size_t>(index)].m_dtDataType != SDT_INT) continue;
        if (void* handle = bm_resolve_opaque_handle(copied[static_cast<std::size_t>(index)].m_int)) {
            copied[static_cast<std::size_t>(index)].m_pCompoundData = handle;
        }
    }
    return copied;
}

template<class R> static void bm_call_legacy(R (__cdecl* fn)(INT, MDATA_INF&, ...), PMDATA_INF out, INT count, PMDATA_INF args) {
    bm_call_legacy_with_args(fn, out, count, args);
}

template<class R> static void bm_call_legacy_with_opaque_handles(R (__cdecl* fn)(INT, MDATA_INF&, ...), PMDATA_INF out, INT count, PMDATA_INF args) {
    auto copied = bm_copy_opaque_handle_args(count, args);
    bm_call_legacy_with_args(fn, out, count, copied.empty() ? nullptr : copied.data());
}

template<class R> static void bm_call_legacy_raw_pointer(R (__cdecl* fn)(INT, MDATA_INF&, ...), PMDATA_INF out, INT count, PMDATA_INF args) {
    if (!args || count <= 0 || args[0].m_dtDataType != SDT_SUB_PTR) {
        if (out) out->m_pCompoundData = nullptr;
        return;
    }
    bm_call_legacy_with_args(fn, out, count, args);
}

template<class R> static void bm_call_legacy_opaque_result(R (__cdecl* fn)(INT, MDATA_INF&, ...), PMDATA_INF out, INT count, PMDATA_INF args) {
    auto copied = bm_copy_opaque_handle_args(count, args);
    PMDATA_INF copiedArgs = copied.empty() ? nullptr : copied.data();
    MDATA_INF dummy = {};
    MDATA_INF& first = (copiedArgs && count > 0) ? copiedArgs[0] : dummy;
    const void* result = fn(count, first);
    if (!out) return;
    const DATA_TYPE expected = out->m_dtDataType;
    out->m_int = bm_register_opaque_handle(const_cast<void*>(result));
    if (expected != _SDT_ALL) out->m_dtDataType = expected;
}
'@
[IO.File]::WriteAllText((Join-Path $OutputRoot 'bm_wrapper_support.h'), $wrapperSupport, [Text.Encoding]::UTF8)

# A legacy pointer-returning command with an integer FNE return type is an
# opaque handle producer.  Detect the pointer typedef from the source's own
# allocation-and-return pattern, then only adapt consumers that use that same
# typedef with m_pCompoundData.  Raw memory-pointer commands remain excluded.
$opaqueHandleProducerTypes = @{}
$opaqueHandleTypes = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($entry in $legacyFunctions.GetEnumerator()) {
    if ($entry.Value -notmatch '^(?:void\s*\*|LPBYTE|PBYTE)$') { continue }
    $targetIndex = Resolve-LegacyCommandIndex $entry.Key
    if ($null -eq $targetIndex -or $modernReturnTypes[$targetIndex] -ne 'SDT_INT') { continue }
    $typeMatches = @([regex]::Matches(
        $legacyFunctionBodies[$entry.Key],
        '(?s)\b(?<type>P[A-Z][A-Za-z0-9_]*)\s+(?<variable>p[A-Za-z0-9_]*)\s*=\s*[^;]+;.*?\breturn\s+\k<variable>\s*;'))
    if ($typeMatches.Count -eq 0) { continue }
    $typeNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($typeMatch in $typeMatches) {
        $typeName = $typeMatch.Groups['type'].Value
        [void]$typeNames.Add($typeName)
        [void]$opaqueHandleTypes.Add($typeName)
    }
    $opaqueHandleProducerTypes[$entry.Key] = $typeNames
}

$requestedEligibleSources = $null
if (-not [string]::IsNullOrWhiteSpace($EligibleSourcesPath) -and (Test-Path -Path $EligibleSourcesPath)) {
    $requestedEligibleSources = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($sourceName in Get-Content -LiteralPath $EligibleSourcesPath -Encoding UTF8) {
        if (-not [string]::IsNullOrWhiteSpace($sourceName)) {
            [void]$requestedEligibleSources.Add($sourceName.Trim())
        }
    }
}

$selectedHelperSources = @($helperSources)
if ($requestedEligibleSources -ne $null) {
    $selectedHelperSources = @($helperSources | Where-Object { $requestedEligibleSources.Contains($_) })
}

$wrapperCount = 0
$adapterSourceNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
if (-not [string]::IsNullOrEmpty($comLifecycleSourceName)) {
    [void]$adapterSourceNames.Add($comLifecycleSourceName)
}
foreach ($entry in $legacyFunctions.GetEnumerator() | Sort-Object Name) {
    $legacyName = $entry.Key
    $legacyType = $entry.Value
    $commandKey = if ($legacyFunctionAbiNames.ContainsKey($legacyName)) {
        $legacyFunctionAbiNames[$legacyName]
    }
    else {
        $legacyName
    }
    if ($commandKey.StartsWith('krnln_', [StringComparison]::OrdinalIgnoreCase)) {
        $commandKey = $commandKey.Substring(6)
    }
    # SDK revisions can retain an old English identifier for a different
    # command. The source comment's call format carries the historical public
    # Chinese name; only an exact, unique match is allowed to override the ABI
    # identifier. References in explanatory prose are deliberately ignored.
    $commentIndex = $null
    if ($legacyFunctionChineseNames.ContainsKey($legacyName)) {
        $commentCandidates = @($modernChineseNames.GetEnumerator() |
            Where-Object { $_.Value -eq $legacyFunctionChineseNames[$legacyName] })
        if ($commentCandidates.Count -eq 1) {
            $commentIndex = [int]$commentCandidates[0].Key
        }
    }

    $modernIndex = $commentIndex
    if ($null -eq $modernIndex) {
        $modernIndex = $modernCommands[$commandKey.ToLowerInvariant()]
    }
    if ($null -eq $modernIndex) {
        $canonicalName = Get-CommandAbiName $commandKey
        $candidates = $normalizedModernCommands[$canonicalName]
        if ($candidates -ne $null -and $candidates.Count -eq 1) {
            $modernIndex = $candidates[0]
        }
    }
    if ($null -eq $modernIndex) { continue }
    $sourceName = $legacyFunctionSources[$legacyName]
    if ([string]::IsNullOrEmpty($sourceName)) { continue }
    if ($requestedEligibleSources -ne $null -and -not $requestedEligibleSources.Contains($sourceName)) { continue }
    $targetReturn = $modernReturnTypes[$modernIndex]
    $isMemberCommand = $modernCategories.ContainsKey($modernIndex) -and $modernCategories[$modernIndex] -eq -1
    $pointerReturn = $legacyType -match '^(?:void\s*\*|LPBYTE|PBYTE)$'
    $legacyBody = $legacyFunctionBodies[$legacyName]
    $isOpaqueHandleProducer = $pointerReturn -and $targetReturn -eq 'SDT_INT' -and
        $opaqueHandleProducerTypes.ContainsKey($legacyName)
    $usesCompoundPointerSlot = $legacyBody -match '\.m_pCompoundData\b'
    $usesKnownOpaqueHandleType = $false
    foreach ($handleType in $opaqueHandleTypes) {
        if ($legacyBody -match ('\b' + [regex]::Escape($handleType) + '\b')) {
            $usesKnownOpaqueHandleType = $true
            break
        }
    }
    $isOpaqueHandleConsumer = $usesCompoundPointerSlot -and $usesKnownOpaqueHandleType
    $isDirectCompoundObjectConsumer = $usesCompoundPointerSlot -and $isMemberCommand
    $isRawPointerBinaryConsumer = $usesCompoundPointerSlot -and -not $isMemberCommand -and
        -not $isOpaqueHandleConsumer -and $pointerReturn -and $targetReturn -eq 'SDT_BIN'
    $usesUnsupportedPointerSlot = $legacyBody -match '\.m_pUnit\b|\.m_pdwSubCodeAdr\b'
    # A legacy raw pointer remains unsupported when FNE exposes only an int.
    # Pointer-as-int producer/consumer pairs are handled through the generic
    # opaque-handle registry above.
    if (($pointerReturn -and $targetReturn -notin @('SDT_BIN', 'SDT_TEXT') -and -not $isOpaqueHandleProducer -and -not $isMemberCommand) -or
        ($usesCompoundPointerSlot -and -not $isOpaqueHandleConsumer -and -not $isDirectCompoundObjectConsumer -and -not $isRawPointerBinaryConsumer) -or
        $usesUnsupportedPointerSlot) {
        continue
    }
    $symbol = if ($targetExecuteSymbols.ContainsKey($modernIndex)) {
        $targetExecuteSymbols[$modernIndex]
    }
    else {
        '{0}_{1}_krnln' -f $legacyName, $modernIndex
    }
    $callExpression = if ($isOpaqueHandleProducer) {
        ('bm_call_legacy_opaque_result<{0}>(&{1}, out, count, args);' -f $legacyType, $legacyName)
    }
    elseif ($isOpaqueHandleConsumer) {
        ('bm_call_legacy_with_opaque_handles<{0}>(&{1}, out, count, args);' -f $legacyType, $legacyName)
    }
    elseif ($isRawPointerBinaryConsumer) {
        ('bm_call_legacy_raw_pointer<{0}>(&{1}, out, count, args);' -f $legacyType, $legacyName)
    }
    else {
        ('bm_call_legacy<{0}>(&{1}, out, count, args);' -f $legacyType, $legacyName)
    }
    $releaseExpression = if ($isOpaqueHandleConsumer -and $legacyBody -match '\bCloseEfile\s*\(') {
        ' if (args != nullptr && count > 0) bm_release_opaque_handle(args[0].m_int);'
    }
    else {
        ''
    }
    $wrapperSource = @(
        '#include "../bm_wrapper_support.h"',
        ('extern "C" {0} __cdecl {1}(INT, MDATA_INF&, ...);' -f $legacyType, $legacyName),
        ('extern "C" void {0}(PMDATA_INF out, INT count, PMDATA_INF args) {{ {1}{2} }}' -f $symbol, $callExpression, $releaseExpression)
    )
    [IO.File]::WriteAllLines((Join-Path $wrapperDirectory ($legacyName + '.cpp')), $wrapperSource, [Text.Encoding]::UTF8)
    [void]$adapterSourceNames.Add($sourceName)
    ++$wrapperCount
}
foreach ($sourceName in $selectedHelperSources) {
    [void]$adapterSourceNames.Add($sourceName)
}
Remove-Item -Path (Join-Path $OutputRoot 'bm_wrappers.cpp') -Force -ErrorAction SilentlyContinue
[IO.File]::WriteAllLines((Join-Path $OutputRoot 'adapter-source-files.txt'), @('bm_runtime.cpp') + @($adapterSourceNames | Sort-Object), [Text.Encoding]::UTF8)
[IO.File]::WriteAllLines((Join-Path $OutputRoot 'adapter-command-source-files.txt'), @($commandSources | Sort-Object), [Text.Encoding]::UTF8)
[IO.File]::WriteAllLines((Join-Path $OutputRoot 'adapter-helper-source-files.txt'), @($helperSources | Sort-Object), [Text.Encoding]::UTF8)

Write-Output ($OutputRoot + '|' + $commandSources.Count + '|' + $wrapperCount)
