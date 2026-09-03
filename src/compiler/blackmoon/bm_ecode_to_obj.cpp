// ============================================================================
// bm_ecode_to_obj.cpp - 易代码到 COFF OBJ 转换实现
// ============================================================================
// 移植自 BlackMoon_VS2019/EcodeToObjFile.cpp(原 CEcodeToObjFile 类)。
// 将易语言编译器生成的中间 PE 文件(内含 e 代码段)解析为 COFF OBJ。
//
// 主要改动:
//   * MFC CString/CStringArray/CPtrArray/CDWordArray -> std::string/std::vector
//   * 类名 CEcodeToObjFile -> bm::EcodeToObjFile,方法名改为 camelCase
//   * _T("xxx") -> "xxx"(执行字符集为 GBK,中文照常显示)
//   * eLibFuncList 大表(688 项)拆出到 bm_elibfunclist.inc
//   * insnLenX86_32 移植自 InstructionLen.h(BSD, oblique)
//   * MakeFuncToCdecl 移植自 Common.cpp
//   * EDllExportInfo/g_eDllExportInfo:从易语言 IDE 内存(g_pBaseFunc 子程序表)
//     读取 DLL 导出声明,实现自定义调用约定/命名(见 bm_mem_scan)。
// ============================================================================
#if !defined(_M_IX86)
int bm_ecode_to_obj_x64_placeholder = 0;
#else

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "bm_ecode_to_obj.h"
#include "bm_globals.h"
#include "bm_constants.h"
#include "bm_string.h"
#include "bm_path.h"
#include "bm_mem_scan.h"
#include "sdk/elib_sdk.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <new>
#include <memory>
#include <climits>

// 支持库通知表预留长度(常量数据段头部)
#define LIBLISTLEN 512

#pragma warning(disable: 4244 4267)

// ----------------------------------------------------------------------------
// PE 文件布局常量
// ----------------------------------------------------------------------------
#define SIZE_OF_NT_SIGNATURE 4

namespace {

// ============================================================================
// 易语言 SDK 数据类型信息结构(对应 e SDK 的 LIB_DATA_TYPE_INFO)
// ----------------------------------------------------------------------------
// elib_sdk.h 中 m_pDataType 简化为 LPVOID,此处按 e SDK 32 位布局定义,
// 以便正确按 sizeof 步进遍历数据类型数组。
// ============================================================================
struct LibDataTypeInfo {
    char*       m_szName;
    char*       m_szEgName;
    char*       m_szExplain;
    INT         m_nCmdCount;
    INT*        m_pnCmdsIndex;
    DWORD       m_dwState;
    DWORD       m_dwUnitBmpID;
    INT         m_nEventCount;
    void*       m_pEventBegin;
    INT         m_nPropertyCount;
    void*       m_pPropertyBegin;   // 指向 LibUnitProperty[]
    void*       m_pfnGetInterface;
    INT         m_nElementCount;
    void*       m_pElementBegin;
};

// 易语言窗口单元属性结构(对应 e SDK 的 UNIT_PROPERTY)
struct LibUnitProperty {
    char*       m_szName;
    char*       m_szEgName;
    char*       m_szExplain;
    SHORT       m_shtType;
    WORD        m_wState;
    char*       m_szzPickStr;
};

// 布局校验(仅 32 位构建生效;指针 4 字节)。代码本身仅支持 32 位(x86)目标。
static_assert(sizeof(void*) != 4 || sizeof(LibDataTypeInfo) == 56, "LibDataTypeInfo layout mismatch (32-bit expected)");
static_assert(sizeof(void*) != 4 || offsetof(LibUnitProperty, m_shtType) == 12, "LibUnitProperty.m_shtType offset mismatch");
static_assert(sizeof(void*) != 4 || sizeof(LibUnitProperty) == 20, "LibUnitProperty layout mismatch (32-bit expected)");

#ifndef LDT_WIN_UNIT
#define LDT_WIN_UNIT (1 << 6)
#endif
#ifndef UD_TEXT
#define UD_TEXT 1005
#endif

// ============================================================================
// EDllExport / g_eDllExportInfo
// ============================================================================
// bm_mem_scan.h 中定义了 EDllExportInfo::getByOrgName(),从易语言 IDE 内存
// (g_pBaseFunc 子程序表)读取子程序注释中的 ($cdecl)/($stdcall)/($name=)
// 等指令,用于决定导出函数的调用约定与命名。
// g_eDllExportInfo 在 bm_mem_scan.cpp 中定义,需在编译前调用 reflush() 填充。
// ============================================================================

// ============================================================================
// x86 指令长度计算(移植自 InstructionLen.h, BSD license, oblique)
// ============================================================================
enum __bits { __b16, __b32, __b64 };

#define MOD_M       0xc0
#define RM_M        0x7
#define BASE_M      0x7
#define REX_W       0x8
#define MAX_INSN_LEN_x86 15

static int __insn_len_x86(void *insn, enum __bits bits)
{
    int len = 0, twobytes = 0, has_modrm = 0;
    enum __bits operand_bits = __b32, addr_bits = bits;
    unsigned char *c = (unsigned char*)insn, modrm, opcode;

    /* 前缀 */
    while (*c == 0xf0 || *c == 0xf2 || *c == 0xf3 ||
           *c == 0x2e || *c == 0x36 || *c == 0x3e || *c == 0x26 ||
           (*c & 0xfc) == 0x64) {
        if (*c == 0x66) operand_bits = __b16;
        if (*c == 0x67) addr_bits = (bits == __b32) ? __b16 : __b32;
        c++; len++;
    }

    if (bits == __b64 && (*c & 0xf0) == 0x40) {
        if (*c & REX_W) operand_bits = __b64;
        c++; len++;
    }

    if (*c == 0x0f) {
        twobytes = 1; c++; len++;
    } else if (*c == 0x9b &&
        ((c[1] == 0xd9 && (c[2] & MOD_M) != MOD_M && (c[2] & 0x30) == 0x30) ||
         (c[1] == 0xdb && (c[2] == 0xe2 || c[2] == 0xe3)) ||
         (c[1] == 0xdd && (c[2] & 0x30) == 0x30) ||
         (c[1] == 0xdf && c[2] == 0xe0))) {
        c++; len++;
    }

    opcode = *c++;
    len++;

    /* 使用 ModR/M 的单字节操作码 */
    if (!twobytes &&
        ((opcode & 0xf4) == 0 || (opcode & 0xf4) == 0x10 ||
         (opcode & 0xf4) == 0x20 || (opcode & 0xf4) == 0x30 ||
         opcode == 0x62 || opcode == 0x63 || opcode == 0x69 || opcode == 0x6b ||
         (opcode & 0xf0) == 0x80 || opcode == 0xc0 || opcode == 0xc1 ||
         (opcode & 0xfc) == 0xc4 || (opcode & 0xfc) == 0xd0 ||
         (opcode & 0xf8) == 0xd8 || opcode == 0xf6 || opcode == 0xf7 ||
         opcode == 0xfe || opcode == 0xff))
        has_modrm = 1;

    /* 两字节操作码 */
    if (twobytes) {
        if (!((opcode >= 0x05 && opcode <= 0x09) || opcode == 0x0b ||
              opcode == 0x0e || (opcode & 0xf8) == 0x30 || opcode == 0x77 ||
              (opcode & 0xf0) == 0x80 || (opcode >= 0xa0 && opcode <= 0xa2) ||
              (opcode >= 0xa8 && opcode <= 0xaa) || (opcode & 0xf8) == 0xc8 ||
              opcode == 0xb9))
            has_modrm = 1;
        if (opcode == 0x38 || opcode == 0x3a) { c++; len++; }
        if (opcode == 0x0f) len++;
    }

    if (has_modrm) {
        len++; modrm = *c++;
        if (addr_bits != __b16 && (modrm & (MOD_M | RM_M)) == 5) len += 4;
        if (addr_bits == __b16 && (modrm & (MOD_M | RM_M)) == 6) len += 2;
        if ((modrm & MOD_M) == 0x40) len += 1;
        if ((modrm & MOD_M) == 0x80) len += (addr_bits == __b16) ? 2 : 4;
        if (addr_bits != __b16 && (modrm & MOD_M) != MOD_M && (modrm & RM_M) == 4) {
            len++;
            if ((modrm & MOD_M) == 0 && (*c & BASE_M) == 5) len += 4;
            c++;
        }
    }

    if (!twobytes) {
        if (((opcode & 7) == 4 && (opcode & 0xf0) <= 0x30) ||
            opcode == 0x6a || opcode == 0x6b || (opcode & 0xf0) == 0x70 ||
            opcode == 0x80 || opcode == 0x82 || opcode == 0x83 ||
            opcode == 0xa8 || (opcode & 0xf8) == 0xb0 || opcode == 0xc0 ||
            opcode == 0xc1 || opcode == 0xc6 || opcode == 0xcd ||
            opcode == 0xd4 || opcode == 0xd5 || (opcode & 0xf8) == 0xe0 ||
            opcode == 0xeb || (opcode == 0xf6 && (modrm & 0x30) == 0))
            len += 1;
        if (opcode == 0xc2 || opcode == 0xca) len += 2;
        if (((opcode & 7) == 5 && (opcode & 0xf0) <= 0x30) ||
            opcode == 0x68 || opcode == 0x69 || opcode == 0x81 ||
            opcode == 0xa9 || opcode == 0xc7 || opcode == 0xe8 || opcode == 0xe9)
            len += (operand_bits == __b16) ? 2 : 4;
        if ((opcode & 0xf8) == 0xb8 || (opcode == 0xf7 && (modrm & 0x30) == 0))
            len += (operand_bits == __b16) ? 2 : (operand_bits == __b32) ? 4 : 8;
        if ((opcode & 0xfc) == 0xa0)
            len += (addr_bits == __b16) ? 2 : (addr_bits == __b32) ? 4 : 8;
        if (opcode == 0xea || opcode == 0x9a) len += 2 + ((operand_bits == __b16) ? 2 : 4);
        if (opcode == 0xc8) len += 3;
    } else {
        if ((opcode & 0xfc) == 0x70 || opcode == 0xa4 ||
            opcode == 0xac || opcode == 0xba || opcode == 0xc2 ||
            (opcode >= 0xc4 && opcode <= 0xc6))
            len += 1;
        if ((opcode & 0xf0) == 0x80) len += (operand_bits == __b16) ? 2 : 4;
        if (opcode == 0x3a) len += 1;
    }

    if (len > MAX_INSN_LEN_x86) len = 1;
    return len;
}

static int insn_len_x86_32_impl(void *insn)
{
    return __insn_len_x86(insn, __b32);
}

// ============================================================================
// 核心库命令实现函数名表(688 项,值移植自原 eLibFuncList)
// ============================================================================
static const char* const eLibFuncList[] = {
#include "bm_elibfunclist.inc"
};
static const int nLibCmdCount = (int)(sizeof(eLibFuncList) / sizeof(eLibFuncList[0]));

// ============================================================================
// 字符串分割(替代原 SplitString)
// ============================================================================
static int SplitString(const std::string& str, char split, std::vector<std::string>& strArray, bool bTrimed = false)
{
    strArray.clear();
    std::string cur;
    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == split) {
            strArray.push_back(bTrimed ? bm::trim(cur) : cur);
            cur.clear();
        } else {
            cur += str[i];
        }
    }
    strArray.push_back(bTrimed ? bm::trim(cur) : cur);
    return (int)strArray.size();
}

// 转小写(替代 CString::MakeLower)
static void makeLower(std::string& s)
{
    for (size_t i = 0; i < s.size(); i++)
        s[i] = (char)tolower((unsigned char)s[i]);
}

static bool isCoreLibraryName(const std::string& name)
{
    std::string normalized = name;
    const size_t slash = normalized.find_last_of("\\/");
    if (slash != std::string::npos) normalized.erase(0, slash + 1);
    const size_t dot = normalized.rfind('.');
    if (dot != std::string::npos) normalized.erase(dot);
    makeLower(normalized);
    return normalized == "krnln";
}

static bool isComDataTypeName(const char* englishName, const char* chineseName)
{
    std::string english = englishName ? englishName : "";
    std::string chinese = chineseName ? chineseName : "";
    makeLower(english);
    // 这些是 SDK 数据类型的语义名称，而不是某个工程函数名。
    return english == "comobject" || english == "variant" ||
        chinese.find("COM") != std::string::npos ||
        chinese.find("变体") != std::string::npos;
}

static void loadCoreCommandMetadata(
    PLIB_INFO library,
    PFN_NOTIFY_SYS notify,
    const LibDataTypeInfo* dataTypes,
    std::vector<std::string>& names,
    std::vector<unsigned char>& comFlags)
{
    names.clear();
    comFlags.clear();
    if (library == nullptr || library->m_nCmdCount <= 0 ||
        library->m_nCmdCount > 100000 || notify == nullptr) {
        return;
    }

    const INT nameAddress = notify(NL_GET_CMD_FUNC_NAMES, 0, 0);
    if (nameAddress != 0 && nameAddress != NR_ERR) {
        const auto* exportedNames = reinterpret_cast<const char* const*>(
            static_cast<std::uintptr_t>(static_cast<std::uint32_t>(nameAddress)));
        names.resize(static_cast<size_t>(library->m_nCmdCount));
        for (INT index = 0; index < library->m_nCmdCount; ++index) {
            const char* exportedName = exportedNames[index];
            if (exportedName == nullptr || IsBadStringPtrA(exportedName, 1) != FALSE) {
                continue;
            }
            names[static_cast<size_t>(index)] = exportedName;
            if (names[static_cast<size_t>(index)].front() != '_') {
                names[static_cast<size_t>(index)].insert(names[static_cast<size_t>(index)].begin(), '_');
            }
        }
    }

    comFlags.assign(static_cast<size_t>(library->m_nCmdCount), 0);
    if (dataTypes == nullptr || library->m_nDataTypeCount <= 0 ||
        library->m_nDataTypeCount > 100000) {
        return;
    }
    for (INT typeIndex = 0; typeIndex < library->m_nDataTypeCount; ++typeIndex) {
        const LibDataTypeInfo& type = dataTypes[typeIndex];
        if (!isComDataTypeName(type.m_szEgName, type.m_szName) ||
            type.m_nCmdCount <= 0 || type.m_nCmdCount > library->m_nCmdCount ||
            type.m_pnCmdsIndex == nullptr) {
            continue;
        }
        for (INT commandIndex = 0; commandIndex < type.m_nCmdCount; ++commandIndex) {
            const INT index = type.m_pnCmdsIndex[commandIndex];
            if (index >= 0 && index < library->m_nCmdCount) {
                comFlags[static_cast<size_t>(index)] = 1;
            }
        }
    }
}

static bool isEcodeRangeValid(DWORD totalSize, ULONGLONG offset,
                              ULONGLONG length)
{
    return offset <= static_cast<ULONGLONG>(totalSize) &&
           length <= static_cast<ULONGLONG>(totalSize) - offset;
}

static bool checkedSizeMul(size_t left, size_t right, size_t& result)
{
    const size_t maxSize = static_cast<size_t>(-1);
    if (right != 0 && left > maxSize / right) {
        return false;
    }
    result = left * right;
    return true;
}

static bool checkedSizeAdd(size_t left, size_t right, size_t& result)
{
    const size_t maxSize = static_cast<size_t>(-1);
    if (left > maxSize - right) {
        return false;
    }
    result = left + right;
    return true;
}

static bool checkedAlign4(DWORD rawSize, DWORD& alignedSize)
{
    const ULONGLONG value =
        (static_cast<ULONGLONG>(rawSize) + 3ULL) & ~3ULL;
    if (value > 0xFFFFFFFFULL) {
        return false;
    }
    alignedSize = static_cast<DWORD>(value);
    return true;
}

