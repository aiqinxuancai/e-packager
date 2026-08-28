// ============================================================================
// bm_ecode_to_obj.h - 易代码到 COFF OBJ 转换模块
// ============================================================================
// 本模块将易语言编译器(e.exe)生成的中间数据文件(PE 格式,内含 e 代码段)
// 解析并转换为 COFF 格式的 OBJ 文件,供 link.exe 链接为独立 EXE/DLL。
//
// 核心流程:
//   1. LoadEProgram()  - 读取 PE 文件,提取 e 代码数据段
//   2. ParseECode()    - 解析 e 代码结构,生成 .text/.data 段 + 符号表 + 重定位表
//   3. SaveObjFile()   - 写出 COFF OBJ 文件(BlackMoon.obj)
//
// 数据结构来源:
//   APP_HEADER_INFO / SECTION_INFO / REPOSITON_INF: 易代码段头信息
//   FILEHDR / SECHDR / RELOC / SYMENT: COFF OBJ 文件格式结构
// ============================================================================
#ifndef __BM_ECODE_TO_OBJ_H__
#define __BM_ECODE_TO_OBJ_H__

#include <windows.h>
#include <string>
#include <vector>

namespace bm {

// ============================================================================
// 易代码数据结构(从 elib_sdk.h / EcodeToObjFile.h 移植)
// ============================================================================

// 重定位信息条目(位域结构)
typedef struct {
    unsigned m_btType : 3;     // 重定位类型(RT_HELP_FUNC/RT_CONST/RT_GLOBAL_VAR/RT_CODE)
    unsigned m_dwOffset : 29;  // 指向代码段中某 INT 位置的偏移
} REPOSITON_INF, *PREPOSITON_INF;

#define  RT_HELP_FUNC    0    // 基于接口函数数据段内数据的地址重定位
#define  RT_CONST        1    // 基于常量数据段内数据的地址重定位
#define  RT_GLOBAL_VAR   2    // 基于全局变量数据段内数据的地址重定位
#define  RT_CODE         3    // 基于代码段内数据的地址重定位

// 数据段信息
typedef struct {
    INT m_nSectionSize;       // 段信息尺寸
    INT m_nNextSectionOffset; // 下一段的偏移(无下一段则为 -1)
    DWORD m_dwState;          // 段状态标志
    #define SCN_READ         (1 << 0)
    #define SCN_WRITE        (1 << 1)
    #define SCN_EXECUTE      (1 << 2)
    #define SCN_DISCARDABLE  (1 << 3)
    #define SCN_EXTEND       (1 << 4)
    #define MAX_SECTION_NAME_LEN 20
    char m_szName[MAX_SECTION_NAME_LEN + 4];  // 段名
    INT m_nLoadedSize;        // 加载尺寸
    INT m_nRecordSize;        // 记录尺寸(文件中实际数据尺寸)
    INT m_nRecordOffset;      // 数据偏移(无记录则为 -1)
    INT m_nRePosItemCount;    // 需重定位的偏移 INT 数目
    INT m_nExportSymbolCount; // 导出符号数目
} SECTION_INFO, *PSECTION_INFO;

// 程序头信息
typedef struct {
    #define NEW_E_APP_MARK  ((DWORD)'JW')
    DWORD   m_dwMark;                    // 程序标记
    char    m_chMark[32];                // 程序标记文本
    INT     m_nHeaderSize;               // 头信息尺寸
    INT     m_nVersion;                  // 版本号
    INT     m_nType;                     // 程序类型
    #define ACS_IS_LIB  (1 << 0)
    DWORD   m_dwState;                   // 程序状态
    DWORD   m_dwCurFreeID;               // 可用 ID
    INT     m_nDllCmdCount;              // DLL 命令数目
    INT     m_nStartCodeOffset;          // 启动代码偏移
    INT     m_nConstSectionOffset;       // 常量段偏移
    INT     m_nWinFormSectionOffset;     // 窗体段偏移
    INT     m_nHelpFuncSectionOffset;    // 辅助函数段偏移
    INT     m_nCodeSectionOffset;       // 代码段偏移
    INT     m_nVarSectionOffset;         // 全局变量段偏移
    INT     m_nBeginSectionOffset;       // 首段信息偏移
} APP_HEADER_INFO, *PAPP_HEADER_INFO;

// ============================================================================
// COFF OBJ 文件格式结构
// ============================================================================
#pragma pack(push, old_coff_pack)
#pragma pack(1)

// COFF 文件头
typedef struct {
    unsigned short usMagic;         // 魔数(0x014c = i386)
    unsigned short usNumSec;       // 段数
    unsigned long  ulTime;         // 时间戳
    unsigned long  ulSymbolOffset;  // 符号表偏移
    unsigned long  ulNumSymbol;    // 符号数
    unsigned short usOptHdrSZ;     // 可选头长度
    unsigned short usFlags;        // 文件标志
} FILEHDR;

// COFF 段头
typedef struct {
    char           cName[8];       // 段名
    unsigned long  ulVSize;        // 虚拟大小
    unsigned long  ulVAddr;        // 虚拟地址
    unsigned long  ulSize;         // 段长度
    unsigned long  ulSecOffset;    // 段数据偏移
    unsigned long  ulRelOffset;    // 重定位表偏移
    unsigned long  ulLNOffset;    // 行号表偏移
    unsigned short ulNumRel;       // 重定位项数
    unsigned short ulNumLN;        // 行号表项数
    unsigned long  ulFlags;        // 段标志
} SECHDR;

// COFF 重定位项
typedef struct {
    unsigned long  ulAddr;         // 位置偏移
    unsigned long  ulSymbol;       // 符号索引
    unsigned short usType;         // 定位类型(6=ADDR32, 20=REL32)
} RELOC;

// COFF 符号表项中的字符串表引用
typedef struct {
    unsigned long ulZero;          // 字符串表标识(0)
    unsigned long ulOffset;       // 字符串表偏移
} eSYMENT;

// COFF 符号表项
typedef struct {
    union {
        char cName[8];            // 符号名(<=7 字节直接存)
        eSYMENT e;                // 字符串表引用(>7 字节)
    };
    unsigned long ulValue;        // 符号值
    short iSection;               // 所在段
    unsigned short usType;        // 符号类型
    unsigned char usClass;        // 符号存储类别
    unsigned char usNumAux;      // 辅助记录数
} SYMENT;

#pragma pack(pop, old_coff_pack)

// 符号表项(带完整名称)
typedef struct {
    SYMENT syment;
    char cName[256];
} LISTSYMENT, *PLISTSYMENT;

// 用户 DLL 命令信息
typedef struct {
    INT nSyment;
    INT nOffsetDllName;
    INT nOffsetFuncName;
} DllTableSyment, *PDllTableSyment;

// 用户静态库信息
typedef struct {
    INT nStack;
    BOOL bIsCdelCall;
    char cName[256];
} UserStaticLibInfo, *PUserStaticLibInfo;

// ============================================================================
// 支持库信息类(替代原项目 CELibInfo)
// ============================================================================
struct LibInfoEntry {
    bool m_bIsUse = false;
    INT m_nIDX = 0;
    std::vector<DWORD> m_dwProperty;
    std::vector<std::string> m_EClassNameList;
    std::string m_sLibName;
    std::string m_sNotifyLibFuncName;
    std::vector<std::string> m_LibFuncNameList;
    std::vector<std::string> m_DependentLibList;
};

// ============================================================================
// x86 指令长度计算(用于 MakeFuncToCdecl)
// ============================================================================
// 返回 x86 32 位指令长度。基于 InstructionLen.h (BSD license, oblique)
int insnLenX86_32(void* insn);

// ============================================================================
// EcodeToObjFile 类:易代码到 COFF OBJ 转换器
// ============================================================================
class EcodeToObjFile {
public:
    // 构造/析构
    EcodeToObjFile();
    ~EcodeToObjFile();

