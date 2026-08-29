param(
    [Parameter(Mandatory = $true)]
    [string]$ObjectPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ObjectPath -PathType Leaf)) {
    throw "黑月拼音数据对象不存在: $ObjectPath"
}

$bytes = [IO.File]::ReadAllBytes($ObjectPath)
if ($bytes.Length -lt 20) {
    throw "黑月拼音数据对象无效: $ObjectPath"
}

function Read-U16 {
    param([int]$Offset)
    if ($Offset -lt 0 -or $Offset + 2 -gt $bytes.Length) { throw "COFF 数据越界: $Offset" }
    return [BitConverter]::ToUInt16($bytes, $Offset)
}

function Read-I16 {
    param([int]$Offset)
    if ($Offset -lt 0 -or $Offset + 2 -gt $bytes.Length) { throw "COFF 数据越界: $Offset" }
    return [BitConverter]::ToInt16($bytes, $Offset)
}

function Read-U32 {
    param([int]$Offset)
    if ($Offset -lt 0 -or $Offset + 4 -gt $bytes.Length) { throw "COFF 数据越界: $Offset" }
    return [BitConverter]::ToUInt32($bytes, $Offset)
}

function Read-AsciiFixed {
    param([int]$Offset, [int]$Length)
    if ($Offset -lt 0 -or $Offset + $Length -gt $bytes.Length) { throw "COFF 数据越界: $Offset" }
    return ([Text.Encoding]::ASCII.GetString($bytes[$Offset..($Offset + $Length - 1)])).Trim([char]0)
}

function Read-AsciiZeroTerminated {
    param([int]$Offset)
    if ($Offset -lt 0 -or $Offset -ge $bytes.Length) { throw "COFF 数据越界: $Offset" }
    $end = $Offset
    while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { ++$end }
    if ($end -eq $Offset) { return '' }
    return [Text.Encoding]::ASCII.GetString($bytes[$Offset..($end - 1)])
}

$machine = Read-U16 0
if ($machine -ne 0x014c) {
    throw ("PY.OBJ 不是预期的 x86 COFF 数据对象: machine=0x{0:X4}" -f $machine)
}
$sectionCount = Read-U16 2
$symbolOffset = [int](Read-U32 8)
$symbolCount = [int](Read-U32 12)
$sectionOffset = 20 + (Read-U16 16)
if ($sectionOffset + $sectionCount * 40 -gt $bytes.Length) {
    throw 'PY.OBJ 的节表越界。'
}

$sections = @()
for ($index = 0; $index -lt $sectionCount; ++$index) {
    $offset = $sectionOffset + $index * 40
    $sections += [PSCustomObject]@{
        Index = $index + 1
        RawSize = [int](Read-U32 ($offset + 16))
        RawOffset = [int](Read-U32 ($offset + 20))
        RelocationOffset = [int](Read-U32 ($offset + 24))
        RelocationCount = [int](Read-U16 ($offset + 32))
    }
}
if ($symbolOffset + $symbolCount * 18 -gt $bytes.Length) {
    throw 'PY.OBJ 的符号表越界。'
}
$stringOffset = $symbolOffset + $symbolCount * 18

function Read-SymbolName {
    param([int]$Index)
    $offset = $symbolOffset + $Index * 18
    if ((Read-U32 $offset) -eq 0) {
        return Read-AsciiZeroTerminated ($stringOffset + [int](Read-U32 ($offset + 4)))
    }
    return Read-AsciiFixed $offset 8
}

$symbols = @()
for ($index = 0; $index -lt $symbolCount; ++$index) {
    $offset = $symbolOffset + $index * 18
    $symbols += [PSCustomObject]@{
        Index = $index
        Name = Read-SymbolName $index
        Value = [uint32](Read-U32 ($offset + 8))
        Section = [int](Read-I16 ($offset + 12))
    }
}