static const char* getBoundedEcodeString(const char* base, DWORD totalSize,
                                         DWORD offset, size_t& length)
{
    length = 0;
    if (base == nullptr || !isEcodeRangeValid(totalSize, offset, 0)) {
        return nullptr;
    }

    const char* start = base + offset;
    const size_t remaining = static_cast<size_t>(totalSize - offset);
    const char* end = static_cast<const char*>(memchr(start, '\0', remaining));
    if (end == nullptr) {
        return nullptr;
    }

    length = static_cast<size_t>(end - start);
    return start;
}

} // anonymous namespace


namespace bm {

// ----------------------------------------------------------------------------
// insnLenX86_32:返回 x86 32 位指令长度(头文件声明)
// ----------------------------------------------------------------------------
int insnLenX86_32(void* insn)
{
    return insn_len_x86_32_impl(insn);
}

// ============================================================================
// 构造函数:初始化 COFF 文件/段头
// ============================================================================
EcodeToObjFile::EcodeToObjFile()
{
    ::ZeroMemory(&objFileHdr, sizeof(FILEHDR));
    objFileHdr.usMagic = 0x014c;   // i386
    objFileHdr.usNumSec = 2;

    ::ZeroMemory(&objTextSec, sizeof(SECHDR));
    ::ZeroMemory(&objDataSec, sizeof(SECHDR));
    ::ZeroMemory(&objCrtSec, sizeof(SECHDR));

    strncpy(objTextSec.cName, ".text", 8);
    strncpy(objDataSec.cName, ".data", 8);
    memcpy(objCrtSec.cName, ".CRT$XIU", 8);
    objTextSec.ulFlags = 0x0020;   // STYP_TEXT 代码段
    objDataSec.ulFlags = 0x0040;   // STYP_DATA 数据段
    objCrtSec.ulFlags = IMAGE_SCN_ALIGN_4BYTES | IMAGE_SCN_CNT_INITIALIZED_DATA;

    bmLibPath.clear();
    m_pDataSection = nullptr;
    m_pCodeSection = nullptr;
    m_pCrtSection = nullptr;
    m_nVarSectionBase = 0;
    dataReLoclist = nullptr;
    codeReLoclist = nullptr;
    crtReLoclist = nullptr;
    bIsDLL = false;
    bCdecl = false;
    bIsConsole = false;
    bUseCom = false;
    dwDllMainOffset = 0;
    m_nTextSecNumRel = 0;
    coreFunctionNames.clear();
    coreComFunctionFlags.clear();
}

// ============================================================================
// 析构函数:释放各段缓冲与符号表
// ============================================================================
EcodeToObjFile::~EcodeToObjFile()
{
    if (m_pDataSection) { delete[] m_pDataSection; }
    if (m_pCodeSection) { delete[] m_pCodeSection; }
    if (m_pCrtSection)  { delete[] m_pCrtSection; }
    m_pDataSection = nullptr;
    m_pCodeSection = nullptr;
    m_pCrtSection = nullptr;

    for (size_t i = 0; i < symentList.size(); i++) {
        PLISTSYMENT p = symentList[i];
        if (p) delete p;
    }

    if (dataReLoclist) { delete[] dataReLoclist; dataReLoclist = nullptr; }
    if (codeReLoclist) { delete[] codeReLoclist; codeReLoclist = nullptr; }
    if (crtReLoclist)  { delete[] crtReLoclist;  crtReLoclist = nullptr; }

    for (size_t i = 0; i < elibInfoList.size(); i++) {
        LibInfoEntry* p = elibInfoList[i];
        if (p) delete p;
    }
    for (size_t i = 0; i < userDllTable.size(); i++) {
        DllTableSyment* p = userDllTable[i];
        if (p) delete p;
    }
    for (size_t i = 0; i < userStaticLib.size(); i++) {
        UserStaticLibInfo* p = userStaticLib[i];
        if (p) delete p;
    }
}

bool EcodeToObjFile::hasSymbolInFiles(
    const std::string& symbol,
    const std::vector<std::string>& files) const
{
    if (symbol.empty()) {
        return false;
    }

    for (const std::string& linkFile : files) {
        std::vector<std::string> candidates;
        if (linkFile.find(':') != std::string::npos ||
            linkFile[0] == '\\' || linkFile[0] == '/') {
            candidates.push_back(linkFile);
        } else {
            for (const std::string& path : paths) {
                if (!path.empty()) {
                    candidates.push_back(path + "\\" + linkFile);
                }
            }
        }

        for (const std::string& candidate : candidates) {
            HANDLE hFile = CreateFileA(candidate.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) {
                continue;
            }

            DWORD highSize = 0;
            SetLastError(ERROR_SUCCESS);
            const DWORD lowSize = GetFileSize(hFile, &highSize);
            if (lowSize == INVALID_FILE_SIZE && GetLastError() != ERROR_SUCCESS) {
                CloseHandle(hFile);
                continue;
            }
            if (highSize != 0 || lowSize < symbol.size()) {
                CloseHandle(hFile);
                continue;
            }

            std::vector<char> data(lowSize);
            DWORD bytesRead = 0;
            const BOOL readOk = ReadFile(hFile, data.data(), lowSize,
                                         &bytesRead, nullptr);
            CloseHandle(hFile);
            if (!readOk || bytesRead < symbol.size()) {
                continue;
            }

            for (DWORD i = 0; i + symbol.size() <= bytesRead; i++) {
                if (memcmp(data.data() + i, symbol.data(), symbol.size()) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool EcodeToObjFile::hasLegacyLinkSymbol(const std::string& symbol) const
{
    return hasSymbolInFiles(symbol, legacyLinkFiles);
}

bool EcodeToObjFile::hasCoreLinkSymbol(const std::string& symbol) const
{
    // 没有提供归档清单时保持旧调用方的行为：由链接器最终验证符号。
    return coreSymbolFiles.empty() || hasSymbolInFiles(symbol, coreSymbolFiles);
}

// ============================================================================
// getLibList:从易程序头中提取使用的支持库列表
// ============================================================================
bool EcodeToObjFile::getLibList(PAPP_HEADER_INFO pEcode, DWORD dwSize,
                                std::vector<std::string>& sList)
{
    sList.clear();
    if (pEcode == nullptr || dwSize < sizeof(APP_HEADER_INFO) ||
        pEcode->m_nDllCmdCount < 0) {
        return false;
    }

    size_t dllTableSize = 0;
    if (!checkedSizeMul(static_cast<size_t>(pEcode->m_nDllCmdCount),
                        sizeof(INT) * 2, dllTableSize) ||
        !isEcodeRangeValid(dwSize, static_cast<DWORD>(sizeof(APP_HEADER_INFO)),
                           dllTableSize)) {
        return false;
    }

    const char* base = reinterpret_cast<const char*>(pEcode);
    size_t stringOffset = sizeof(APP_HEADER_INFO) + dllTableSize;
    size_t stringLength = 0;
    const char* pString = getBoundedEcodeString(
        base, dwSize, static_cast<DWORD>(stringOffset), stringLength);
    if (pString == nullptr) {
        return false;
    }
    stringOffset += stringLength + 1;  // 跳过 DLL 命令表后的首个字符串

    while (stringOffset < dwSize) {
        pString = getBoundedEcodeString(
            base, dwSize, static_cast<DWORD>(stringOffset), stringLength);
        if (pString == nullptr) {
            return false;
        }
        if (stringLength == 0) {
            return true;
        }

        std::string info(pString, stringLength);
        const size_t nameEnd = info.find('\x0d');
        const size_t guidEnd = nameEnd == std::string::npos
            ? std::string::npos : info.find('\x0d', nameEnd + 1);
        const size_t majorEnd = guidEnd == std::string::npos
            ? std::string::npos : info.find('\x0d', guidEnd + 1);
        const size_t minorEnd = majorEnd == std::string::npos
            ? std::string::npos : info.find('\x0d', majorEnd + 1);
        if (nameEnd == std::string::npos || guidEnd == std::string::npos ||
            majorEnd == std::string::npos || minorEnd == std::string::npos) {
            return false;
        }

        sList.push_back(info.substr(0, nameEnd));
        stringOffset += stringLength + 1;
    }

    return false;
}

// ============================================================================
// addLibListForElib:扫描支持库 DLL 的导入表,把依赖 .lib 加入 useLibList
// ----------------------------------------------------------------------------
// 原实现用 ImageDirectoryEntryToData(依赖 imagehlp),此处改为直接读取已加载
// 模块的导入表(RVA 即地址),行为等价且无外部库依赖。
// ============================================================================
void EcodeToObjFile::addLibListForElib(HINSTANCE hInstance)
{
    if (!hInstance) return;

    PIMAGE_DOS_HEADER dosHdr = (PIMAGE_DOS_HEADER)hInstance;
    if (dosHdr->e_magic != IMAGE_DOS_SIGNATURE) return;
    PIMAGE_NT_HEADERS ntHdr = (PIMAGE_NT_HEADERS)((LPBYTE)hInstance + dosHdr->e_lfanew);
    if (ntHdr->Signature != IMAGE_NT_SIGNATURE) return;

    DWORD importRVA = ntHdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (importRVA == 0) return;

    PIMAGE_IMPORT_DESCRIPTOR pImportDesc =
        (PIMAGE_IMPORT_DESCRIPTOR)((LPBYTE)hInstance + importRVA);
    if (!pImportDesc) return;

    for (; pImportDesc->Name; pImportDesc++) {
        const char* pszDllName = (const char*)((LPBYTE)hInstance + pImportDesc->Name);
        std::string sDllName(pszDllName);
        size_t nIdx = sDllName.rfind('.');
        if (nIdx == std::string::npos) continue;
        std::string sLibName = sDllName.substr(0, nIdx);
        sLibName += ".lib";

        bool bIsIn = false;
        for (size_t i = 0; i < useLibList.size(); i++) {
            if (_stricmp(useLibList[i].c_str(), sLibName.c_str()) == 0) { bIsIn = true; break; }
        }
        if (!bIsIn &&
            _stricmp(sLibName.c_str(), "MSVCRT.lib") != 0 &&
            _stricmp(sLibName.c_str(), "MFC42.lib") != 0) {
            useLibList.push_back(sLibName);
        }
    }
}

// ============================================================================
// getClassIndex:查找支持库在 elibInfoList 中的索引
// ============================================================================
int EcodeToObjFile::getClassIndex(INT nIDX)
{
    for (size_t i = 0; i < elibInfoList.size(); i++) {
        LibInfoEntry* p = elibInfoList[i];
        if (p != nullptr && p->m_nIDX == nIDX) return (int)i;
    }
    return -1;
}

// ============================================================================
// parseECode:解析 e 代码数据,生成 .text/.data 段 + 符号表 + 重定位表
// ============================================================================
bool EcodeToObjFile::parseECode(PAPP_HEADER_INFO pECode, DWORD dwSize)
{
    auto rejectMalformedECode = [this](const char* reason) -> bool {
        m_error = bm::format("易代码数据损坏：%s", reason);
        return false;
    };

    if (pECode == nullptr || dwSize < sizeof(APP_HEADER_INFO)) {
        return rejectMalformedECode("程序头不完整");
    }
    if (pECode->m_nHeaderSize < 0 ||
        (pECode->m_nHeaderSize > 0 &&
         pECode->m_nHeaderSize < static_cast<INT>(sizeof(APP_HEADER_INFO))) ||
        (pECode->m_nHeaderSize > 0 &&
         static_cast<ULONGLONG>(pECode->m_nHeaderSize) > dwSize)) {
        return rejectMalformedECode("程序头尺寸无效");
    }
    if (pECode->m_nDllCmdCount < 0) {
        return rejectMalformedECode("DLL 命令数量无效");
    }

    size_t dllTableSize = 0;
    if (!checkedSizeMul(static_cast<size_t>(pECode->m_nDllCmdCount),
                        sizeof(INT) * 2, dllTableSize) ||
        !isEcodeRangeValid(dwSize, static_cast<DWORD>(sizeof(APP_HEADER_INFO)),
                           dllTableSize)) {
        return rejectMalformedECode("DLL 命令表超出数据范围");
    }

    PSECTION_INFO pCodeSec = nullptr;
    PSECTION_INFO pConstSec = nullptr;
    PSECTION_INFO pWinFormSec = nullptr;
    PSECTION_INFO pHelpFuncSec = nullptr;
    PSECTION_INFO pVarSec = nullptr;
    PSECTION_INFO pBeginSec = nullptr;
    auto validateSection = [&](INT offset, const char* sectionName,
                               PSECTION_INFO& section) -> bool {
        section = nullptr;
        if (offset == -1) {
            return true;
        }
        if (offset < static_cast<INT>(sizeof(APP_HEADER_INFO)) ||
            !isEcodeRangeValid(dwSize, offset, sizeof(SECTION_INFO))) {
            return rejectMalformedECode(
                bm::format("%s段头偏移无效", sectionName).c_str());
        }

        section = reinterpret_cast<PSECTION_INFO>(
            reinterpret_cast<LPBYTE>(pECode) + offset);
        if (section->m_nSectionSize < static_cast<INT>(sizeof(SECTION_INFO)) ||
            !isEcodeRangeValid(dwSize, offset,
                               static_cast<size_t>(section->m_nSectionSize)) ||
            section->m_nLoadedSize < 0 ||
            section->m_nRecordSize < 0 ||
            section->m_nRePosItemCount < 0 ||
            section->m_nExportSymbolCount < 0) {
            return rejectMalformedECode(
                bm::format("%s段尺寸或数量无效", sectionName).c_str());
        }
        if (section->m_nNextSectionOffset != -1 &&
            (section->m_nNextSectionOffset < 0 ||
             !isEcodeRangeValid(dwSize, section->m_nNextSectionOffset,
                                sizeof(SECTION_INFO)))) {
            return rejectMalformedECode(
                bm::format("%s段的下一段偏移无效", sectionName).c_str());
        }

        if (section->m_nRecordSize > 0) {
            if (section->m_nRecordOffset < 0 ||
                !isEcodeRangeValid(dwSize, section->m_nRecordOffset,
                                   static_cast<size_t>(section->m_nRecordSize))) {
                return rejectMalformedECode(
                    bm::format("%s段记录区超出数据范围", sectionName).c_str());
            }
        } else if (section->m_nRecordOffset != -1 &&
                   !isEcodeRangeValid(dwSize, section->m_nRecordOffset, 0)) {
            return rejectMalformedECode(
                bm::format("%s段记录偏移无效", sectionName).c_str());
        }

        size_t relocSize = 0;
        size_t sectionRelocEnd = 0;
        const ULONGLONG relocOffset =
            static_cast<ULONGLONG>(static_cast<DWORD>(offset)) +
            sizeof(SECTION_INFO);
        if (!checkedSizeMul(
                static_cast<size_t>(section->m_nRePosItemCount),
                sizeof(REPOSITON_INF), relocSize) ||
            !checkedSizeAdd(sizeof(SECTION_INFO), relocSize,
                            sectionRelocEnd) ||
            sectionRelocEnd > static_cast<size_t>(section->m_nSectionSize) ||
            !isEcodeRangeValid(dwSize, relocOffset, relocSize)) {
            return rejectMalformedECode(
                bm::format("%s段重定位表超出数据范围", sectionName).c_str());
        }
        return true;
    };

    if (!validateSection(pECode->m_nCodeSectionOffset, "代码", pCodeSec) ||
        !validateSection(pECode->m_nConstSectionOffset, "常量", pConstSec) ||
        !validateSection(pECode->m_nWinFormSectionOffset, "窗体", pWinFormSec) ||
        !validateSection(pECode->m_nHelpFuncSectionOffset, "辅助函数", pHelpFuncSec) ||
        !validateSection(pECode->m_nVarSectionOffset, "变量", pVarSec) ||
        !validateSection(pECode->m_nBeginSectionOffset, "首段", pBeginSec)) {
        return false;
    }
    if (pCodeSec == nullptr || pCodeSec->m_nRecordSize <= 0 ||
        pCodeSec->m_nRecordOffset < 0) {
        m_error = "易代码段不存在！";
        return false;
    }

    const ULONGLONG codeRecordEnd =
        static_cast<ULONGLONG>(static_cast<DWORD>(pCodeSec->m_nRecordOffset)) +
        static_cast<ULONGLONG>(pCodeSec->m_nRecordSize);
    if (pECode->m_nStartCodeOffset < pCodeSec->m_nRecordOffset ||
        static_cast<ULONGLONG>(static_cast<DWORD>(pECode->m_nStartCodeOffset)) >=
            codeRecordEnd) {
        return rejectMalformedECode("启动代码偏移超出代码段");
    }
    if (bIsDLL && exportFuncName.size() != exportFuncOffset.size()) {
        return rejectMalformedECode("导出函数名称和偏移数量不一致");
    }
    if (bIsDLL) {
        for (size_t i = 0; i < exportFuncOffset.size(); i++) {
            if (exportFuncOffset[i] <
                    static_cast<DWORD>(pCodeSec->m_nRecordOffset) ||
                static_cast<ULONGLONG>(exportFuncOffset[i]) >= codeRecordEnd) {
                return rejectMalformedECode("导出函数偏移超出代码段");
            }
        }
        if (dwDllMainOffset != 0 &&
            (dwDllMainOffset < static_cast<DWORD>(pCodeSec->m_nRecordOffset) ||
             static_cast<ULONGLONG>(dwDllMainOffset) >= codeRecordEnd)) {
            return rejectMalformedECode("DllMain 偏移超出代码段");
        }
    }

    // ---- 阶段 1:加载支持库信息 ----
    std::vector<std::string> aryLibList;
    if (!getLibList(pECode, dwSize, aryLibList)) {
        return rejectMalformedECode("支持库列表超出数据范围");
    }
    if (aryLibList.size() > static_cast<size_t>(0x7FFFFFFF)) {
        return rejectMalformedECode("支持库数量过大");
    }
    INT nUselib = (INT)aryLibList.size();
    if (nUselib > 0) {
        INT nClassStart = 2;

        for (INT i = 0; i < nUselib; i++) {
            if (aryLibList[i].size() >= 2 && strncmp(aryLibList[i].c_str(), "@@", 2) == 0) {
                m_error = "黑月程序不能使用易语言的COM组件封裝";
                return false;
            }

            std::string sFne = bm::format("%slib\\%s.fne", bm::g_path.eidePath.c_str(), aryLibList[i].c_str());
            HINSTANCE hInstance = LoadLibraryA(sFne.c_str());
            if (hInstance == NULL) {
                m_error = bm::format("不可能吧！不能载入支持库%s？！", sFne.c_str());
                return false;
            }
            PFN_GET_LIB_INFO getinfo = (PFN_GET_LIB_INFO)GetProcAddress(hInstance, "GetNewInf");
            if (getinfo == NULL) {
                m_error = bm::format("支持库%s没有GetNewInf导出函数（它不是一个支持库）", sFne.c_str());
                FreeLibrary(hInstance);
                return false;
            }
            PLIB_INFO pLib = (PLIB_INFO)getinfo();

            LibDataTypeInfo* dt = (LibDataTypeInfo*)pLib->m_pDataType;

            BOOL bClassHasProperty = FALSE;
            BOOL bIsWinUnit = FALSE;
            for (INT j = 0; j < pLib->m_nDataTypeCount; j++) {
                if ((dt[j].m_dwState & LDT_WIN_UNIT) == LDT_WIN_UNIT) {
                    bIsWinUnit = TRUE;
                } else {
                    if (dt[j].m_nPropertyCount > 0) {
                        bClassHasProperty = TRUE;   // 存在属性类型
                    }
                }
            }

            PFN_NOTIFY_SYS fnNotifySys = (PFN_NOTIFY_SYS)pLib->m_pfnNotify;
            LibInfoEntry* pInfo = nullptr;

            if (isCoreLibraryName(aryLibList[i])) {
                loadCoreCommandMetadata(
                    pLib,
                    fnNotifySys,
                    static_cast<const LibDataTypeInfo*>(pLib->m_pDataType),
                    coreFunctionNames,
                    coreComFunctionFlags);
            }

            if (bClassHasProperty) {
                // 有属性类型:必须有对应静态库
                std::string staticLibPath;
                bool found = false;
                for (size_t nPathIdx = 0; nPathIdx < paths.size(); nPathIdx++) {
                    if (!paths[nPathIdx].empty()) {
                        sFne = bm::format("%s\\%s_static.lib", paths[nPathIdx].c_str(), aryLibList[i].c_str());
                        if (GetFileAttributesA(sFne.c_str()) != INVALID_FILE_ATTRIBUTES) {
                            staticLibPath = sFne; found = true; break;
                        }
                    }
                }
                if (!found) {
                    m_error = bm::format("黑月程序不能使用%s.fne（%s）这个有属性的类型支持库，它没相应的静态库。请与作者联系", aryLibList[i].c_str(), pLib->m_szName ? pLib->m_szName : "");
                    FreeLibrary(hInstance);
                    return false;
                }
                (void)staticLibPath;

                pInfo = new LibInfoEntry;
                std::string str = aryLibList[i];
                pInfo->m_sLibName = aryLibList[i];
                makeLower(str);
                pInfo->m_sNotifyLibFuncName = bm::format("_%s_ProcessNotifyLib@12", str.c_str());

                pInfo->m_nIDX = nClassStart;
                pInfo->m_EClassNameList.resize(pLib->m_nDataTypeCount);
                pInfo->m_LibFuncNameList.resize(pLib->m_nCmdCount);

                for (INT j = 0; j < pLib->m_nDataTypeCount; j++) {   // 遍历每个数据类型
                    const char* peg = dt[j].m_szEgName ? dt[j].m_szEgName : "";
                    sFne = peg;
                    sFne = bm::trim(sFne);
                    if (sFne.empty()) {
                        sFne = bm::format("%3d", j);
                    } else {
                        size_t nDot = sFne.find('.');
                        if (nDot != std::string::npos) sFne[nDot] = '_';
                    }
                    pInfo->m_EClassNameList[j] = sFne;

                    for (INT nn = 0; nn < dt[j].m_nCmdCount; nn++) {
                        INT nIDX = dt[j].m_pnCmdsIndex[nn];
                        const char* peg2 = pLib->m_pBeginCmdInfo[nIDX].m_szEgName ? pLib->m_pBeginCmdInfo[nIDX].m_szEgName : "";
                        std::string sFuncName = peg2;
                        sFuncName = bm::trim(sFuncName);
                        if (sFuncName.empty()) sFuncName = bm::format("%3d", nn);
                        if (pInfo->m_LibFuncNameList[nIDX].empty()) {
                            pInfo->m_LibFuncNameList[nIDX] = bm::format("_%s_%s_%s", str.c_str(), sFne.c_str(), sFuncName.c_str());
                        } else {
                            pInfo->m_LibFuncNameList[nIDX] = bm::format("_%s_obj_%s", str.c_str(), sFuncName.c_str());
                        }
                    }

                    DWORD nProperty = 0;
                    if (dt[j].m_nPropertyCount > 0) {   // 存在属性,寻找文本属性位
                        INT nMax = dt[j].m_nPropertyCount - 1;
                        if (nMax > 31) nMax = 31;
                        for (INT xy = nMax; xy > -1; xy--) {
                            nProperty = nProperty << 1;
                            DWORD m_wState = ((LibUnitProperty*)dt[j].m_pPropertyBegin)[xy].m_shtType;
                            if ((m_wState & UD_TEXT) == UD_TEXT) m_wState = 1;
                            else m_wState = 0;
                            nProperty += m_wState;
                        }
                    }
                    pInfo->m_dwProperty.push_back(nProperty);
                }

                nClassStart++;

                // 补全未对应到的独立命令
                for (INT j = 0; j < pLib->m_nCmdCount; j++) {
                    if (pInfo->m_LibFuncNameList[j].empty()) {
                        const char* peg2 = pLib->m_pBeginCmdInfo[j].m_szEgName ? pLib->m_pBeginCmdInfo[j].m_szEgName : "";
                        std::string sFuncName = peg2;
                        sFuncName = bm::trim(sFuncName);
                        if (sFuncName.empty()) sFuncName = bm::format("%3d", j);
                        pInfo->m_LibFuncNameList[j] = bm::format("_%s_%s", str.c_str(), sFuncName.c_str());
                    }
                }
            } else {
                // 无属性类型:通过通知函数获取静态库信息
                LPSTR pStrLib = (LPSTR)fnNotifySys(NL_GET_NOTIFY_LIB_FUNC_NAME, 0, 0);
                if (::IsBadStringPtrA(pStrLib, 8)) {
                    // 此支持库不支持静态库编译,按普通支持库处理
                    pInfo = new LibInfoEntry;
                    pInfo->m_sLibName = aryLibList[i];
                    pInfo->m_sNotifyLibFuncName = bm::format("_%s_ProcessNotifyLib@12", pInfo->m_sLibName.c_str());
                } else {
                    // 通用支持库
                    pInfo = new LibInfoEntry;
                    pInfo->m_sLibName = aryLibList[i];
                    pInfo->m_sNotifyLibFuncName = bm::format("_%s@12", pStrLib);

                    char** g_CmdNames = (char**)fnNotifySys(NL_GET_CMD_FUNC_NAMES, 0, 0);
                    if ((INT)(ULONG_PTR)g_CmdNames == -1) {
                        m_error = bm::format("%s.fne对应的静态库是无效的。请与作者联系", aryLibList[i].c_str());
                        FreeLibrary(hInstance);
                        return false;
                    }
                    pInfo->m_LibFuncNameList.resize(pLib->m_nCmdCount);
                    for (INT j = 0; j < pLib->m_nCmdCount; j++)
                        pInfo->m_LibFuncNameList[j] = bm::format("_%s", g_CmdNames[j]);

                    pStrLib = (LPSTR)fnNotifySys(NL_GET_DEPENDENT_LIBS, 0, 0);
                    if (pStrLib != NULL && ::IsBadStringPtrA(pStrLib, 8) == FALSE) {
                        while (*pStrLib) {
                            pInfo->m_DependentLibList.push_back(std::string(pStrLib));
                            INT nLen = (INT)strlen(pStrLib);
                            pStrLib += (nLen + 1);
                        }
                    }
                    addLibListForElib(hInstance);
                }
                (void)bIsWinUnit;
            }

            elibInfoList.push_back(pInfo);
            FreeLibrary(hInstance);
        }
    }

    // ---- 阶段 2:提取代码段 + 启动符号 ----
    DWORD dwECPOffset;   // 启动地址偏移

    PSECTION_INFO pSec = pCodeSec;
    LPBYTE pByte = (LPBYTE)((LPBYTE)pECode + pSec->m_nRecordOffset);
    objTextSec.ulSize = static_cast<DWORD>(pSec->m_nRecordSize);
    m_pCodeSection = new BYTE[objTextSec.ulSize];
    objTextSec.ulSecOffset = sizeof(FILEHDR) + sizeof(SECHDR) * objFileHdr.usNumSec;
    memcpy(m_pCodeSection, pByte, objTextSec.ulSize);
    dwECPOffset = static_cast<DWORD>(
        pECode->m_nStartCodeOffset - pSec->m_nRecordOffset);

    addSyment("_ECodeStart", dwECPOffset, 1, false);

    if (bIsDLL) {   // 为 DLL 生成导出符号
        for (size_t i = 0; i < exportFuncName.size(); i++) {
            std::string sFuncName;
            EDllExport* pExport = g_eDllExportInfo.getByOrgName(exportFuncName[i].c_str());
            if (pExport) {   // 用户单独定义了指针调用的函数名称
                if (pExport->Cdecl == 2 || (pExport->Cdecl == 0 && bCdecl == false)) {
                    sFuncName = bm::format("_%s@%d", pExport->Name, pExport->ParamCount * 4);   // __stdcall
                } else {
                    sFuncName = bm::format("_%s", pExport->Name);   // __cdecl
                }
            } else {
                sFuncName = bm::format("_%s", exportFuncName[i].c_str());
            }
            dwECPOffset = exportFuncOffset[i] -
                          static_cast<DWORD>(pSec->m_nRecordOffset);
            addSyment(sFuncName.c_str(), dwECPOffset, 1, false);

            // 再为 cdecl 调整自身
            if (pExport) {
                if (pExport->Cdecl == 1 || (pExport->Cdecl == 0 && bCdecl))
                    makeFuncToCdecl(m_pCodeSection + dwECPOffset, (int)(objTextSec.ulSize - dwECPOffset));
            } else if (bCdecl) {
                makeFuncToCdecl(m_pCodeSection + dwECPOffset, (int)(objTextSec.ulSize - dwECPOffset));
            }
        }
        if (dwDllMainOffset) {   // 有 DllMain 入口
            dwECPOffset = dwDllMainOffset -
                          static_cast<DWORD>(pSec->m_nRecordOffset);
            addSyment("_EDllMain@12", dwECPOffset, 1, false);
        }
    }

    // ---- 阶段 3:提取常量数据 + 全局变量 ----
    LPBYTE pData = NULL;
    DWORD dwConstRecordSize = 0;
    DWORD dwDataSize = 0;
    DWORD dwDLLStart = 0;
    DWORD dwDLLEnd = 0;
    if (pConstSec != nullptr && pConstSec->m_nRecordSize > 0) {
        pData = (LPBYTE)((LPBYTE)pECode + pConstSec->m_nRecordOffset);
        dwConstRecordSize = static_cast<DWORD>(pConstSec->m_nRecordSize);
    }

    LPBYTE pVar = NULL;
    std::vector<BYTE> extendedVar;
    DWORD dwVarRecordSize = 0;
    DWORD dwVarSize = 0;
    DWORD dwVAVVarSize = 0;
    if (pVarSec != nullptr) {
        dwVarRecordSize = static_cast<DWORD>(pVarSec->m_nRecordSize);
        dwVAVVarSize = static_cast<DWORD>(pVarSec->m_nLoadedSize);
        if (dwVarRecordSize > 0) {
            pVar = (LPBYTE)((LPBYTE)pECode + pVarSec->m_nRecordOffset);
        }
        if ((pVarSec->m_dwState & SCN_EXTEND) == SCN_EXTEND) {
            if (dwVAVVarSize < dwVarRecordSize) {
                return rejectMalformedECode("扩展变量段尺寸小于记录尺寸");
            }
            extendedVar.resize(dwVAVVarSize);
            if (!extendedVar.empty()) {
                ::ZeroMemory(extendedVar.data(), extendedVar.size());
            }
            if (dwVarRecordSize > 0) {
                memcpy(extendedVar.data(), pVar, dwVarRecordSize);
            }
            pVar = extendedVar.data();
            dwVarSize = dwVAVVarSize;
        } else {
            dwVarSize = dwVarRecordSize;
        }
    }

    // 合并常量段与全局变量
    if (!checkedAlign4(dwConstRecordSize, dwDataSize) ||
        !checkedAlign4(dwVarSize, dwVarSize)) {
        return rejectMalformedECode("数据段尺寸溢出");
    }
    size_t dataSectionSize = 0;
    size_t dataSizeWithVar = 0;
    if (!checkedSizeAdd(dwDataSize, dwVarSize, dataSizeWithVar) ||
        !checkedSizeAdd(dataSizeWithVar, LIBLISTLEN, dataSectionSize) ||
        dataSectionSize > 0xFFFFFFFFULL) {
        return rejectMalformedECode("数据段尺寸溢出");
    }

    m_nVarSectionBase = dwDataSize + LIBLISTLEN;   // 全局变量基址
    objDataSec.ulSize = static_cast<DWORD>(dataSectionSize);
    objDataSec.ulSecOffset = objTextSec.ulSize + objTextSec.ulSecOffset;

    if (objDataSec.ulSize) {   // 建 DATA 段
        m_pDataSection = new BYTE[objDataSec.ulSize];
        ::ZeroMemory(m_pDataSection, objDataSec.ulSize);
    }
    if (dwConstRecordSize > 0) {   // 有常量数据
        memcpy(m_pDataSection + LIBLISTLEN, pData, dwConstRecordSize);
        if (dwDLLStart && dwDLLEnd) {
            if (dwDLLStart >= dwDLLEnd || dwDLLEnd > dwConstRecordSize) {
                return rejectMalformedECode("DLL 字符串范围无效");
            }
            memset((void*)(m_pDataSection + LIBLISTLEN + dwDLLStart), 0, dwDLLEnd - dwDLLStart);   // 清除 DLL 串
        }
    }
    if (dwVarRecordSize > 0) {   // 有全局变量数据
        memcpy(m_pDataSection + dwDataSize + LIBLISTLEN, pVar,
               dwVarRecordSize);
    }

    // CRT 段当前禁用(原项目依赖 g_setBM.bStaticLib,此处不启用)
    // objCrtSec.ulSize 保持为 0

    // ---- 阶段 4:处理 DLL 命令声明表 ----
    const INT nDLLNum = pECode->m_nDllCmdCount;
    if (nDLLNum > 0) {
        if (pData == nullptr || dwConstRecordSize == 0) {
            return rejectMalformedECode("DLL 命令表缺少常量数据");
        }
        dllIndexList.resize(nDLLNum);
        dllCallStackNum.resize(nDLLNum);
        isLibFunList.resize(nDLLNum);
        DWORD* pDllFile = (DWORD*)((LPBYTE)pECode + sizeof(APP_HEADER_INFO));
        DWORD* pDllName = pDllFile + nDLLNum;
        for (INT i = 0; i < nDLLNum; i++) {
            const DWORD dllFileOffset = *pDllFile;
            const DWORD dllNameOffset = *pDllName;
            size_t dllFileLength = 0;
            size_t dllNameLength = 0;
            const char* strDllFile = getBoundedEcodeString(
                reinterpret_cast<const char*>(pData), dwConstRecordSize,
                dllFileOffset, dllFileLength);
            const char* strDllName = getBoundedEcodeString(
                reinterpret_cast<const char*>(pData), dwConstRecordSize,
                dllNameOffset, dllNameLength);
            if (strDllFile == nullptr || strDllName == nullptr) {
                return rejectMalformedECode("DLL 名称偏移超出常量数据");
            }
            INT nIDX = 0, nStack = 0, bIsLibFun = 0;
            if (addDllSyment(strDllFile, strDllName, nIDX, nStack, bIsLibFun)) {
                if (nIDX == -1) {   // 找不到系统 DLL
                    std::string strName;
                    INT nDllIndex = (INT)userDllTable.size();
                    strName = bm::format("BMUserDll_T_%08x", nDllIndex);
                    size_t userDllDataOffset = 0;
                    if (!checkedSizeMul(userDllTable.size(), sizeof(DWORD),
                                        userDllDataOffset) ||
                        !checkedSizeAdd(objDataSec.ulSize, userDllDataOffset,
                                        userDllDataOffset) ||
                        userDllDataOffset > 0xFFFFFFFFULL) {
                        return rejectMalformedECode("用户 DLL 数据符号偏移溢出");
                    }
                    nIDX = addSyment(strName.c_str(),
                                     static_cast<DWORD>(userDllDataOffset), 2);
                    DllTableSyment* UserDllTab = new DllTableSyment();
                    UserDllTab->nSyment = nIDX;
                    UserDllTab->nOffsetDllName = static_cast<INT>(dllFileOffset);
                    UserDllTab->nOffsetFuncName = static_cast<INT>(dllNameOffset);
                    userDllTable.push_back(UserDllTab);
                }
                dllIndexList[i] = nIDX;
                dllCallStackNum[i] = nStack;
                isLibFunList[i] = bIsLibFun;
            } else {
                return false;
            }
            pDllFile++;
            pDllName++;
        }
    }

    // ---- 阶段 5:处理代码段重定位 ----
    LPBYTE pDllAddrr = NULL;
    LPBYTE pLibAddrr = NULL;
    LPBYTE pFunAddrr = NULL;
    LPBYTE pFun12 = NULL;

    pSec = pCodeSec;
    if (pSec->m_nRePosItemCount > 0) {
        size_t relocAllocSize = 0;
        if (!checkedSizeMul(
                static_cast<size_t>(pSec->m_nRePosItemCount), sizeof(RELOC),
                relocAllocSize)) {
            return rejectMalformedECode("代码重定位表尺寸溢出");
        }
        codeReLoclist = new RELOC[pSec->m_nRePosItemCount];
        PREPOSITON_INF pRel = (PREPOSITON_INF)((LPBYTE)pSec + sizeof(SECTION_INFO));
        INT nLen = pSec->m_nRePosItemCount;
        INT nIDX = 0;
        for (INT i = 0; i < pSec->m_nRePosItemCount; i++) {
            const DWORD relOffset = pRel[i].m_dwOffset;
            if (!isEcodeRangeValid(objTextSec.ulSize, relOffset,
                                   sizeof(DWORD))) {
                return rejectMalformedECode("代码重定位偏移超出代码段");
            }
            codeReLoclist[nIDX].usType = 6;   // RELOC_ADDR32
            std::string sName;
            DWORD dwOffset;
            DWORD* pOffset;
            LPBYTE pCode = NULL;

            switch (pRel[i].m_btType) {
            case RT_HELP_FUNC:
                if (relOffset < 2 ||
                    !isEcodeRangeValid(objTextSec.ulSize, relOffset - 2, 6)) {
                    return rejectMalformedECode("辅助函数重定位改写范围无效");
                }
                pOffset = (DWORD*)(m_pCodeSection + pRel[i].m_dwOffset);
                dwOffset = *pOffset / 4;   // 函数号
                *pOffset = 0;
                pCode = (LPBYTE)pOffset;
                switch (dwOffset) {
                case 0:   // 错误提示函数
                case 6:   // 分配内存函数
                case 7:   // 重分配内存函数
                case 8:   // 释放内存函数
                case 9:   // 结束程序
                case 12:  // 退出程序函数
                {
                    pCode -= 2;
                    *pCode = 0xE9;   // JMP XXXX
                    pCode++;
                    *pCode = 0;
                    pCode += 4;
                    *pCode = 0x90;   // NOP

                    if (dwOffset == 0) sName = "_E_ReportError";
                    else if (dwOffset == 6) sName = "_E_MAlloc";
                    else if (dwOffset == 7) sName = "_E_MRealloc";
                    else if (dwOffset == 8) sName = "_E_MFree";
                    else if (dwOffset == 9) sName = "_E_End";
                    else if (dwOffset == 12) {
                        sName = "_E_HelpFunc12";
                        pCode = (LPBYTE)pOffset;
                        pCode -= 2;
                        pFun12 = pCode;
                    }
                    codeReLoclist[nIDX].usType = 20;
                    codeReLoclist[nIDX].ulAddr = relOffset - 1;
                    codeReLoclist[nIDX].ulSymbol = addSyment(sName.c_str(), 0, 0, false);
                    nIDX++;
                    break;
                }
                case 1:   // 调用 DLL 命令
                case 2:   // 调用外部支持库
                case 3:   // 调用函数型命令
                    pCode -= 2;
                    if (dwOffset == 1) pDllAddrr = pCode;
                    else if (dwOffset == 2) pLibAddrr = pCode;
                    else if (dwOffset == 3) pFunAddrr = pCode;

                    *pCode = 0xE9;   // JMP XXXX
                    pCode++;
                    *pCode = 0;
                    pCode += 4;
                    *pCode = 0x90;   // NOP
                    sName = "_E_ReportError";   // 防漏调之用
                    codeReLoclist[nIDX].usType = 20;
                    codeReLoclist[nIDX].ulAddr = relOffset - 1;
                    codeReLoclist[nIDX].ulSymbol = addSyment(sName.c_str(), 0, 0, false);
                    nIDX++;
                    break;
                case 10:   // 调用事件再定义函数
                    *pOffset = 0x90909090;
                    pCode -= 2;
                    *pCode = 0xC3;   // RET
                    pCode++;
                    *pCode = 0x90;   // NOP
                    nLen--;   // 减少重定位项
                    break;
                default:
                    m_error = "黑月程序不能使用窗口单元组件";
                    return false;
                }
                break;
            case RT_CONST:
                codeReLoclist[nIDX].ulAddr = relOffset;
                pOffset = (DWORD*)(m_pCodeSection + relOffset);
                dwOffset = *pOffset;
                *pOffset = 0;
                sName = bm::format("EC%08x", dwOffset);
                codeReLoclist[nIDX].ulSymbol = addSyment(sName.c_str(), dwOffset + LIBLISTLEN, 2);
                nIDX++;
                break;
            case RT_GLOBAL_VAR:
                codeReLoclist[nIDX].ulAddr = relOffset;
                pOffset = (DWORD*)(m_pCodeSection + relOffset);
                dwOffset = *pOffset;
                dwOffset += m_nVarSectionBase;
                *pOffset = 0;
                sName = bm::format("EV%08x", dwOffset);
                codeReLoclist[nIDX].ulSymbol = addSyment(sName.c_str(), dwOffset, 2);
                nIDX++;
                break;
            case RT_CODE:
            {
                codeReLoclist[nIDX].ulAddr = relOffset;
                pOffset = (DWORD*)(m_pCodeSection + relOffset);
                dwOffset = *pOffset;
                *pOffset = 0;
                sName = bm::format("EF%08x", dwOffset);
                codeReLoclist[nIDX].ulSymbol = addSyment(sName.c_str(), dwOffset, 1, false);
                nIDX++;
                break;
            }
            default:
                return rejectMalformedECode("代码重定位类型无效");
            }
        }

        objTextSec.ulNumRel = nLen;
        m_nTextSecNumRel = nLen;   // 修正重定位数目(避免 65535 溢出)
    }

    // ---- 阶段 6:扫描代码中 DLL/库函数调用位置 ----
    if (pFun12 || pDllAddrr || pLibAddrr || pFunAddrr) {
        pSec = pCodeSec;
        if (objTextSec.ulSize < 7) {
            return rejectMalformedECode("代码段长度不足以扫描调用指令");
        }
        INT nCodeLen = static_cast<INT>(objTextSec.ulSize) - 7;
        pByte = m_pCodeSection;
        LPBYTE pCodeEnd = pByte + nCodeLen;
        DWORD dwObjCode = 0x00E80000;
        std::vector<RELOC*> DllReLoclist;
        INT dwDllNum = 0;
        INT dwFuncNum = 0;
        BOOL bIsCdeclCallOnE = FALSE;
        while (pByte <= pCodeEnd) {
            if (memcmp(&dwObjCode, pByte, 3) == 0) {
                const size_t pByteOffset =
                    static_cast<size_t>(pByte - m_pCodeSection);
                LPINT pAdrr = (LPINT)(pByte + 3);   // 0000E8XX-XX-XX-XX
                INT valAdrr = *pAdrr;
                const LONGLONG targetOffset =
                    static_cast<LONGLONG>(pByteOffset) + 7 + valAdrr;
                if (targetOffset < 0 ||
                    targetOffset >= static_cast<LONGLONG>(objTextSec.ulSize)) {
                    pByte++;
                    nCodeLen--;
                    continue;
                }
                LPBYTE curAdrr = m_pCodeSection +
                                 static_cast<size_t>(targetOffset);
                if (pDllAddrr == curAdrr) {
                    if (pByteOffset < 3 ||
                        !isEcodeRangeValid(objTextSec.ulSize,
                                           static_cast<ULONGLONG>(pByteOffset - 3),
                                           16)) {
                        return rejectMalformedECode("DLL 调用指令范围无效");
                    }
                    pAdrr = (LPINT)(pByte - 2);   // xxxx0000E8
                    valAdrr = *pAdrr;   // DLL 命令索引值
                    if (valAdrr < 0 || valAdrr >= nDLLNum ||
                        static_cast<size_t>(valAdrr) >= dllIndexList.size() ||
                        static_cast<size_t>(valAdrr) >= dllCallStackNum.size() ||
                        static_cast<size_t>(valAdrr) >= isLibFunList.size()) {
                        return rejectMalformedECode("DLL 调用索引超出 DLL 命令表");
                    }
                    // 处理代码
                    BOOL bIsCdelCall;
                    BYTE bStack = (BYTE)dllCallStackNum[valAdrr];
                    BOOL bIsLibFunc = isLibFunList[valAdrr] != 0;
                    curAdrr = pByte - 3;

                    // 检测上一条指令是否带栈字节(使用带栈字节的调用形式)
                    if (*(curAdrr + 5) == 0xE8 && *(unsigned short*)(curAdrr + 10) == 0xC483) {
                        bStack = *(curAdrr + 12);
                        bIsCdeclCallOnE = TRUE;   // 表明此时是否 CDECL 约定
                    } else {
                        bIsCdeclCallOnE = FALSE;
                    }

                    // 判断此代码是否为 CDECL(即说明表里时编译成 STDCALL,新版不再编译成此形式,改为 CDECL)
                    if (bStack > 0) bIsCdelCall = TRUE;
                    else bIsCdelCall = FALSE;

                    if (bIsLibFunc && (bIsCdeclCallOnE || !bIsCdelCall))
                        curAdrr += 5;   // 跳过 mov eax,0
                    else if ((!bIsLibFunc && (bIsCdeclCallOnE || !bIsCdelCall)) || (bIsLibFunc && !bIsCdeclCallOnE && bIsCdelCall)) {
                        // 把 mov eax,0 改成 xor eax,eax 以节省两个字节
                        *(unsigned short*)curAdrr = 0xC033;   // 33C0 xor eax,eax
                        curAdrr += 2;
                    }

                    if (bIsLibFunc) {
                        *curAdrr = 0xE8;   // call xxxxx
                        curAdrr++;
                    } else {
                        *(unsigned short*)curAdrr = 0x15FF;   // FF15 call dword ptr [..]
                        curAdrr += 2;
                    }

                    DWORD dwDllArrOffset = (DWORD)(curAdrr - m_pCodeSection);   // 地址偏移
                    pAdrr = (LPINT)curAdrr;
                    *pAdrr = 0;   // call 00000000 / call dword ptr [00000000]
                    pAdrr++;
                    if (bIsCdelCall || bIsCdeclCallOnE) {   // add esp,xx 平衡栈
                        curAdrr = (LPBYTE)pAdrr;
                        *(unsigned short*)curAdrr = 0xC483;   // add esp,xx
                        curAdrr += 2;
                        *curAdrr = bStack;
                        curAdrr++;
                        if (!bIsLibFunc) {
                            *curAdrr = 0x90;
                            curAdrr++;
                            if (bIsCdeclCallOnE) {
                                *curAdrr = 0x90;
                                curAdrr++;
                            }
                        }
                    } else {   // STDCALL
                        if (!bIsLibFunc) {
                            curAdrr = (LPBYTE)pAdrr;
                            *(unsigned short*)curAdrr = 0x9090;
                            curAdrr += 2;
                        }
                    }

                    RELOC* newReloc = new RELOC;
                    newReloc->usType = bIsLibFunc ? 20 : 6;   // 相对/绝对地址
                    newReloc->ulSymbol = dllIndexList[valAdrr];
                    newReloc->ulAddr = dwDllArrOffset;
                    DllReLoclist.push_back(newReloc);

                    pByte += 2;
                    dwDllNum++;
                } else if (pFunAddrr && pFunAddrr == curAdrr) {
                    if (pByteOffset < 2 ||
                        !isEcodeRangeValid(objTextSec.ulSize,
                                           static_cast<ULONGLONG>(pByteOffset - 2),
                                           9)) {
                        return rejectMalformedECode("核心库调用指令范围无效");
                    }
                    pAdrr = (LPINT)(pByte - 2);   // xxxx0000E8
                    valAdrr = *pAdrr / 4;   // DLL 命令索引值
                    if (valAdrr < 0 || valAdrr >= nLibCmdCount) {
                        m_error = bm::format("黑月不支持的核心库第%d号函数！", valAdrr);
                        return false;
                    }
                    std::string resolvedFunctionName;
                    if (static_cast<size_t>(valAdrr) < coreFunctionNames.size()) {
                        resolvedFunctionName = coreFunctionNames[static_cast<size_t>(valAdrr)];
                    }
                    // 只有归档实际包含的符号才可进入 OBJ。这样可以在
                    // 新旧核心归档之间自动选择名称，不把 FNE 的声明误当成
                    // 黑月已经提供了实现。
                    if (!resolvedFunctionName.empty() &&
                        !hasCoreLinkSymbol(resolvedFunctionName)) {
                        resolvedFunctionName.clear();
                    }
                    // 对没有实现命令名通知的旧核心 FNE，保留历史表作为
                    // 兼容输入；新版本优先使用 FNE 自己导出的名称。
                    if (resolvedFunctionName.empty() && valAdrr < nLibCmdCount &&
                        eLibFuncList[valAdrr] != nullptr &&
                        hasCoreLinkSymbol(eLibFuncList[valAdrr])) {
                        resolvedFunctionName = eLibFuncList[valAdrr];
                    }
                    if (resolvedFunctionName.empty()) {
                        const std::string declaredName =
                            static_cast<size_t>(valAdrr) < coreFunctionNames.size()
                            ? coreFunctionNames[static_cast<size_t>(valAdrr)] : std::string();
                        if (!declaredName.empty()) {
                            m_error = bm::format(
                                "黑月核心归档未提供核心库命令%d的实现“%s”",
                                valAdrr,
                                declaredName.c_str());
                        } else {
                            m_error = bm::format("黑月不支持的核心库第%d号函数！", valAdrr);
                        }
                        return false;
                    }
                    if (static_cast<size_t>(valAdrr) < coreComFunctionFlags.size() &&
                        coreComFunctionFlags[static_cast<size_t>(valAdrr)] != 0) {
                        bUseCom = true;
                    }

                    curAdrr = pByte + 3;
                    DWORD dwDllArrOffset = (DWORD)(curAdrr - m_pCodeSection);
                    pAdrr = (LPINT)curAdrr;
                    *pAdrr = 0;

                    RELOC* newReloc = new RELOC;
                    newReloc->usType = 20;   // 相对地址
                    newReloc->ulSymbol = addSyment(resolvedFunctionName.c_str(), 0, 0, false);
                    newReloc->ulAddr = dwDllArrOffset;
                    DllReLoclist.push_back(newReloc);

                    pByte += 2;
                    dwFuncNum++;
                } else if (pFun12 && pFun12 == curAdrr) {
                    if (pByteOffset < 2 ||
                        !isEcodeRangeValid(objTextSec.ulSize,
                                           static_cast<ULONGLONG>(pByteOffset - 2),
                                           9)) {
                        return rejectMalformedECode("辅助函数调用指令范围无效");
                    }
                    pAdrr = (LPINT)(pByte - 2);   // xxxx0000E8
                    valAdrr = *pAdrr;   // 辅助函数值

                    if (valAdrr == 0 || valAdrr == 1 || valAdrr == 3 || valAdrr == 2) {
                        std::string cFuncName;
                        if (valAdrr == 0) cFuncName = "_E_FindFile";
                        else if (valAdrr == 1) cFuncName = "_E_CloseFindFile";
                        else if (valAdrr == 3) cFuncName = "_E_Destroy";
                        else if (valAdrr == 2) cFuncName = "_E_CloneConstArray";

                        curAdrr = pByte + 3;
                        DWORD dwDllArrOffset = (DWORD)(curAdrr - m_pCodeSection);
                        pAdrr = (LPINT)curAdrr;
                        *pAdrr = 0;

                        RELOC* newReloc = new RELOC;
                        newReloc->usType = 20;
                        newReloc->ulSymbol = addSyment(cFuncName.c_str(), 0, 0, false);
                        newReloc->ulAddr = dwDllArrOffset;
                        DllReLoclist.push_back(newReloc);
                    } else if (valAdrr == 4 || valAdrr == 5) {
                        // 4=读对象属性取值,5=写对象属性赋值
                        std::string cFuncName;
                        if (valAdrr == 4) cFuncName = "_BlackMoonGetClassPropertyVaule";
                        else if (valAdrr == 5) cFuncName = "_BlackMoonSetClassPropertyVaule";

                        curAdrr = pByte + 3;
                        DWORD dwDllArrOffset = (DWORD)(curAdrr - m_pCodeSection);
                        pAdrr = (LPINT)curAdrr;
                        *pAdrr = 0;

                        RELOC* newReloc = new RELOC;
                        newReloc->usType = 20;
                        newReloc->ulSymbol = addSyment(cFuncName.c_str(), 0, 0, false);
                        newReloc->ulAddr = dwDllArrOffset;
                        DllReLoclist.push_back(newReloc);

                        // 取类号和函数号
                        if (pByteOffset < 7 ||
                            !isEcodeRangeValid(objTextSec.ulSize,
                                               static_cast<ULONGLONG>(pByteOffset - 7),
                                               14)) {
                            return rejectMalformedECode("属性调用指令范围无效");
                        }
                        pAdrr = (LPINT)(pByte - 7);   // xxxx0000E8
                        INT nObjLibIdx = *pAdrr;   // 对象索引值
                        INT nCLibIdx = nObjLibIdx >> 16;
                        INT nLibClassIdx = nObjLibIdx & 0x0000FFFF;
                        nLibClassIdx--;
                        nObjLibIdx = getClassIndex(nCLibIdx);
                        if (nObjLibIdx < 0 ||
                            static_cast<size_t>(nObjLibIdx) >= elibInfoList.size()) {
                            m_error = bm::format("黑月检测到易代码有异常，在调用不存在的%d号有属性类型支持库！", nCLibIdx);
                            return false;
                        }
                        LibInfoEntry* pLibInf = elibInfoList[nObjLibIdx];
                        if (pLibInf == nullptr || nLibClassIdx < 0 ||
                            static_cast<size_t>(nLibClassIdx) >= pLibInf->m_EClassNameList.size() ||
                            static_cast<size_t>(nLibClassIdx) >= pLibInf->m_dwProperty.size()) {
                            const char* libName = pLibInf != nullptr
                                ? pLibInf->m_sLibName.c_str() : "未知";
                            m_error = bm::format("黑月检测到易代码有异常，在调用库%s不存在的%d号类型！", libName, nLibClassIdx);
                            return false;
                        }
                        *(DWORD*)pAdrr = pLibInf->m_dwProperty[nLibClassIdx];   // 写入属性是否文本类型的信息
                        std::string str = pLibInf->m_sLibName;
                        makeLower(str);
                        if (valAdrr == 4)
                            cFuncName = bm::format("_%s_%s_GetPropertyVaule@12", str.c_str(), pLibInf->m_EClassNameList[nLibClassIdx].c_str());
                        else if (valAdrr == 5)
                            cFuncName = bm::format("_%s_%s_SetPropertyVaule@16", str.c_str(), pLibInf->m_EClassNameList[nLibClassIdx].c_str());

                        pAdrr = (LPINT)(pByte - 2);   // xxxx0000E8
                        *pAdrr = 0;
                        dwDllArrOffset = (DWORD)((LPBYTE)pAdrr - m_pCodeSection);

                        newReloc = new RELOC;
                        newReloc->usType = 6;   // 绝对地址
                        newReloc->ulSymbol = addSyment(cFuncName.c_str(), 0, 0, false);
                        newReloc->ulAddr = dwDllArrOffset;
                        DllReLoclist.push_back(newReloc);
                        pLibInf->m_bIsUse = true;
                    }

                    pByte += 2;
                    dwFuncNum++;
                } else if (pLibAddrr && pLibAddrr == curAdrr) {
                    if (pByteOffset < 7 ||
                        !isEcodeRangeValid(objTextSec.ulSize,
                                           static_cast<ULONGLONG>(pByteOffset - 7),
                                           14)) {
                        return rejectMalformedECode("支持库调用指令范围无效");
                    }
                    pAdrr = (LPINT)(pByte - 7);   // xxxx0000E8
                    valAdrr = *pAdrr / 4;   // 命令索引值

                    pAdrr = (LPINT)(pByte - 2);   // xxxx0000E8
                    INT nLibIndex = *pAdrr;   // 库索引,从 1 开始
                    if (valAdrr < 0 || nLibIndex <= 0 ||
                        nLibIndex > nUselib ||
                        static_cast<size_t>(nLibIndex - 1) >= elibInfoList.size()) {
                        m_error = bm::format("黑月检测到易代码有异常，在调用不存在的%d号支持库！", nLibIndex);
                        return false;
                    }
                    *pAdrr = 0;
                    LibInfoEntry* pLibInf = elibInfoList[nLibIndex - 1];

                    if (pLibInf == nullptr ||
                        static_cast<size_t>(valAdrr) >= pLibInf->m_LibFuncNameList.size()) {
                        const char* libName = pLibInf != nullptr
                            ? pLibInf->m_sLibName.c_str() : "未知";
                        m_error = bm::format("黑月检测到易代码有异常，在调用支持库%s中不存在的%d号函数或者该支持库不支持静态库编译", libName, valAdrr);
                        return false;
                    }
                    const char* cFuncName = pLibInf->m_LibFuncNameList[valAdrr].c_str();
                    if (cFuncName == nullptr) {   // 库里改成删除的函数名
                        m_error = bm::format("支持库%s的%d号函数名称没有导出", pLibInf->m_sLibName.c_str(), valAdrr);
                        return false;
                    }

                    DWORD dwDllArrOffset = (DWORD)((LPBYTE)pAdrr - m_pCodeSection);
                    RELOC* newReloc = new RELOC;
                    newReloc->usType = 6;   // 绝对地址
                    newReloc->ulSymbol = addSyment(cFuncName, 0, 0, false);
                    newReloc->ulAddr = dwDllArrOffset;
                    DllReLoclist.push_back(newReloc);

                    // 处理代码
                    curAdrr = pByte + 3;
                    dwDllArrOffset = (DWORD)(curAdrr - m_pCodeSection);
                    pAdrr = (LPINT)curAdrr;
                    *pAdrr = 0;

                    newReloc = new RELOC;
                    newReloc->usType = 20;   // 相对地址
                    newReloc->ulSymbol = addSyment("_BlackMoonCalleLibFunctionHelper", 0, 0, false);
                    newReloc->ulAddr = dwDllArrOffset;
                    DllReLoclist.push_back(newReloc);

                    pByte += 2;
                    dwFuncNum++;
                    pLibInf->m_bIsUse = true;
                }
                pByte += 2;
            }
            pByte++;
            nCodeLen--;
        }

        const size_t codeRelocCount = DllReLoclist.size();
        size_t totalRelocCount = 0;
        size_t totalRelocSize = 0;
        if (m_nTextSecNumRel < 0 ||
            !checkedSizeAdd(static_cast<size_t>(m_nTextSecNumRel),
                            codeRelocCount, totalRelocCount) ||
            totalRelocCount > static_cast<size_t>(0x7FFFFFFF) ||
            !checkedSizeMul(totalRelocCount, sizeof(RELOC), totalRelocSize)) {
            return rejectMalformedECode("代码重定位合并数量溢出");
        }
        nCodeLen = static_cast<INT>(codeRelocCount);
        if (nCodeLen) {
            INT nReloc = static_cast<INT>(totalRelocCount);   // 修正写入项目(避免 65535 溢出)
            RELOC* pNewReLoc = new RELOC[nReloc];
            if (codeReLoclist && m_nTextSecNumRel > 0) {
                memcpy(pNewReLoc, codeReLoclist, m_nTextSecNumRel * sizeof(RELOC));
                delete[] codeReLoclist;   // 释放原来
                codeReLoclist = pNewReLoc;
                pNewReLoc += m_nTextSecNumRel;
            } else {
                codeReLoclist = pNewReLoc;
            }
            objTextSec.ulNumRel = nReloc;
            m_nTextSecNumRel = nReloc;
            for (INT i = 0; i < nCodeLen; i++) {
                RELOC* pReloc = DllReLoclist[i];
                memcpy(pNewReLoc, pReloc, sizeof(RELOC));   // 追加数据
                pNewReLoc++;
                delete pReloc;
            }
        }
    }

    // ---- 阶段 7:数据段重定位 + 支持库通知表 ----
    addSyment("_BlackMoonFuncForeLib", 0, 2);
    addSyment("_BlackMoonCalleLibList", sizeof(INT), 2);

    if (nUselib > 0) {   // 扣除未使用的库
        INT nTest = nUselib;
        for (INT i = 0; i < nUselib; i++) {
            if (static_cast<size_t>(i) >= elibInfoList.size() ||
                elibInfoList[i] == nullptr) {
                return rejectMalformedECode("支持库信息列表索引无效");
            }
            LibInfoEntry* p = elibInfoList[i];
            if (p->m_bIsUse == false) nTest--;
        }
        nUselib = nTest;
    }

    const INT nConstRelocCount = pConstSec != nullptr
        ? pConstSec->m_nRePosItemCount : 0;
    if (nConstRelocCount > 0 || nUselib > 0) {
        size_t nLenSize = static_cast<size_t>(nConstRelocCount);
        if (nUselib > 0 &&
            (!checkedSizeAdd(nLenSize, static_cast<size_t>(nUselib), nLenSize) ||
             !checkedSizeAdd(nLenSize, 1, nLenSize))) {
            return rejectMalformedECode("数据重定位数量溢出");
        }
        if (nLenSize > static_cast<size_t>(0x7FFFFFFF)) {
            return rejectMalformedECode("数据重定位数量过大");
        }
        size_t dataRelocAllocSize = 0;
        if (!checkedSizeMul(nLenSize, sizeof(RELOC), dataRelocAllocSize)) {
            return rejectMalformedECode("数据重定位表尺寸溢出");
        }

        const INT nLen = static_cast<INT>(nLenSize);
        dataReLoclist = new RELOC[nLen];
        PREPOSITON_INF pRel = pConstSec != nullptr
            ? (PREPOSITON_INF)((LPBYTE)pConstSec + sizeof(SECTION_INFO))
            : nullptr;
        INT nStart = 0;

        // 支持库函数前置库通知表
        if (nUselib > 0) {
            dataReLoclist[0].usType = 6;   // RELOC_ADDR32
            dataReLoclist[0].ulAddr = 0;
            dataReLoclist[0].ulSymbol = addSyment("_BlackMoonFuncForeLibNotifySys@12", 0, 0, false);
            for (INT i = 0; i < (INT)aryLibList.size(); i++) {
                if (static_cast<size_t>(i) >= elibInfoList.size() ||
                    elibInfoList[i] == nullptr) {
                    return rejectMalformedECode("支持库信息列表索引无效");
                }
                LibInfoEntry* p = elibInfoList[i];
                if (p->m_bIsUse) {
                    if (nStart + 1 >= nLen) {
                        return rejectMalformedECode("支持库通知重定位数量无效");
                    }
                    dataReLoclist[nStart + 1].usType = 6;
                    dataReLoclist[nStart + 1].ulAddr = sizeof(INT) + nStart * sizeof(INT);
                    dataReLoclist[nStart + 1].ulSymbol = addSyment(p->m_sNotifyLibFuncName.c_str(), 0, 0, false);
                    nStart++;
                }
            }
            nStart = nUselib + 1;
        }

        for (INT i = 0; i < nConstRelocCount; i++) {
            if (pRel == nullptr ||
                !isEcodeRangeValid(dwConstRecordSize, pRel[i].m_dwOffset,
                                   sizeof(DWORD))) {
                return rejectMalformedECode("常量段重定位偏移超出数据范围");
            }
            dataReLoclist[i + nStart].usType = 6;
            std::string sName;
            DWORD dwOffset;
            DWORD* pOffset;

            switch (pRel[i].m_btType) {
            case RT_CONST:
            {
                dataReLoclist[i + nStart].ulAddr = pRel[i].m_dwOffset + LIBLISTLEN;
                pOffset = (DWORD*)(m_pDataSection + pRel[i].m_dwOffset + LIBLISTLEN);
                dwOffset = *pOffset;
                *pOffset = 0;
                sName = bm::format("ECC%08x", dwOffset);
                dataReLoclist[i + nStart].ulSymbol = addSyment(sName.c_str(), dwOffset + LIBLISTLEN, 2);
                break;
            }
            case RT_CODE:
            {
                dataReLoclist[i + nStart].ulAddr = pRel[i].m_dwOffset + LIBLISTLEN;
                pOffset = (DWORD*)(m_pDataSection + pRel[i].m_dwOffset + LIBLISTLEN);
                dwOffset = *pOffset;
                *pOffset = 0;
                sName = bm::format("ECF%08x", dwOffset);
                dataReLoclist[i + nStart].ulSymbol = addSyment(sName.c_str(), dwOffset, 1, false);
                break;
            }
            default:
                m_error = bm::format("晕~~有没有搞错啊，常量段定位到%d段？！", pRel[i].m_btType);
                return false;
            }
        }
        objDataSec.ulNumRel = nLen;
    }

    // ---- 用户源代码 DLL 调用桩生成 ----
    if (userDllTable.size() > static_cast<size_t>(0x7FFFFFFF)) {
        return rejectMalformedECode("用户 DLL 数量过大");
    }
    INT nUserDllFunc = (INT)userDllTable.size();
    if (nUserDllFunc > 0) {
        size_t codeExtensionSize = 0;
        size_t newCodeSectionSize = 0;
        size_t addCodeRelocCount = 0;
        size_t addCodeRelocSize = 0;
        size_t addDataRelocSize = 0;
        if (!checkedSizeMul(static_cast<size_t>(nUserDllFunc), 22,
                            codeExtensionSize) ||
            codeExtensionSize > static_cast<size_t>(0x7FFFFFFF) ||
            !checkedSizeAdd(objTextSec.ulSize, codeExtensionSize,
                            newCodeSectionSize) ||
            newCodeSectionSize > 0xFFFFFFFFULL ||
            !checkedSizeMul(static_cast<size_t>(nUserDllFunc), 4,
                            addCodeRelocCount) ||
            addCodeRelocCount > static_cast<size_t>(0x7FFFFFFF) ||
            !checkedSizeMul(addCodeRelocCount, sizeof(RELOC),
                            addCodeRelocSize) ||
            !checkedSizeMul(static_cast<size_t>(nUserDllFunc), sizeof(RELOC),
                            addDataRelocSize)) {
            return rejectMalformedECode("用户 DLL 桩尺寸溢出");
        }

        const INT nNexLen = static_cast<INT>(codeExtensionSize);
        const INT nAddCodeRel = static_cast<INT>(addCodeRelocCount);
        LPBYTE pNewCode = new BYTE[newCodeSectionSize];
        memcpy(pNewCode, m_pCodeSection, objTextSec.ulSize);
        pByte = pNewCode + objTextSec.ulSize;
        BYTE btCode[] = {104, 0, 0, 0, 0, 104, 0, 0, 0, 0, 104, 0, 0, 0, 0, 232, 0, 0, 0, 0, 255, 224};
        // push 0 / push 0 / push 0 / call ff / jmp eax

        RELOC* AddCodeReLoc = new RELOC[nAddCodeRel];   // 追加代码段重定位
        RELOC* AddDataReLoc = new RELOC[nUserDllFunc];  // 追加数据段重定位
        INT nSCodeRel = 0;
        INT nCallDllFunc = addSyment("_BlackMoonCallUserDllFunc@12", 0, 0, false);
        for (INT i = 0; i < nUserDllFunc; i++) {
            memcpy(pByte, btCode, 22);
            DllTableSyment* pTemp = userDllTable[i];

            AddCodeReLoc[nSCodeRel].usType = 6;
            AddCodeReLoc[nSCodeRel].ulSymbol = pTemp->nSyment;
            AddCodeReLoc[nSCodeRel].ulAddr = objTextSec.ulSize + 1 + i * 22;
            nSCodeRel++;

            std::string strName = bm::format("BMUserDll_N_%08x", i);
            AddCodeReLoc[nSCodeRel].usType = 6;
            AddCodeReLoc[nSCodeRel].ulSymbol = addSyment(strName.c_str(), pTemp->nOffsetFuncName + LIBLISTLEN, 2);
            AddCodeReLoc[nSCodeRel].ulAddr = objTextSec.ulSize + 6 + i * 22;
            nSCodeRel++;

            strName = bm::format("BMUserDll_D_%08x", i);
            AddCodeReLoc[nSCodeRel].usType = 6;
            AddCodeReLoc[nSCodeRel].ulSymbol = addSyment(strName.c_str(), pTemp->nOffsetDllName + LIBLISTLEN, 2);
            AddCodeReLoc[nSCodeRel].ulAddr = objTextSec.ulSize + 11 + i * 22;
            nSCodeRel++;

            AddCodeReLoc[nSCodeRel].usType = 20;   // 相对地址
            AddCodeReLoc[nSCodeRel].ulSymbol = nCallDllFunc;
            AddCodeReLoc[nSCodeRel].ulAddr = objTextSec.ulSize + 16 + i * 22;
            nSCodeRel++;

            pByte += 22;

            strName = bm::format("BMUserDll_F_%08x", i);
            AddDataReLoc[i].usType = 6;
            AddDataReLoc[i].ulSymbol = addSyment(strName.c_str(), objTextSec.ulSize + i * 22, 1, false);
            AddDataReLoc[i].ulAddr = objDataSec.ulSize + i * sizeof(DWORD);
        }
        // 合并代码段重定位
        size_t newTextRelocCount = 0;
        size_t newTextRelocSize = 0;
        if (!checkedSizeAdd(static_cast<size_t>(nAddCodeRel),
                            static_cast<size_t>(m_nTextSecNumRel),
                            newTextRelocCount) ||
            newTextRelocCount > static_cast<size_t>(0x7FFFFFFF) ||
            !checkedSizeMul(newTextRelocCount, sizeof(RELOC),
                            newTextRelocSize)) {
            delete[] AddCodeReLoc;
            delete[] AddDataReLoc;
            delete[] pNewCode;
            return rejectMalformedECode("代码重定位合并尺寸溢出");
        }
        INT nNewLen = static_cast<INT>(newTextRelocCount);
        RELOC* pTempRel = new RELOC[nNewLen];
        if (codeReLoclist)
            memcpy(pTempRel, codeReLoclist, sizeof(RELOC) * m_nTextSecNumRel);
        memcpy(pTempRel + m_nTextSecNumRel, AddCodeReLoc, sizeof(RELOC) * nAddCodeRel);
        objTextSec.ulNumRel = nNewLen;
        m_nTextSecNumRel = nNewLen;
        delete[] AddCodeReLoc;
        if (codeReLoclist) delete[] codeReLoclist;
        codeReLoclist = pTempRel;

        // 合并数据段重定位
        size_t newDataRelocCount = 0;
        size_t newDataRelocSize = 0;
        if (!checkedSizeAdd(static_cast<size_t>(nUserDllFunc),
                            static_cast<size_t>(objDataSec.ulNumRel),
                            newDataRelocCount) ||
            newDataRelocCount > static_cast<size_t>(0x7FFFFFFF) ||
            !checkedSizeMul(newDataRelocCount, sizeof(RELOC),
                            newDataRelocSize)) {
            delete[] AddDataReLoc;
            return rejectMalformedECode("数据重定位合并尺寸溢出");
        }
        nNewLen = static_cast<INT>(newDataRelocCount);
        pTempRel = new RELOC[nNewLen];
        if (dataReLoclist)
            memcpy(pTempRel, dataReLoclist, sizeof(RELOC) * objDataSec.ulNumRel);
        memcpy(pTempRel + objDataSec.ulNumRel, AddDataReLoc, sizeof(RELOC) * nUserDllFunc);
        objDataSec.ulNumRel = nNewLen;
        delete[] AddDataReLoc;
        if (dataReLoclist) delete[] dataReLoclist;
        dataReLoclist = pTempRel;

        // 扩展代码段与数据段
        delete[] m_pCodeSection;
        m_pCodeSection = pNewCode;
        objTextSec.ulSize = objTextSec.ulSize + nNexLen;

        size_t newDataSectionSize = 0;
        if (!checkedSizeMul(static_cast<size_t>(nUserDllFunc), sizeof(DWORD),
                            newDataSectionSize) ||
            !checkedSizeAdd(objDataSec.ulSize, newDataSectionSize,
                            newDataSectionSize) ||
            newDataSectionSize > 0xFFFFFFFFULL) {
            return rejectMalformedECode("用户 DLL 数据段尺寸溢出");
        }
        pNewCode = new BYTE[newDataSectionSize];
        ::ZeroMemory(pNewCode, newDataSectionSize);
        if (m_pDataSection) {
            memcpy(pNewCode, m_pDataSection, objDataSec.ulSize);
            delete[] m_pDataSection;
        }
        m_pDataSection = pNewCode;
        objTextSec.ulSize = static_cast<DWORD>(newCodeSectionSize);
        objDataSec.ulSize = static_cast<DWORD>(newDataSectionSize);
        objDataSec.ulSecOffset = objTextSec.ulSize + objTextSec.ulSecOffset;
        objCrtSec.ulSecOffset = objDataSec.ulSize + objDataSec.ulSecOffset;
    }

    return true;
}

// ============================================================================
// addSyment:添加符号到符号表(自动去重)
// ============================================================================
int EcodeToObjFile::addSyment(const char* strName, DWORD ulValue, DWORD iSection, bool bIsVar)
{
    for (size_t i = 0; i < symentList.size(); i++) {
        PLISTSYMENT curSyment = symentList[i];
        if (strlen(curSyment->cName) != strlen(strName)) continue;
        if (strcmp(curSyment->cName, strName) == 0)   // 重复则返回已有索引
            return (int)i;
    }

    PLISTSYMENT newSyment = new LISTSYMENT;
    ::ZeroMemory(newSyment, sizeof(LISTSYMENT));
    strncpy(newSyment->cName, strName, 255);
    newSyment->cName[255] = 0;
    newSyment->syment.ulValue = ulValue;
    newSyment->syment.iSection = (short)iSection;
    if (!bIsVar) newSyment->syment.usType = 0x20;   // 为函数
    newSyment->syment.usClass = 2;   // 外部定义存储类
    symentList.push_back(newSyment);
    return (int)(symentList.size() - 1);
}

// ============================================================================
// saveObjFile:写出 COFF OBJ 文件
// ============================================================================
bool EcodeToObjFile::saveObjFile(const std::string& strPath)
{
    FILE *fp = fopen(strPath.c_str(), "wb");
    if (fp == NULL) return false;

    if (objCrtSec.ulSize) {
        objTextSec.ulRelOffset = objCrtSec.ulSize + objCrtSec.ulSecOffset;   // 接 CRT 段
    } else {
        objTextSec.ulRelOffset = objDataSec.ulSize + objDataSec.ulSecOffset;   // 接数据段
    }

    if (m_nTextSecNumRel > 0xFFFF)
        objDataSec.ulRelOffset = objTextSec.ulRelOffset + (m_nTextSecNumRel + 1) * sizeof(RELOC);   // TEXT 段重定位溢出 65535,需多一个段
    else
        objDataSec.ulRelOffset = objTextSec.ulRelOffset + m_nTextSecNumRel * sizeof(RELOC);

    if (objCrtSec.ulSize > 0) {
        objCrtSec.ulRelOffset = objDataSec.ulRelOffset + objDataSec.ulNumRel * sizeof(RELOC);   // 接 DATA 段重定位
        objFileHdr.ulSymbolOffset = objCrtSec.ulRelOffset + objCrtSec.ulNumRel * sizeof(RELOC);   // 接 CRT 段重定位
    } else {
        objFileHdr.ulSymbolOffset = objDataSec.ulRelOffset + objDataSec.ulNumRel * sizeof(RELOC);   // 接 DATA 段重定位
    }

    objFileHdr.ulNumSymbol = (unsigned long)symentList.size();   // 符号表成员数
    DWORD SymentCharTableLen = 4;   // 字符串表长度和紧跟其后

    for (size_t i = 0; i < symentList.size(); i++) {   // 对应符号偏移
        PLISTSYMENT curSyment = symentList[i];
        INT nLen = (INT)strlen(curSyment->cName);
        nLen++;
        curSyment->syment.e.ulOffset = SymentCharTableLen;
        SymentCharTableLen += nLen;
    }

    fwrite(&objFileHdr, sizeof(FILEHDR), 1, fp);   // 写文件头
    fflush(fp);

    if (m_nTextSecNumRel > 0xFFFF) {
        objTextSec.ulNumRel = 0xFFFF;
        objTextSec.ulFlags |= IMAGE_SCN_LNK_NRELOC_OVFL;
    }
    fwrite(&objTextSec, sizeof(SECHDR), 1, fp);   // 写代码段头
    fflush(fp);
    fwrite(&objDataSec, sizeof(SECHDR), 1, fp);   // 写数据段头
    fflush(fp);

    if (objCrtSec.ulSize > 0) {
        fwrite(&objCrtSec, sizeof(SECHDR), 1, fp);   // 写 CRT 段头
        fflush(fp);
    }

    if (m_pCodeSection) { fwrite(m_pCodeSection, objTextSec.ulSize, 1, fp); fflush(fp); }   // 写代码段数据
    if (m_pDataSection) { fwrite(m_pDataSection, objDataSec.ulSize, 1, fp); fflush(fp); }   // 写数据段数据
    if (m_pCrtSection)  { fwrite(m_pCrtSection, objCrtSec.ulSize, 1, fp); fflush(fp); }    // 写 CRT 段数据

    if (codeReLoclist && m_nTextSecNumRel > 0) {   // 写代码段重定位
        if (m_nTextSecNumRel > 0xFFFF) {
            RELOC rel2[1];
            rel2[0].ulAddr = m_nTextSecNumRel + 1;
            rel2[0].usType = 0;
            rel2[0].ulSymbol = 0;
            fwrite(rel2, sizeof(RELOC), 1, fp);
            fflush(fp);
        }
        fwrite(codeReLoclist, m_nTextSecNumRel * sizeof(RELOC), 1, fp);
        fflush(fp);
    }
    if (dataReLoclist && objDataSec.ulNumRel) {
        fwrite(dataReLoclist, objDataSec.ulNumRel * sizeof(RELOC), 1, fp);   // 写数据段重定位
        fflush(fp);
    }
    if (crtReLoclist && objCrtSec.ulNumRel) {
        fwrite(crtReLoclist, objCrtSec.ulNumRel * sizeof(RELOC), 1, fp);   // 写 CRT 段重定位
        fflush(fp);
    }

    INT SymentLen = (INT)symentList.size();
    if (SymentLen > 0) {   // 写符号表与字符串表
        PLISTSYMENT curSyment;
        for (INT i = 0; i < SymentLen; i++) {   // 写符号表
            curSyment = symentList[i];
            fwrite(&curSyment->syment, sizeof(SYMENT), 1, fp);
            fflush(fp);
        }
        fwrite(&SymentCharTableLen, sizeof(DWORD), 1, fp);   // 写字符串表长度
        fflush(fp);
        for (INT i = 0; i < SymentLen; i++) {   // 写字符串表
            curSyment = symentList[i];
            INT nLen = (INT)strlen(curSyment->cName);
            nLen++;
            fwrite(curSyment->cName, nLen, 1, fp);
            fflush(fp);
        }
    }

    fclose(fp);
    return true;
}

// ============================================================================
// makeDebugFile:生成调试静态库使用的 DEF/OBJ
// ============================================================================
// 调试器会把静态库调用按 cdecl 形式转发到“调试静态库.dll”。这里为每个
// 带参数的 cdecl 静态库函数生成一个 47 字节的转发桩，并用 COFF 重定位
// 把桩连接到真正的静态库函数。该格式与 BlackMoon 3.54 保持兼容。
// ============================================================================
bool EcodeToObjFile::makeDebugFile(const std::string& strBasePath) const
{
    const std::string defPath = strBasePath + ".def";
    const std::string objPath = strBasePath + ".obj";

    FILE* def = fopen(defPath.c_str(), "wb");
    if (def == nullptr) {
        return false;
    }

    const std::string defHeader =
        "LIBRARY " DEBUGDLL "\r\n\r\nEXPORTS\r\n\r\n";
    fwrite(defHeader.data(), defHeader.size(), 1, def);

    int wrapperCount = 0;
    for (const UserStaticLibInfo* item : userStaticLib) {
        if (item == nullptr) {
            continue;
        }

        std::string exportName;
        if (item->bIsCdelCall) {
            if (item->nStack > 0) {
                exportName = bm::format("%s@@%d", item->cName,
                                        item->nStack);
                ++wrapperCount;
            } else {
                exportName = bm::format("%s@@0 = %s", item->cName,
                                        item->cName);
            }
        } else {
            exportName = item->cName;
        }
        exportName += "\r\n";
        fwrite(exportName.data(), exportName.size(), 1, def);
    }
    fclose(def);

    if (wrapperCount == 0) {
        DeleteFileA(defPath.c_str());
        return false;
    }

    struct DebugSymbol {
        SYMENT entry = {};
        std::string name;
    };

    std::vector<DebugSymbol> symbols;
    std::vector<RELOC> relocations;
    std::vector<unsigned char> text(
        static_cast<size_t>(wrapperCount) * 47U, 0);

    auto addSymbol = [&symbols](const std::string& name, DWORD value,
                                short section) -> DWORD {
        for (size_t i = 0; i < symbols.size(); ++i) {
            if (symbols[i].name == name) {
                return static_cast<DWORD>(i);
            }
        }

        DebugSymbol symbol;
        symbol.name = name;
        symbol.entry.ulValue = value;
        symbol.entry.iSection = section;
        symbol.entry.usClass = 2; // IMAGE_SYM_CLASS_EXTERNAL
        symbols.push_back(std::move(symbol));
        return static_cast<DWORD>(symbols.size() - 1);
    };

    static const unsigned char stubTemplate[47] = {
        86, 87, 83, 141, 116, 36, 16, 129, 236, 0, 0, 0, 0,
        137, 231, 252, 185, 0, 0, 0, 0, 243, 165, 232, 0, 0,
        0, 0, 129, 196, 0, 0, 0, 0, 91, 95, 94, 89, 129,
        196, 0, 0, 0, 0, 81, 195, 144
    };

    size_t wrapperIndex = 0;
    for (const UserStaticLibInfo* item : userStaticLib) {
        if (item == nullptr || !item->bIsCdelCall || item->nStack <= 0) {
            continue;
        }

        const size_t codeOffset = wrapperIndex * 47U;
        memcpy(text.data() + codeOffset, stubTemplate, sizeof(stubTemplate));
        const int stack = item->nStack;
        const int argumentCount = stack / 4;
        memcpy(text.data() + codeOffset + 9, &stack, sizeof(stack));
        memcpy(text.data() + codeOffset + 17, &argumentCount,
               sizeof(argumentCount));
        memcpy(text.data() + codeOffset + 30, &stack, sizeof(stack));
        memcpy(text.data() + codeOffset + 40, &stack, sizeof(stack));

        const std::string wrapperName =
            bm::format("%s@@%d", item->cName, item->nStack);
        addSymbol(wrapperName, static_cast<DWORD>(codeOffset), 1);
        std::string targetName = item->cName;
        if (targetName.empty() || targetName.front() != '_') {
            targetName.insert(targetName.begin(), '_');
        }
        const DWORD targetSymbol = addSymbol(targetName, 0, 0);

        RELOC relocation = {};
        relocation.ulAddr = static_cast<DWORD>(codeOffset + 24);
        relocation.ulSymbol = targetSymbol;
        relocation.usType = 20; // IMAGE_REL_I386_REL32
        relocations.push_back(relocation);
        ++wrapperIndex;
    }

    FILEHDR fileHeader = {};
    fileHeader.usMagic = 0x014c;
    fileHeader.usNumSec = 1;
    fileHeader.ulNumSymbol = static_cast<unsigned long>(symbols.size());

    SECHDR textSection = {};
    memcpy(textSection.cName, ".text", 5);
    textSection.ulSize = static_cast<unsigned long>(text.size());
    textSection.ulSecOffset = sizeof(FILEHDR) + sizeof(SECHDR);
    textSection.ulRelOffset = textSection.ulSecOffset + textSection.ulSize;
    textSection.ulNumRel = static_cast<unsigned short>(relocations.size());
    textSection.ulFlags = 0x0020; // IMAGE_SCN_CNT_CODE
    fileHeader.ulSymbolOffset = textSection.ulRelOffset +
                                relocations.size() * sizeof(RELOC);

    DWORD stringTableLength = 4;
    for (DebugSymbol& symbol : symbols) {
        symbol.entry.e.ulZero = 0;
        symbol.entry.e.ulOffset = stringTableLength;
        stringTableLength += static_cast<DWORD>(symbol.name.size() + 1);
    }

    FILE* obj = fopen(objPath.c_str(), "wb");
    if (obj == nullptr) {
        DeleteFileA(defPath.c_str());
        return false;
    }

    fwrite(&fileHeader, sizeof(fileHeader), 1, obj);
    fwrite(&textSection, sizeof(textSection), 1, obj);
    fwrite(text.data(), text.size(), 1, obj);
    fwrite(relocations.data(), relocations.size() * sizeof(RELOC), 1, obj);
    for (const DebugSymbol& symbol : symbols) {
        fwrite(&symbol.entry, sizeof(symbol.entry), 1, obj);
    }
    fwrite(&stringTableLength, sizeof(stringTableLength), 1, obj);
    for (const DebugSymbol& symbol : symbols) {
        fwrite(symbol.name.data(), symbol.name.size() + 1, 1, obj);
    }
    const bool ok = ferror(obj) == 0;
    fclose(obj);
    if (!ok) {
        DeleteFileA(objPath.c_str());
        DeleteFileA(defPath.c_str());
        return false;
    }
    return true;
}

// ============================================================================
// addDllSyment:处理 DLL 命令声明,生成对应 COFF 符号
// ============================================================================
bool EcodeToObjFile::addDllSyment(const char* strLibName, const char* strFuncName,
                                  INT& nIDX, INT& nStack, INT& bIsLibfun, INT nDefaultStack)
{
    std::vector<std::string> strDllFiles;
    std::string strDllFile;
    std::string strSubName = bm::trim(std::string(strFuncName));

    int nCount = SplitString(std::string(strLibName), ',', strDllFiles, true);
    if (nCount > 0) {
        strDllFile = strDllFiles[0];
    }
    int nLen = (int)strDllFile.size();
    BOOL bIsIndexFunc = FALSE;
    BOOL bIsCdelCall = FALSE;
    nStack = nDefaultStack;
    bIsLibfun = 0;

    if (strFuncName[0] == '#') bIsIndexFunc = TRUE;

    // 检测是否为 __cdecl 调用方式
    size_t atPos = strSubName.find("@@");
    int nStart = (atPos == std::string::npos) ? -1 : (int)atPos;
    if (nStart > 0) {
        bIsCdelCall = TRUE;
        nStart += 2;
        std::string sNum = strSubName.substr(nStart);
        size_t nstrLen = sNum.size();
        size_t i;
        for (i = 0; i < nstrLen; i++) {
            if (sNum[i] < '0' || sNum[i] > '9') break;
        }
        if (i < nstrLen) {
            m_error = bm::format("DLL“%s”子程序的__cdel调用方式的堆栈字节数部分有非数字字符", strFuncName);
            return false;
        }
        nStack = atoi(sNum.c_str());
        if (nStack > 255) {
            m_error = bm::format("DLL“%s”子程序的__cdel调用方式的堆栈字节数超过范围", strFuncName);
            return false;
        }
        sNum = strSubName.substr(0, nStart - 2);
        strSubName = sNum;
    } else if (!strSubName.empty() && strSubName[0] == '@') {   // @_xxxxx
        bIsCdelCall = TRUE;
        std::string sNum = strSubName.substr(1);
        strSubName = sNum;
    }

    std::string strMBLibName;
    std::string strMBFuncName;
    int nTestLen = nLen;
    const bool isLegacyStaticLib =
        _stricmp(strDllFile.c_str(), "静态库") == 0;

    if (nLen == 0) {
        if (bIsIndexFunc) {
            m_error = bm::format("系统DLL不要用“%s”的函数序号", strFuncName);
            return false;
        }
        const char* sList[] = {"Advapi32.dll", "Kernel32.dll", "User32.dll", "Gdi32.dll", "Mpr.dll", "Shell32.dll", NULL};
        BOOL bFind = FALSE;
        for (int i = 0; i < 6; i++) {
            HINSTANCE hInstance = LoadLibraryA(sList[i]);
            if (hInstance == NULL) {
                m_error = bm::format("你这什么系统？！居然没有%s文件", sList[i]);
                return false;
            }
            if (GetProcAddress(hInstance, strSubName.c_str())) {
                FreeLibrary(hInstance);
                bFind = TRUE;
                strDllFile = sList[i];
                break;
            }
            FreeLibrary(hInstance);
        }
        if (!bFind) {
            m_error = bm::format("%s不是默认系统DLL的函数，黑月程序不能调用", strFuncName);
            return false;
        }
    } else {
        // 检测是否静态库模式
        size_t posLib = strDllFile.find(".lib");
        nStart = (posLib == std::string::npos) ? -1 : (int)posLib;
        if (nStart < 0) {
            size_t posObj = strDllFile.find(".obj");
            nStart = (posObj == std::string::npos) ? -1 : (int)posObj;
        }
        // 旧版易语言用“静态库”作为占位文件名,实际链接项写在同名
        // .ini 的 [Link]/Opt 中。按真正的静态库函数处理,避免退化为
        // 运行时 DLL 调用或直接拒绝编译。
        if (nStart >= 0 || isLegacyStaticLib) {
            bIsLibfun = 1;
            // CDEL 调用方式,不需要改变
            if (bIsCdelCall == FALSE) {
                size_t atPos2 = strSubName.find('@');
                int nStart2 = (atPos2 == std::string::npos) ? -1 : (int)atPos2;
                if (nStart2 > 0) {
                    nStart2++;
                    std::string sNum = strSubName.substr(nStart2);
                    size_t nstrLen = sNum.size();
                    size_t i;
                    for (i = 0; i < nstrLen; i++) {
                        if (sNum[i] < '0' || sNum[i] > '9') break;
                    }
                    if (i < nstrLen) {
                        m_error = bm::format("静态库“%s”子程序的__stdcall调用方式的堆栈字节数部分有非数字字符", strFuncName);
                        return false;
                    }
                } else {
                    bIsCdelCall = TRUE;   // 此时应认定为 CDECL 调用约定
                }
            }
            if (isLegacyStaticLib && !bIsCdelCall) {
                // 旧版静态库的 stdcall 符号命名不统一:WaveObjectLib
                // 使用双下划线,MSVC 编译的 ufmod.obj 使用单下划线。
                const std::string standardName =
                    (strSubName.empty() || strSubName[0] != '_')
                    ? bm::format("_%s", strSubName.c_str())
                    : strSubName;
                const std::string alternateName = "_" + standardName;
                strMBFuncName = hasLegacyLinkSymbol(alternateName)
                    ? alternateName
                    : standardName;
            } else if (strSubName.empty() || strSubName[0] != '_') {
                // cdecl 静态库(如 zlib)使用标准单下划线。
                strMBFuncName = bm::format("_%s", strSubName.c_str());
            } else {
                strMBFuncName = strSubName;
            }

            nIDX = addSyment(strMBFuncName.c_str(), 0, 0, false);

            // 加入静态库列表(去重)
            for (size_t i = 0; i < userStaticLib.size(); i++) {
                UserStaticLibInfo* p = userStaticLib[i];
                if (strcmp(p->cName, strSubName.c_str()) == 0 && p->bIsCdelCall == bIsCdelCall && p->nStack == nStack)
                    return true;
            }
            UserStaticLibInfo* pUserLib = new UserStaticLibInfo();
            pUserLib->bIsCdelCall = bIsCdelCall;
            pUserLib->nStack = nStack;
            strncpy(pUserLib->cName, strSubName.c_str(), 255);
            pUserLib->cName[255] = 0;
            userStaticLib.push_back(pUserLib);

            // 加入使用的静态库列表(去重)
            BOOL bNotFound = TRUE;
            for (size_t i = 0; i < useLibList.size(); i++) {
                if (_stricmp(useLibList[i].c_str(), strDllFile.c_str()) == 0) { bNotFound = FALSE; break; }
            }
            // “静态库”只是占位符,不能把它作为实际文件传给 link.exe。
            if (bNotFound && !isLegacyStaticLib) {
                useLibList.push_back(strDllFile);
            }

            // 其它库也需要加进去(去重)
            for (int i = 1; i < nCount; i++) {
                bNotFound = TRUE;
                for (size_t j = 0; j < useLibList.size(); j++) {
                    if (_stricmp(useLibList[j].c_str(), strDllFiles[i].c_str()) == 0) { bNotFound = FALSE; break; }
                }
                if (bNotFound) useLibList.push_back(strDllFiles[i]);
            }
            return true;
        }
        // 寻找扩展名
        size_t rpos = strDllFile.rfind('.');
        if (rpos == std::string::npos) {   // 没有扩展名,认作 DLL,且认作为静态库
            nIDX = -1;
            return true;
        }
        if (bIsIndexFunc) {
            nIDX = -1;
            return true;
        }
    }

    // CDEL 调用方式,不需要改变
    if (bIsCdelCall) {
        strMBFuncName = bm::format("__imp__%s", strSubName.c_str());   // 函数
    } else {
        size_t atPos2 = strSubName.find('@');
        int nStart2 = (atPos2 == std::string::npos) ? -1 : (int)atPos2;
        if (nStart2 > 0) {
            nStart2++;
            std::string sNum = strSubName.substr(nStart2);
            size_t nstrLen = sNum.size();
            size_t i;
            for (i = 0; i < nstrLen; i++) {
                if (sNum[i] < '0' || sNum[i] > '9') break;
            }
            if (i < nstrLen)
                strMBFuncName = bm::format("__imp__%s@", strSubName.c_str());   // 带有@但无栈
            else
                strMBFuncName = bm::format("__imp__%s", strSubName.c_str());   // 带有@栈
        } else {
            strMBFuncName = bm::format("__imp__%s@", strSubName.c_str());   // 函数
        }
    }

    HANDLE hFileLib = INVALID_HANDLE_VALUE;
    strMBLibName.clear();
    // 在各目录中寻找
    for (size_t nPathIdx = 0; nPathIdx < paths.size(); nPathIdx++) {
        if (!paths[nPathIdx].empty()) {
            strMBLibName = bm::format("%s\\%slib", paths[nPathIdx].c_str(), strDllFile.substr(0, strDllFile.size() - 3).c_str());
            hFileLib = CreateFileA(strMBLibName.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_ARCHIVE, 0);
            if (hFileLib != INVALID_HANDLE_VALUE) break;
        }
    }
    if (hFileLib == INVALID_HANDLE_VALUE) {
        if (nTestLen == 0) {   // 此情况不可能找不到相关 LIB 文件
            m_error = bm::format("找不到与函数%s相关静态库文件%s，请报告缺失的文件", strFuncName, strMBLibName.c_str());
            return false;
        }
        nIDX = -1;
        return true;
    }
    DWORD highSize = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD lowSize = GetFileSize(hFileLib, &highSize);
    if ((lowSize == INVALID_FILE_SIZE && GetLastError() != ERROR_SUCCESS) ||
        highSize != 0 || lowSize == 0 || lowSize > static_cast<DWORD>(INT_MAX)) {
        CloseHandle(hFileLib);
        nIDX = -1;
        return nTestLen != 0;
    }
    nLen = static_cast<int>(lowSize);
    std::unique_ptr<char[]> libData(new (std::nothrow) char[nLen]);
    if (!libData) {
        CloseHandle(hFileLib);
        nIDX = -1;
        return false;
    }
    DWORD dwNumOfByteRead = 0;
    LPSTR lpLibData = libData.get();
    const BOOL readOk = ReadFile(hFileLib, lpLibData, lowSize,
                                 &dwNumOfByteRead, nullptr);
    CloseHandle(hFileLib);
    if (!readOk || dwNumOfByteRead != lowSize) {
        nIDX = -1;
        return nTestLen != 0;
    }

    const char* strName = strMBFuncName.c_str();
    const char* strData = lpLibData;
    INT nCmpLen = (INT)strlen(strName);
    nLen -= nCmpLen;

    while (nLen > 0) {
        if (memcmp(strName, strData, nCmpLen) == 0) {
            // 加入符号表
            nIDX = addSyment(strData, 0, 0, false);

            // 加入使用的静态库列表
            strMBLibName = bm::format("%slib", strDllFile.substr(0, strDllFile.size() - 3).c_str());
            for (size_t i = 0; i < useLibList.size(); i++) {
                if (_stricmp(useLibList[i].c_str(), strMBLibName.c_str()) == 0)
                    return true;
            }
            useLibList.push_back(strMBLibName);
            return true;
        }
        strData++;
        nLen--;
    }

    nIDX = -1;
    if (nTestLen == 0) {
        m_error = bm::format("在静态库文件%s中找不到函数%s，请报告这个错误", strMBLibName.c_str(), strFuncName);
        return false;
    }
    return true;
}

// ============================================================================
// loadEProgram:读取易程序 PE 文件,提取 e 代码数据段
// ============================================================================
bool EcodeToObjFile::loadEProgram(const std::string& strEFilePath, LPBYTE& lpRAWData, DWORD& dwSize)
{
    lpRAWData = NULL;
    dwSize = 0;
    bIsDLL = false;
    bIsConsole = false;
    dwDllMainOffset = 0;
    exportFuncName.clear();
    exportFuncOffset.clear();

    HANDLE hFile = CreateFileA(strEFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        m_error = bm::format("不能打开文件“%s”", strEFilePath.c_str());
        return false;
    }

    LARGE_INTEGER fileLength;
    ::ZeroMemory(&fileLength, sizeof(fileLength));
    if (!GetFileSizeEx(hFile, &fileLength) || fileLength.QuadPart <= 0 ||
        fileLength.QuadPart > 0xFFFFFFFFLL) {
        CloseHandle(hFile);
        m_error = bm::format("不能读取文件“%s”的尺寸", strEFilePath.c_str());
        return false;
    }
    const DWORD fileSize = static_cast<DWORD>(fileLength.QuadPart);
    LPBYTE pMem = new (std::nothrow) BYTE[fileSize];
    if (pMem == nullptr) {
        CloseHandle(hFile);
        m_error = bm::format("载入文件“%s”时内存不足", strEFilePath.c_str());
        return false;
    }
    DWORD bytesRead = 0;
    BOOL bRet = ReadFile(hFile, pMem, fileSize, &bytesRead, 0);
    CloseHandle(hFile);

    auto failLoad = [&](const char* message) -> bool {
        if (lpRAWData != nullptr) {
            delete[] lpRAWData;
            lpRAWData = nullptr;
        }
        delete[] pMem;
        m_error = message;
        return false;
    };

    if (bRet == FALSE || bytesRead != fileSize) {
        return failLoad(bRet == FALSE ? "不能载入文件" : "文件内容不完整");
    }

    if (!isEcodeRangeValid(fileSize, 0, sizeof(IMAGE_DOS_HEADER))) {
        return failLoad("文件头不完整");
    }

    PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(pMem);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew < 0) {
        return failLoad("不是有效的PE文件");
    }

    const ULONGLONG ntOffset = static_cast<ULONGLONG>(dosHeader->e_lfanew);
    const size_t ntPrefixSize = SIZE_OF_NT_SIGNATURE + sizeof(IMAGE_FILE_HEADER);
    if (!isEcodeRangeValid(fileSize, ntOffset, ntPrefixSize)) {
        return failLoad("PE文件头不完整");
    }

    LPBYTE ntBase = pMem + dosHeader->e_lfanew;
    if (*reinterpret_cast<const DWORD*>(ntBase) != IMAGE_NT_SIGNATURE) {
        return failLoad("不是有效的PE文件");
    }

    PIMAGE_FILE_HEADER fileHeader = reinterpret_cast<PIMAGE_FILE_HEADER>(
        ntBase + SIZE_OF_NT_SIGNATURE);
    if (fileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER)) {
        return failLoad("PE可选头不完整");
    }

    size_t ntHeaderSize = 0;
    if (!checkedSizeAdd(ntPrefixSize, fileHeader->SizeOfOptionalHeader,
                        ntHeaderSize) ||
        !isEcodeRangeValid(fileSize, ntOffset, ntHeaderSize)) {
        return failLoad("PE文件头超出文件范围");
    }

    PIMAGE_NT_HEADERS pNTHeader = reinterpret_cast<PIMAGE_NT_HEADERS>(ntBase);
    if (pNTHeader->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return failLoad("PE文件不是32位程序");
    }

    size_t sectionTableOffset = 0;
    size_t sectionTableSize = 0;
    if (!checkedSizeAdd(static_cast<size_t>(dosHeader->e_lfanew),
                        ntHeaderSize, sectionTableOffset) ||
        !checkedSizeMul(static_cast<size_t>(fileHeader->NumberOfSections),
                        sizeof(IMAGE_SECTION_HEADER), sectionTableSize) ||
        !isEcodeRangeValid(fileSize, sectionTableOffset, sectionTableSize)) {
        return failLoad("PE节表超出文件范围");
    }

    PIMAGE_SECTION_HEADER section = reinterpret_cast<PIMAGE_SECTION_HEADER>(
        pMem + sectionTableOffset);
    DWORD dwEcodeVirtualAddress = 0;

    // 易语言不同版本的 IDE/链接器会把易代码节标记为
    // RWX(0xE0000040) 或 RW(0xC0000040)。两者都可能是 .data 节，
    // 不能只按旧版的精确属性匹配。
    for (WORD i = 0; i < fileHeader->NumberOfSections; i++) {
        if (section[i].SizeOfRawData > 0 &&
            !isEcodeRangeValid(fileSize, section[i].PointerToRawData,
                               section[i].SizeOfRawData)) {
            return failLoad("PE节数据超出文件范围");
        }
        const DWORD ecodeCharacteristics = section[i].Characteristics;
        const bool isEcodeSection =
            (ecodeCharacteristics & 0xc0000040u) == 0xc0000040u &&
            (ecodeCharacteristics & 0x00000020u) == 0;
        if (isEcodeSection) {
            if (section[i].SizeOfRawData == 0 || lpRAWData != nullptr) {
                continue;
            }
            dwEcodeVirtualAddress = section[i].VirtualAddress;
            dwSize = section[i].SizeOfRawData;
            lpRAWData = new (std::nothrow) BYTE[dwSize];
            if (lpRAWData == nullptr) {
                return failLoad("提取易代码时内存不足");
            }
            memcpy(lpRAWData, pMem + section[i].PointerToRawData, dwSize);
        }
    }

    auto rvaToFileRange = [&](DWORD rva, ULONGLONG length,
                              DWORD& fileOffset) -> bool {
        const DWORD headersSize = pNTHeader->OptionalHeader.SizeOfHeaders;
        if (headersSize > 0 &&
            isEcodeRangeValid(fileSize, rva, length) &&
            static_cast<ULONGLONG>(rva) + length <= headersSize) {
            fileOffset = rva;
            return true;
        }

        for (WORD i = 0; i < fileHeader->NumberOfSections; i++) {
            const ULONGLONG sectionStart = section[i].VirtualAddress;
            const ULONGLONG rawSize = section[i].SizeOfRawData;
            if (static_cast<ULONGLONG>(rva) < sectionStart ||
                static_cast<ULONGLONG>(rva) - sectionStart > rawSize) {
                continue;
            }
            const ULONGLONG relative = static_cast<ULONGLONG>(rva) - sectionStart;
            if (length > rawSize - relative) {
                continue;
            }
            const ULONGLONG rawOffset =
                static_cast<ULONGLONG>(section[i].PointerToRawData) + relative;
            if (rawOffset > 0xFFFFFFFFULL ||
                !isEcodeRangeValid(fileSize, rawOffset, length)) {
                continue;
            }
            fileOffset = static_cast<DWORD>(rawOffset);
            return true;
        }
        return false;
    };

    auto getRvaString = [&](DWORD rva, const char*& value,
                            size_t& length) -> bool {
        DWORD fileOffset = 0;
        if (!rvaToFileRange(rva, 1, fileOffset)) {
            return false;
        }
        value = getBoundedEcodeString(reinterpret_cast<const char*>(pMem),
                                      fileSize, fileOffset, length);
        return value != nullptr;
    };

    if ((pNTHeader->FileHeader.Characteristics & IMAGE_FILE_DLL) == IMAGE_FILE_DLL) {
        bIsDLL = true;
        if (lpRAWData == nullptr ||
            pNTHeader->OptionalHeader.NumberOfRvaAndSizes <=
                IMAGE_DIRECTORY_ENTRY_EXPORT) {
            return failLoad("不能获取DLL的函数导出表目录");
        }

        const IMAGE_DATA_DIRECTORY& exportDirectory =
            pNTHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        DWORD exportOffset = 0;
        if (exportDirectory.VirtualAddress == 0 ||
            exportDirectory.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
            !rvaToFileRange(exportDirectory.VirtualAddress,
                            sizeof(IMAGE_EXPORT_DIRECTORY), exportOffset)) {
            return failLoad("不能获取DLL的函数导出表目录偏移");
        }

        PIMAGE_EXPORT_DIRECTORY ped = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(
            pMem + exportOffset);
        if (ped->NumberOfNames > ped->NumberOfFunctions) {
            return failLoad("DLL导出表名称数量无效");
        }

        size_t namesSize = 0;
        size_t functionsSize = 0;
        if (!checkedSizeMul(static_cast<size_t>(ped->NumberOfNames),
                            sizeof(DWORD), namesSize) ||
            !checkedSizeMul(static_cast<size_t>(ped->NumberOfFunctions),
                            sizeof(DWORD), functionsSize)) {
            return failLoad("DLL导出表尺寸溢出");
        }

        DWORD namesOffset = 0;
        DWORD functionsOffset = 0;
        if ((namesSize > 0 &&
             (ped->AddressOfNames == 0 ||
              !rvaToFileRange(ped->AddressOfNames, namesSize, namesOffset))) ||
            (functionsSize > 0 &&
             (ped->AddressOfFunctions == 0 ||
              !rvaToFileRange(ped->AddressOfFunctions, functionsSize,
                              functionsOffset)))) {
            return failLoad("DLL导出表数组超出文件范围");
        }

        const DWORD* names = namesSize > 0
            ? reinterpret_cast<const DWORD*>(pMem + namesOffset) : nullptr;
        const DWORD* functions = functionsSize > 0
            ? reinterpret_cast<const DWORD*>(pMem + functionsOffset) : nullptr;
        for (DWORD i = 0; i < ped->NumberOfNames; i++) {
            DWORD nameRva = 0;
            DWORD functionRva = 0;
            memcpy(&nameRva, names + i, sizeof(nameRva));
            memcpy(&functionRva, functions + i, sizeof(functionRva));

            const char* pSrc = nullptr;
            size_t nameLength = 0;
            if (!getRvaString(nameRva, pSrc, nameLength)) {
                return failLoad("DLL导出函数名称超出文件范围");
            }
            if (functionRva < dwEcodeVirtualAddress || dwSize < 5 ||
                static_cast<ULONGLONG>(functionRva) - dwEcodeVirtualAddress >
                    static_cast<ULONGLONG>(dwSize) - 5) {
                return failLoad("DLL导出函数地址超出代码段");
            }

            const DWORD codeOffset = functionRva - dwEcodeVirtualAddress;
            LPBYTE pCode = lpRAWData + codeOffset;
            if (pCode[0] != 0xB8) {
                std::string strFuncName(pSrc, nameLength);
                return failLoad(bm::format("不能找到导出函数“%s”的地址",
                                           strFuncName.c_str()).c_str());
            }

            DWORD exportOffsetValue = 0;
            memcpy(&exportOffsetValue, pCode + 1, sizeof(exportOffsetValue));
            std::string strFuncName(pSrc, nameLength);
            if (_stricmp(strFuncName.c_str(), "Dll入口函数") == 0) {
                dwDllMainOffset = exportOffsetValue;
            } else {
                exportFuncOffset.push_back(exportOffsetValue);
                exportFuncName.push_back(strFuncName);
            }
        }
    } else if ((pNTHeader->OptionalHeader.Subsystem & IMAGE_SUBSYSTEM_WINDOWS_CUI) ==
               IMAGE_SUBSYSTEM_WINDOWS_CUI) {
        bIsConsole = true;
    }

    delete[] pMem;

    if (lpRAWData) return true;
    m_error = bm::format("不能从“%s”中提取易代码", strEFilePath.c_str());
    return false;
}
// ============================================================================
// makeFuncToCdecl:将 __stdcall 函数改为 __cdecl(RET n -> RET + 填充)
// ----------------------------------------------------------------------------
// 遍历指令直到遇到 RET(0xC3, cdecl)或 RET imm16(0xC2, stdcall),
// 把 stdcall 的 RET n 改写为 RET(0xC3) + int3(0xCC) 填充。
// ============================================================================
void EcodeToObjFile::makeFuncToCdecl(LPBYTE pStart, int nLen)
{
    if ((uintptr_t)pStart < 0xFFFF || nLen <= 0) return;

    int nTLen = 0;
    for (int i = 0; i < 1000; i++) {
        BYTE bIns = *(BYTE*)pStart;
        if (0xC2 == bIns) {   // RET imm16 (stdcall)
            *(WORD*)pStart = 0xCCC3;
            *(BYTE*)((uintptr_t)pStart + 2) = 0xCC;
            return;
        } else if (0xC3 == bIns) {   // RET (cdecl)
            return;
        }
        int nInsLen = insnLenX86_32((void*)pStart);
        if (nInsLen <= 0) return;
        pStart = (LPBYTE)((uintptr_t)pStart + nInsLen);
        nTLen += nInsLen;
        if (nTLen >= nLen) return;
    }
}

} // namespace bm

#endif  // defined(_M_IX86)