    // 读取易程序 PE 文件,提取 e 代码数据段
    // strEFilePath: PE 文件路径
    // lpRAWData:    [out] e 代码数据指针(调用者负责 delete[])
    // dwSize:       [out] e 代码数据大小
    // 返回:是否成功
    bool loadEProgram(const std::string& strEFilePath, LPBYTE& lpRAWData, DWORD& dwSize);

    // 解析 e 代码数据,生成 .text/.data 段 + 符号表 + 重定位表
    // pECode: e 代码数据(APP_HEADER_INFO 结构)
    // dwSize: 数据大小
    // 返回:是否成功
    bool parseECode(PAPP_HEADER_INFO pECode, DWORD dwSize);

    // 写出 COFF OBJ 文件
    // strPath: 目标 OBJ 文件路径
    // 返回:是否成功
    bool saveObjFile(const std::string& strPath);

    // 根据调试编译期间收集到的静态库符号，生成调试用 DEF/OBJ。
    // strBasePath 不含扩展名，例如“D:\\项目\\调试静态库”。
    bool makeDebugFile(const std::string& strBasePath) const;

    // 添加符号到符号表(自动去重)
    // 返回:符号索引
    int addSyment(const char* strName, DWORD ulValue, DWORD iSection, bool bIsVar = true);