function Find-Symbol {
    param([string]$Pattern)
    $found = @($symbols | Where-Object { $_.Name -match $Pattern } | Select-Object -First 1)
    if ($found.Count -ne 1) { throw "PY.OBJ 中缺少符号: $Pattern" }
    return $found[0]
}

$nameTable = Find-Symbol '^\?s_PYNameTab@@'
$pinyinTable = Find-Symbol '^\?s_wPYTab@@'
$manyTable = Find-Symbol '^\?s_wManyPYTab@@'
if ($pinyinTable.Section -le 0 -or $pinyinTable.Section -gt $sections.Count -or
    $manyTable.Section -ne $pinyinTable.Section -or $nameTable.Section -ne $pinyinTable.Section) {
    throw 'PY.OBJ 中的拼音数据符号不在同一有效数据节。'
}
$dataSection = $sections[$pinyinTable.Section - 1]
if ($dataSection.RawOffset + $dataSection.RawSize -gt $bytes.Length) {
    throw 'PY.OBJ 拼音数据节越界。'
}

$names = @('') * 256
for ($index = 0; $index -lt $dataSection.RelocationCount; ++$index) {
    $offset = $dataSection.RelocationOffset + $index * 10
    if ($offset + 10 -gt $bytes.Length) { throw 'PY.OBJ 重定位表越界。' }
    $address = [int](Read-U32 $offset)
    $symbolIndex = [int](Read-U32 ($offset + 4))
    if ($address -ge $pinyinTable.Value -or ($address % 8) -ne 4 -or $symbolIndex -ge $symbols.Count) { continue }
    $code = [int](Read-U32 ($dataSection.RawOffset + $address - 4))
    if ($code -lt 0 -or $code -ge $names.Count) { continue }
    $targetSymbol = $symbols[$symbolIndex]
    if ($targetSymbol.Section -le 0 -or $targetSymbol.Section -gt $sections.Count) { continue }
    $targetSection = $sections[$targetSymbol.Section - 1]
    $targetOffset = $targetSection.RawOffset + $targetSymbol.Value + [int](Read-U32 ($dataSection.RawOffset + $address))
    $names[$code] = Read-AsciiZeroTerminated $targetOffset
}
if (($names | Where-Object { $_ }).Count -lt 16) {
    throw 'PY.OBJ 中未能解析足够的拼音名称重定位。'
}

$tableCount = (0xF7 - 0xB0 + 1) * 94
$pinyinDataOffset = $dataSection.RawOffset + $pinyinTable.Value
if ($pinyinDataOffset + $tableCount * 2 -gt $dataSection.RawOffset + $dataSection.RawSize) {
    throw 'PY.OBJ 的 GBK 拼音表越界。'
}
$pinyinValues = [System.Collections.Generic.List[UInt16]]::new()
$maxManyIndex = -1
for ($index = 0; $index -lt $tableCount; ++$index) {
    $value = Read-U16 ($pinyinDataOffset + $index * 2)
    $pinyinValues.Add($value)
    if (($value -band 0x8000) -ne 0) {
        $maxManyIndex = [Math]::Max($maxManyIndex, ($value -band 0x7fff))
    }
}
$manyCount = ($maxManyIndex + 1) * 3
$manyDataOffset = $dataSection.RawOffset + $manyTable.Value
if ($manyCount -le 0 -or $manyDataOffset + $manyCount * 2 -gt $dataSection.RawOffset + $dataSection.RawSize) {
    throw 'PY.OBJ 的多音字表越界。'
}
$manyValues = [System.Collections.Generic.List[UInt16]]::new()
for ($index = 0; $index -lt $manyCount; ++$index) {
    $manyValues.Add((Read-U16 ($manyDataOffset + $index * 2)))
}

