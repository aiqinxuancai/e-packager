// 用真实 FNE ABI 验证旧/新事件、长枚举、空文件配置及损坏元数据导出。
#include <Windows.h>
#include <lib2.h>
#include <intrin.h>

static bool initialized = false;
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) initialized = true;
    return TRUE;
}

#define OPTIONS8 "Option\0Option\0Option\0Option\0Option\0Option\0Option\0Option\0"
#define OPTIONS64 OPTIONS8 OPTIONS8 OPTIONS8 OPTIONS8 OPTIONS8 OPTIONS8 OPTIONS8 OPTIONS8
static const char picks[] = OPTIONS64 OPTIONS64 "Last\0";
static UNIT_PROPERTY properties[] = {
    {"LongPick", "longPick", "129 choices", UD_PICK_INT, _PROP_OS(OS_ALL), picks},
    {"File", "file", "empty title and extension", UD_FILE_NAME, _PROP_OS(__OS_WIN), "\0Text|*.txt\0\0" "1\0"},
    {"Spec", "spec", nullptr, UD_PICK_SPEC_INT, UW_ONLY_READ, "-7\0\0" "42\0Answer\0\0"},
    {nullptr, nullptr, nullptr, 0, UW_IS_HIDED, nullptr},
};
static EVENT_ARG_INFO oldArgs[] = {{"Enabled", "legacy bool", EAS_IS_BOOL_ARG}, {"Index", nullptr, 0}};
static EVENT_INFO oldEvents[] = {
    {"Legacy", "old event", EV_RETURN_BOOL | _EVENT_OS(__OS_WIN), 2, oldArgs},
    {"LegacyInt", nullptr, EV_RETURN_INT | EV_IS_HIDED, 0, nullptr},
};
static EVENT_ARG_INFO2 newArgs[] = {{"Value", "reference parameter", EAS_BY_REF, SDT_INT}};
static EVENT_INFO2 newEvents[] = {{"Modern", "new event", static_cast<DWORD>(EV_IS_VER2) | EV_IS_KEY_EVENT | _EVENT_OS(__OS_LINUX), 1, newArgs, SDT_INT}};
static LIB_DATA_TYPE_INFO types[3] = {};
static LIB_INFO info = {};
extern "C" __declspec(dllexport) PLIB_INFO WINAPI GetNewInf()
{
    // 未初始化的映像不能执行入口；快速失败确保测试不能靠 SEH 捕获后重试蒙混通过。
    if (!initialized) __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    types[0].m_szName = "LegacyControl";
    types[0].m_dwState = LDT_WIN_UNIT | _DT_OS(__OS_WIN);
    types[0].m_nPropertyCount = 4;
    types[0].m_pPropertyBegin = properties;
    types[0].m_nEventCount = 2;
    types[0].m_pEventBegin = reinterpret_cast<PEVENT_INFO2>(oldEvents);
    types[1].m_szName = "ModernControl";
    types[1].m_dwState = LDT_WIN_UNIT;
    types[1].m_nEventCount = 1;
    types[1].m_pEventBegin = newEvents;
    types[2].m_szName = "BrokenControl";
    types[2].m_dwState = LDT_WIN_UNIT;
    types[2].m_nPropertyCount = 2;
    types[2].m_nEventCount = 3;
    info.m_dwLibFormatVer = LIB_FORMAT_VER;
    info.m_szGuid = "AD7611EC1A214E67A80BAD270445B361";
    info.m_szName = "ExportFixture";
    info.m_nMajorVersion = 1;
    info.m_nDataTypeCount = 3;
    info.m_pDataType = types;
    return &info;
}
