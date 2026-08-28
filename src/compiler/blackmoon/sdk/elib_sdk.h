// ============================================================================
// elib_sdk.h - 易语言支持库 SDK 精简头文件
// ============================================================================
// 本文件从原易语言 SDK(lib2.h)中提取黑月插件所需的核心数据结构定义,
// 去除了与黑月无关的窗口单元(GUI)相关部分,保留:
//   1. 基本数据类型定义(SDT_*, DATA_TYPE 等)
//   2. 命令信息结构(CMD_INFO, ARG_INFO)
//   3. 库信息结构(LIB_INFO)—— 支持库的核心描述
//   4. 通知常量(NL_*, NRS_*, NR_*)
//   5. 通知函数原型(PFN_NOTIFY_SYS, PFN_NOTIFY_LIB)
//
// 注意:本文件仅包含结构定义,不含实现逻辑。
// ============================================================================
#ifndef __ELIB_SDK_H__
#define __ELIB_SDK_H__

#include <windows.h>

// ----------------------------------------------------------------------------
// 基本字符集与语言版本定义
// ----------------------------------------------------------------------------
#ifndef _T
    #define _T(x)      x
    #define _WT(x)     x
#endif

// 支持库支持的语言
#define LT_CHINESE  1
#define LT_ENGLISH  2

// 操作系统支持标志(用于 m_dwState 中的平台标记)
#define __OS_WIN        0x80000000
#define __OS_LINUX      0x40000000
#define __OS_UNIX       0x20000000
#define OS_ALL          (__OS_WIN | __OS_LINUX | __OS_UNIX)

// 库操作系统转换宏:将 os 标志转换为可放入 m_dwState 的值
#define _LIB_OS(os)     (os)
#define _TEST_LIB_OS(m_dwState,os)    ((_LIB_OS(os) & (m_dwState)) != 0)

// 语言版本(本插件为简体中文)
#define __GBK_LANG_VER  1

// ----------------------------------------------------------------------------
// 易语言数据类型定义
// ----------------------------------------------------------------------------
typedef DWORD DATA_TYPE;
typedef DATA_TYPE* PDATA_TYPE;

// 数据类型掩码:区分系统类型/用户类型/库类型
#define DTM_SYS_DATA_TYPE_MASK      0x80000000
#define DTM_USER_DATA_TYPE_MASK     0x40000000
#define DTM_LIB_DATA_TYPE_MASK      0x00000000

// 数组标志:某数据类型值置此位表示为数组
#define DT_IS_ARY                    0x20000000

// 系统基本数据类型
#define _SDT_NULL        0                                              // 空白
#define _SDT_ALL         MAKELONG(MAKEWORD(0, 0), 0x8000)              // 通用型(内部使用)
#define SDT_BYTE         MAKELONG(MAKEWORD(1, 1), 0x8000)             // 字节
#define SDT_SHORT        MAKELONG(MAKEWORD(1, 2), 0x8000)             // 短整数
#define SDT_INT          MAKELONG(MAKEWORD(1, 3), 0x8000)            // 整数
#define SDT_INT64        MAKELONG(MAKEWORD(1, 4), 0x8000)             // 长整数
#define SDT_FLOAT        MAKELONG(MAKEWORD(1, 5), 0x8000)            // 小数
#define SDT_DOUBLE       MAKELONG(MAKEWORD(1, 6), 0x8000)            // 双精度小数
#define SDT_BOOL         MAKELONG(MAKEWORD(2, 0), 0x8000)            // 逻辑型
#define SDT_DATE_TIME    MAKELONG(MAKEWORD(3, 0), 0x8000)            // 日期时间型
#define SDT_TEXT         MAKELONG(MAKEWORD(4, 0), 0x8000)            // 文本型
#define SDT_BIN          MAKELONG(MAKEWORD(5, 0), 0x8000)            // 字节集
#define SDT_SUB_PTR      MAKELONG(MAKEWORD(6, 0), 0x8000)            // 子程序指针

// 逻辑型真/假值
typedef SHORT   DTBOOL;
typedef DTBOOL* PDTBOOL;
#define BL_TRUE     (-1)
#define BL_FALSE    0

// 程序版本类型
#define PT_EDIT_VER          1   // 为易语言编辑版
#define PT_DEBUG_RUN_VER     2   // 为 DEBUG 调试运行版
#define PT_RELEASE_RUN_VER   3   // 为 RELEASE 正式运行版