function Format-U16Array {
    param([System.Collections.IEnumerable]$Values, [int]$Columns = 12)
    $builder = [Text.StringBuilder]::new()
    $line = [System.Collections.Generic.List[string]]::new()
    $index = 0
    foreach ($value in $Values) {
        $line.Add(('0x{0:X4}u' -f ([uint16]$value)))
        ++$index
        if (($index % $Columns) -eq 0) {
            [void]$builder.Append('    ' + ($line -join ', ') + ",`r`n")
            $line.Clear()
        }
    }
    if ($line.Count -gt 0) {
        [void]$builder.Append('    ' + ($line -join ', ') + "`r`n")
    }
    return $builder.ToString()
}

function Format-NameArray {
    param([string[]]$Values, [int]$Columns = 8)
    $builder = [Text.StringBuilder]::new()
    $line = [System.Collections.Generic.List[string]]::new()
    $index = 0
    foreach ($value in $Values) {
        $line.Add(('"{0}"' -f $value.Replace('\', '\\').Replace('"', '\"')))
        ++$index
        if (($index % $Columns) -eq 0) {
            [void]$builder.Append('    ' + ($line -join ', ') + ",`r`n")
            $line.Clear()
        }
    }
    if ($line.Count -gt 0) {
        [void]$builder.Append('    ' + ($line -join ', ') + "`r`n")
    }
    return $builder.ToString()
}

$template = @'
#include "stdafx.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {
static constexpr const char* kPinyinNames[256] = {
__PY_NAMES__};
static constexpr std::uint16_t kPinyinTable[__PY_TABLE_COUNT__] = {
__PY_TABLE__};
static constexpr std::uint16_t kManyPinyinTable[__PY_MANY_COUNT__] = {
__PY_MANY__};

static const char* PinyinName(std::uint8_t code) {
    return kPinyinNames[code] ? kPinyinNames[code] : "";
}

static bool IsMappedGbk(std::uint8_t high, std::uint8_t low) {
    return high >= 0xB0u && high <= 0xF7u && low >= 0xA1u && low <= 0xFEu;
}

static std::vector<std::uint16_t> PinyinWords(const char* text) {
    if (!text) return {};
    const auto high = static_cast<std::uint8_t>(text[0]);
    const auto low = static_cast<std::uint8_t>(text[1]);
    if (!IsMappedGbk(high, low)) return {};
    const std::size_t index = static_cast<std::size_t>(high - 0xB0u) * 94u + static_cast<std::size_t>(low - 0xA1u);
    const std::uint16_t value = kPinyinTable[index];
    if (value == 0) return {};
    if ((value & 0x8000u) == 0) return {value};
    const std::size_t manyIndex = static_cast<std::size_t>(value & 0x7fffu) * 3u;
    std::vector<std::uint16_t> values;
    for (std::size_t offset = 0; offset < 3u && manyIndex + offset < std::size(kManyPinyinTable); ++offset) {
        if (kManyPinyinTable[manyIndex + offset] != 0) values.push_back(kManyPinyinTable[manyIndex + offset]);
    }
    return values;
}

static std::string PinyinText(std::uint16_t word, int mode = 0) {
    std::string result;
    if (mode != 2) result += PinyinName(static_cast<std::uint8_t>(word & 0xffu));
    if (mode != 1) result += PinyinName(static_cast<std::uint8_t>(word >> 8));
    return result;
}

static char* ClonePinyinText(const std::string& value) {
    return value.empty() ? nullptr : CloneTextData(const_cast<char*>(value.c_str()), static_cast<INT>(value.size()));
}

static LPBYTE MakePinyinArray(const std::vector<std::uint16_t>& words) {
    const std::size_t payload = words.size() * sizeof(char*);
    auto* result = static_cast<LPBYTE>(E_MAlloc_Nzero(static_cast<DWORD>(sizeof(INT) * 2 + payload)));
    if (!result) return nullptr;
    *reinterpret_cast<INT*>(result) = 1;
    *reinterpret_cast<INT*>(result + sizeof(INT)) = static_cast<INT>(words.size());
    auto** values = reinterpret_cast<char**>(result + sizeof(INT) * 2);
    for (std::size_t index = 0; index < words.size(); ++index) values[index] = ClonePinyinText(PinyinText(words[index]));
    return result;
}

static std::vector<std::string> PinyinPossibilities(const char* text) {
    std::vector<std::string> values(1);
    if (!text) return values;
    const auto* cursor = reinterpret_cast<const unsigned char*>(text);
    while (*cursor) {
        std::vector<std::string> parts;
        if (cursor[1] && IsMappedGbk(cursor[0], cursor[1])) {
            for (const auto word : PinyinWords(reinterpret_cast<const char*>(cursor))) parts.push_back(PinyinText(word));
            cursor += 2;
        }
        else {
            parts.emplace_back(1, static_cast<char>(std::tolower(*cursor)));
            ++cursor;
        }
        if (parts.empty()) parts.emplace_back();
        std::vector<std::string> next;
        for (const auto& prefix : values) {
            for (const auto& part : parts) {
                next.push_back(prefix + part);
                if (next.size() >= 1024u) break;
            }
            if (next.size() >= 1024u) break;
        }
        values.swap(next);
    }
    return values;
}

static BOOL ComparePinyin(const char* left, const char* right, bool prefix) {
    const auto leftValues = PinyinPossibilities(left);
    const auto rightValues = PinyinPossibilities(right);
    for (const auto& first : leftValues) {
        for (const auto& second : rightValues) {
            if (prefix ? second.rfind(first, 0) == 0 : first == second) return TRUE;
        }
    }
    return FALSE;
}
}  // namespace

LIBAPI(LPBYTE, krnln_GetAllPY) {
    return MakePinyinArray(PinyinWords(ArgInf.m_pText));
}

LIBAPI(int, krnln_GetPYCount) {
    return static_cast<int>(PinyinWords(ArgInf.m_pText).size());
}

static char* GetPinyinPart(INT count, MDATA_INF& first, int mode) {
    PMDATA_INF args = &first;
    const auto words = PinyinWords(first.m_pText);
    const int requested = (count > 1 && args[1].m_dtDataType != _SDT_NULL) ? args[1].m_int : 1;
    if (requested < 1 || static_cast<std::size_t>(requested) > words.size()) return nullptr;
    return ClonePinyinText(PinyinText(words[static_cast<std::size_t>(requested - 1)], mode));
}

LIBAPI(char*, krnln_GetPY) {
    return GetPinyinPart(nArgCount, ArgInf, 0);
}

LIBAPI(char*, krnln_GetSM) {
    return GetPinyinPart(nArgCount, ArgInf, 1);
}

LIBAPI(char*, krnln_GetYM) {
    return GetPinyinPart(nArgCount, ArgInf, 2);
}

LIBAPI(BOOL, krnln_CompPY) {
    PMDATA_INF args = &ArgInf;
    const bool prefix = nArgCount > 3 && args[3].m_dtDataType != _SDT_NULL && args[3].m_bool != FALSE;
    return ComparePinyin(args[0].m_pText, args[1].m_pText, prefix);
}
'@

$source = $template.
    Replace('__PY_NAMES__', (Format-NameArray $names)).
    Replace('__PY_TABLE_COUNT__', [string]$pinyinValues.Count).
    Replace('__PY_TABLE__', (Format-U16Array $pinyinValues)).
    Replace('__PY_MANY_COUNT__', [string]$manyValues.Count).
    Replace('__PY_MANY__', (Format-U16Array $manyValues))

$outputDirectory = Split-Path -Parent $OutputPath
if (-not [string]::IsNullOrEmpty($outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
# The adapter's legacy-source pass reads copied BlackMoon files as CP936.
# This generated translation unit is intentionally ASCII, so omit a UTF-8
# BOM and let it travel through that pass without any encoding conversion.
[IO.File]::WriteAllText($OutputPath, $source, [Text.Encoding]::ASCII)
Write-Output ("pinyin-source-generated: output={0};names={1};table={2};many={3}" -f $OutputPath, ($names | Where-Object { $_ }).Count, $pinyinValues.Count, $manyValues.Count)