    // 处理 DLL 命令声明,生成对应的 COFF 符号
    bool addDllSyment(const char* strLibName, const char* strFuncName,
                      INT& nIDX, INT& nStack, INT& bIsLibfun, INT nDefaultStack = 0);

    // 检查项目链接选项中的库/OBJ 是否提供指定符号。
    bool hasLegacyLinkSymbol(const std::string& symbol) const;

    // 检查黑月核心归档是否提供指定的命令实现符号。
    bool hasCoreLinkSymbol(const std::string& symbol) const;

    bool hasSymbolInFiles(
        const std::string& symbol,
        const std::vector<std::string>& files) const;

    // 从易程序头中提取使用的支持库列表
    bool getLibList(PAPP_HEADER_INFO pEcode, DWORD dwSize,
                    std::vector<std::string>& sList);

    // 扫描支持库 DLL 的 PE 导入表,把依赖的 .lib 文件加入 useLibList
    void addLibListForElib(HINSTANCE hInstance);

    // 查找支持库在 ELibInfolist 中的索引
    int getClassIndex(INT nIDX);

    // 将 __stdcall 函数改为 __cdecl(RET n -> RET + NOP)
    void makeFuncToCdecl(LPBYTE pStart, int nLen);

    // ---- 公开成员 ----
    std::string m_error;                    // 错误信息
    std::string bmLibPath;                  // 黑月 lib 目录
    std::vector<std::string> paths;         // 库搜索路径列表
    std::vector<std::string> legacyLinkFiles; // 项目 INI 中的链接文件
    std::vector<std::string> useLibList;   // 需链接的库列表
    // 当前核心 FNE 通过 NL_GET_CMD_FUNC_NAMES 导出的命令实现符号。
    // 下标与易代码中的核心命令索引一致，优先于历史兼容表。
    std::vector<std::string> coreFunctionNames;
    // 由核心 FNE 数据类型关系推导出的 COM/Variant 命令集合。
    std::vector<unsigned char> coreComFunctionFlags;
    // 本次转换候选的黑月核心归档，用于避免把 FNE 名称映射到不存在的实现。
    std::vector<std::string> coreSymbolFiles;
    std::vector<std::string> exportFuncName; // DLL 导出函数名列表
    std::vector<DWORD> exportFuncOffset;    // DLL 导出函数偏移列表
    std::vector<LibInfoEntry*> elibInfoList; // 支持库信息列表
    std::vector<DllTableSyment*> userDllTable;     // 用户 DLL 命令表
    std::vector<UserStaticLibInfo*> userStaticLib;  // 用户静态库列表
    std::vector<INT> dllIndexList;          // DLL 命令符号索引列表
    std::vector<INT> dllCallStackNum;       // DLL 命令栈大小列表
    std::vector<INT> isLibFunList;          // 是否为静态库函数标志

    bool bIsDLL = false;        // 是否为 DLL
    bool bCdecl = false;        // 是否使用 cdecl 调用约定
    bool bIsConsole = false;    // 是否为控制台程序
    bool bUseCom = false;        // 是否使用 COM
    DWORD dwDllMainOffset = 0;  // DllMain 偏移

    // COFF 段数据
    LPBYTE m_pDataSection = nullptr;
    LPBYTE m_pCodeSection = nullptr;
    LPBYTE m_pCrtSection = nullptr;
    DWORD m_nVarSectionBase = 0;

    // COFF 重定位表
    RELOC* dataReLoclist = nullptr;
    RELOC* codeReLoclist = nullptr;
    RELOC* crtReLoclist = nullptr;

    // 符号表
    std::vector<LISTSYMENT*> symentList;

    // COFF 文件/段头
    FILEHDR objFileHdr;
    SECHDR objTextSec;
    SECHDR objDataSec;
    SECHDR objCrtSec;
    int m_nTextSecNumRel = 0;
};

} // namespace bm

#endif // __BM_ECODE_TO_OBJ_H__