// ----------------------------------------------------------------------------
// 命令参数信息结构
// ----------------------------------------------------------------------------
typedef struct
{
    LPTSTR      m_szName;            // 参数名称
    LPTSTR      m_szExplain;        // 参数详细说明
    SHORT       m_shtBitmapIndex;   // 指向图像索引(1 起,0 表无)
    SHORT       m_shtBitmapCount;   // 图像数目
    DATA_TYPE   m_dtType;           // 参数数据类型
    INT         m_nDefault;         // 默认指针值
    DWORD       m_dwState;          // 状态标志(AS_* 宏)
    // 参数状态标志
    #define AS_HAS_DEFAULT_VALUE           (1 << 0)   // 参数有默认值
    #define AS_DEFAULT_VALUE_IS_EMPTY      (1 << 1)   // 默认值为空
    #define AS_RECEIVE_VAR                 (1 << 2)   // 提供变量(非数组)
    #define AS_RECEIVE_VAR_ARRAY           (1 << 3)   // 提供变量数组
    #define AS_RECEIVE_VAR_OR_ARRAY        (1 << 4)   // 提供变量或数组
    #define AS_RECEIVE_ARRAY_DATA          (1 << 5)   // 提供数组数据
    #define AS_RECEIVE_ALL_TYPE_DATA       (1 << 6)   // 提供所有类型数据
    #define AS_RECEIVE_VAR_OR_OTHER        (1 << 9)   // 提供变量地址或数据
}
ARG_INFO, *PARG_INFO;

// ----------------------------------------------------------------------------
// 命令信息结构
// ----------------------------------------------------------------------------
typedef struct
{
    LPTSTR      m_szName;            // 命令中文名称
    LPTSTR      m_szEgName;          // 命令英文名称(可为 NULL)
    LPTSTR      m_szExplain;         // 命令详细说明
    SHORT       m_shtCategory;       // 全局命令类别(从 1 起,库内为 -1)
    WORD        m_wState;            // 命令状态标志(CT_* 宏)
    DATA_TYPE   m_dtRetValType;     // 返回值数据类型
    WORD        m_wReserved;         // 保留,必须为 0
    SHORT       m_shtUserLevel;      // 用户学习难度等级
    SHORT       m_shtBitmapIndex;    // 图像索引
    SHORT       m_shtBitmapCount;    // 图像数目
    INT         m_nArgCount;         // 参数数目
    PARG_INFO   m_pBeginArgInfo;     // 参数信息数组
    // 命令状态标志
    #define CT_IS_HIDED             (1 << 2)   // 是否为隐藏命令
    #define CT_IS_ERROR             (1 << 3)   // 在编译中不能使用
    #define CT_DISABLED_IN_RELEASE  (1 << 4)   // RELEASE 版中不可用
    #define CT_ALLOW_APPEND_NEW_ARG (1 << 5)   // 末尾允许追加参数
    #define CT_RETRUN_ARY_TYPE_DATA (1 << 6)   // 返回值为数组
    #define CT_IS_OBJ_COPY_CMD     (1 << 7)   // 对象复制命令
    #define CT_IS_OBJ_FREE_CMD     (1 << 8)   // 对象释放命令
    #define CT_IS_OBJ_CONSTURCT_CMD (1 << 9)  // 对象构造命令
}
CMD_INFO, *PCMD_INFO;

// ----------------------------------------------------------------------------
// 库常量信息结构
// ----------------------------------------------------------------------------
typedef struct
{
    LPTSTR  m_szName;        // 常量名称
    LPTSTR  m_szEgName;      // 英文名称
    LPTSTR  m_szExplain;     // 说明
    SHORT   m_shtLayout;     // 保留
    SHORT   m_shtType;        // 常量类型(CT_NULL/CT_NUM/CT_BOOL/CT_TEXT)
    LPTSTR  m_szText;        // CT_TEXT 时的文本
    DOUBLE  m_dbValue;       // CT_NUM/CT_BOOL 时的数值
    // 常量类型
    #define CT_NULL     0
    #define CT_NUM      1   // 数值:3.1415926
    #define CT_BOOL     2   // 逻辑:1
    #define CT_TEXT     3   // 文本:"abc"
}
LIB_CONST_INFO, *PLIB_CONST_INFO;

// ----------------------------------------------------------------------------
// 窗口单元标识(用于标识一个窗口中的某个单元)
// ----------------------------------------------------------------------------
typedef struct
{
    DWORD m_dwFormID;   // 窗口 ID
    DWORD m_dwUnitID;   // 窗口单元 ID
}
MUNIT, *PMUNIT;

// ----------------------------------------------------------------------------
// 调用数据结构(MDATA_INF):命令调用时传递的参数/返回值
// ----------------------------------------------------------------------------
#pragma pack(push, old_value)
#pragma pack(1)   // 一字节对齐,确保 sizeof(MDATA_INF) == 12

typedef struct
{
    union
    {
        BYTE      m_byte;          // SDT_BYTE
        SHORT     m_short;         // SDT_SHORT
        INT       m_int;           // SDT_INT
        DWORD     m_uint;          // (DWORD)SDT_INT
        INT64     m_int64;         // SDT_INT64
        FLOAT     m_float;         // SDT_FLOAT
        DOUBLE    m_double;        // SDT_DOUBLE
        DATE      m_date;          // SDT_DATE_TIME
        BOOL      m_bool;          // SDT_BOOL
        char*     m_pText;         // SDT_TEXT(可能为 NULL)
        LPBYTE    m_pBin;          // SDT_BIN(可能为 NULL)
        DWORD     m_dwSubCodeAdr;  // SDT_SUB_PTR 子程序地址
        MUNIT     m_unit;          // 窗口单元类型
        void*     m_pCompoundData; // 复合数据类型指针
        void*     m_pAryData;      // 数组数据指针
        // 指针形式(AS_RECEIVE_VAR 等标志时使用)
        BYTE*     m_pByte;
        SHORT*    m_pShort;
        INT*      m_pInt;
        DWORD*    m_pUInt;
        INT64*    m_pInt64;
        FLOAT*    m_pFloat;
        DOUBLE*   m_pDouble;
        DATE*     m_pDate;
        BOOL*     m_pBool;
        char**    m_ppText;
        LPBYTE*   m_ppBin;
        DWORD*    m_pdwSubCodeAdr;
        PMUNIT    m_pUnit;
        void**    m_ppCompoundData;
        void**    m_ppAryData;
    };
    DATA_TYPE m_dtDataType;   // 本数据的实际数据类型
}
MDATA_INF, *PMDATA_INF;

#pragma pack(pop, old_value)
// 断言:MDATA_INF 必须为 12 字节(3 个 DWORD)
// static_assert(sizeof(MDATA_INF) == sizeof(DWORD) * 3, "MDATA_INF size mismatch");

// ----------------------------------------------------------------------------
// 通知函数原型
// ----------------------------------------------------------------------------
// 系统通知支持库的函数(IDE 或运行时调用支持库)
typedef INT (WINAPI *PFN_NOTIFY_LIB) (INT nMsg, DWORD dwParam1, DWORD dwParam2);
// 支持库通知系统的函数(支持库调用 IDE 或运行时)
typedef INT (WINAPI *PFN_NOTIFY_SYS) (INT nMsg, DWORD dwParam1, DWORD dwParam2);

// 命令执行函数原型(CDECL 调用约定)
typedef void (*PFN_EXECUTE_CMD) (PMDATA_INF pRetData, INT nArgCount, PMDATA_INF pArgInf);
// AddIn 功能函数原型
typedef INT (WINAPI *PFN_RUN_ADDIN_FN) (INT nAddInFnIndex);
// 超级模板函数原型
typedef INT (WINAPI *PFN_SUPER_TEMPLATE) (INT nTemplateIndex);

// ----------------------------------------------------------------------------
// 支持库信息结构(LIB_INFO)—— 支持库的核心描述
// ----------------------------------------------------------------------------
// 易语言通过 GetNewInf() 获取本结构指针,据此识别并加载支持库。
// 填充完整的 LIB_INFO 是支持库被正确加载的前提。
// ----------------------------------------------------------------------------
#define LIB_FORMAT_VER  20000101   // 库格式号

typedef struct
{
    DWORD   m_dwLibFormatVer;          // 库格式号,必须为 LIB_FORMAT_VER
    LPSTR   m_szGuid;                  // 本库唯一 GUID(不可为 NULL)
    INT     m_nMajorVersion;           // 主版本号
    INT     m_nMinorVersion;           // 次版本号
    INT     m_nBuildNumber;            // 构建版本号
    INT     m_nRqSysMajorVer;          // 所需易语言系统主版本
    INT     m_nRqSysMinorVer;          // 所需易语言系统次版本
    INT     m_nRqSysKrnlLibMajorVer;   // 所需系统核心库主版本
    INT     m_nRqSysKrnlLibMinorVer;   // 所需系统核心库次版本
    LPSTR   m_szName;                  // 库名称(不可为 NULL)
    INT     m_nLanguage;               // 库支持的语言(LT_CHINESE 等)
    LPSTR   m_szExplain;               // 库详细说明
    DWORD   m_dwState;                 // 库状态标志(LBS_* 宏)
    // 库状态标志
    #define LBS_FUNC_NO_RUN_CODE   (1 << 2)   // 纯库,无支持执行代码
    #define LBS_NO_EDIT_INFO       (1 << 3)   // 无编辑用信息
    #define LBS_IS_DB_LIB          (1 << 5)   // 数据库操作支持库
    #define LBS_LIB_INFO2         (1 << 7)   // 为 LIB_INFO2 结构
    // 作者信息
    LPSTR   m_szAuthor;
    LPSTR   m_szZipCode;
    LPSTR   m_szAddress;
    LPSTR   m_szPhoto;
    LPSTR   m_szFax;
    LPSTR   m_szEmail;
    LPSTR   m_szHomePage;
    LPSTR   m_szOther;
    // 数据类型
    INT     m_nDataTypeCount;          // 自定义数据类型数目
    LPVOID  m_pDataType;               // 数据类型信息数组(此处简化为 LPVOID)
    // 命令类别
    INT     m_nCategoryCount;          // 全局命令类别数目
    LPSTR   m_szzCategory;             // 全局命令类别菜单文本
    // 命令
    INT                 m_nCmdCount;   // 命令数目
    PCMD_INFO           m_pBeginCmdInfo;   // 命令信息数组
    PFN_EXECUTE_CMD*    m_pCmdsFunc;       // 命令执行函数数组
    // AddIn 功能
    PFN_RUN_ADDIN_FN    m_pfnRunAddInFn;   // AddIn 功能函数(可为 NULL)
    LPSTR               m_szzAddInFnInfo;  // AddIn 说明文本
    // 通知函数(不可为 NULL)
    PFN_NOTIFY_LIB      m_pfnNotify;
    // 超级模板
    PFN_SUPER_TEMPLATE  m_pfnSuperTemplate;    // 可为 NULL
    LPSTR               m_szzSuperTemplateInfo; // 可为 NULL
    // 库常量
    INT                 m_nLibConstCount;
    PLIB_CONST_INFO     m_pLibConst;
    // 依赖文件列表
    LPSTR  m_szzDependFiles;   // 可为 NULL
}
LIB_INFO, *PLIB_INFO;

// 取支持库信息的导出函数名
#define FUNCNAME_GET_LIB_INFO    "GetNewInf"
typedef PLIB_INFO (WINAPI *PFN_GET_LIB_INFO) ();

// ----------------------------------------------------------------------------
// 通知常量定义
// ----------------------------------------------------------------------------

// NL_* : 系统通知支持库的通知值
#define NL_SYS_NOTIFY_FUNCTION       1    // 通知支持库记录系统通知函数指针
                                           //   dwParam1 = (PFN_NOTIFY_SYS)
#define NL_FREE_LIB_DATA             6    // 通知支持库释放资源,准备退出
#define NL_GET_CMD_FUNC_NAMES       14   // 返回命令实现函数名表(char*[])
#define NL_GET_NOTIFY_LIB_FUNC_NAME 15   // 返回通知函数名(PFN_NOTIFY_LIB)
#define NL_GET_DEPENDENT_LIBS       16   // 返回静态库依赖文件列表
#define NL_UNLOAD_FROM_IDE          17   // 支持库被 IDE 卸载

// NR_* : 通知处理返回值
#define NR_OK     0
#define NR_ERR    (-1)

// NES_* : 通知易语言编辑器(IDE)的通知值
#define NES_GET_MAIN_HWND          1    // 取易语言主窗口句柄
#define NES_RUN_FUNC               2    // 通知易语言执行指定功能

// NAS_* : 通知易语言调试器或运行时的通知值
#define NAS_GET_APP_ICON           1000
#define NAS_GET_PATH               1006
#define NAS_IS_EWIN                1014

// NRS_* : 通知运行时系统的通知值
#define NRS_UNIT_DESTROIED         2000
#define NRS_GET_CMD_LINE_STR       2002
#define NRS_GET_EXE_PATH_STR       2003
#define NRS_GET_EXE_NAME           2004
#define NRS_DO_EVENTS              2018
#define NRS_MALLOC                 2024   // 分配内存
#define NRS_MFREE                  2025   // 释放内存
#define NRS_MREALLOC               2026   // 重新分配内存
#define NRS_RUNTIME_ERR            2027   // 通知运行时错误
#define NRS_EXIT_PROGRAM           2028   // 通知系统退出
#define NRS_GET_PRG_TYPE           2030   // 返回当前程序类型
#define NRS_EVENT_NOTIFY           2008
#define NRS_EVENT_NOTIFY2          2031
#define NRS_GET_WINFORM_COUNT      2032
#define NRS_GET_WINFORM_HWND       2033

#endif // __ELIB_SDK_H__
