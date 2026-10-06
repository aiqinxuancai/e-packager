// 覆盖 lib2.h 全部 20 种 UD 属性的真实 Win32 支持库接口。
#include <Windows.h>
#include <lib2.h>
#include <cstring>
#include <cstdlib>

struct Slot { int size; BYTE bytes[256]; };
struct State { Slot values[20]; };
static const SHORT kinds[] = {UD_PICK_SPEC_INT, UD_INT, UD_DOUBLE, UD_BOOL, UD_DATE_TIME,
    UD_TEXT, UD_PICK_INT, UD_PICK_TEXT, UD_EDIT_PICK_TEXT, UD_PIC, UD_ICON, UD_CURSOR,
    UD_MUSIC, UD_FONT, UD_COLOR, UD_COLOR_TRANS, UD_FILE_NAME, UD_COLOR_BACK, UD_IMAGE_LIST, UD_CUSTOMIZE};
static UNIT_PROPERTY properties[20] = {};
static char names[20][16] = {};
static LIB_DATA_TYPE_INFO type = {};
static LIB_INFO info = {};
static State* GetState(HUNIT unit) { return reinterpret_cast<State*>(GetWindowLongPtrW(reinterpret_cast<HWND>(unit), GWLP_USERDATA)); }
static bool Binary(int i) { return (i >= 9 && i <= 13) || i >= 18; }
static bool Text(int i) { return i == 5 || i == 7 || i == 8 || i == 16; }
static HUNIT WINAPI Create(LPBYTE data, INT size, DWORD style, HWND parent, UINT id, HMENU menu,
    INT x, INT y, INT cx, INT cy, DWORD, DWORD, HWND, BOOL)
{
    auto* state = new State{};
    if (size == sizeof(State)) memcpy(state, data, size);
    HWND wnd = CreateWindowW(L"STATIC", L"", style, x, y, cx, cy, parent, menu, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    return reinterpret_cast<HUNIT>(wnd);
}
static BOOL WINAPI Get(HUNIT unit, INT index, PUNIT_PROPERTY_VALUE value)
{
    if (index < 0 || index >= 20) return FALSE;
    const Slot& slot = GetState(unit)->values[index];
    if (Binary(index)) {
        value->m_data.m_nDataSize = slot.size;
        value->m_data.m_pData = static_cast<LPBYTE>(malloc(slot.size ? slot.size : 1));
        memcpy(value->m_data.m_pData, slot.bytes, slot.size);
    } else if (Text(index)) {
        char* data = static_cast<char*>(malloc(slot.size + 1));
        memcpy(data, slot.bytes, slot.size); data[slot.size] = 0; value->m_szText = data;
    } else memcpy(value, slot.bytes, index == 2 || index == 4 ? 8 : 4);
    return TRUE;
}
static BOOL WINAPI Set(HUNIT unit, INT index, PUNIT_PROPERTY_VALUE value, LPCSTR*)
{
    int gate = 0; memcpy(&gate, GetState(unit)->values[1].bytes, 4);
    if (index == 19 && gate == -1) return FALSE; // 拒绝更新必须由回读校验发现。
    Slot& slot = GetState(unit)->values[index];
    const void* data = value;
    int size = index == 2 || index == 4 ? 8 : 4;
    if (Binary(index)) { data = value->m_data.m_pData; size = value->m_data.m_nDataSize; }
    else if (Text(index)) { data = value->m_szText; size = static_cast<int>(strlen(value->m_szText)); }
    if (size < 0 || size > 256) return FALSE;
    slot.size = size; memcpy(slot.bytes, data, size);
    return index == 13; // 字体强制走保存、重建、回读链路。
}
static BOOL WINAPI Editable(HUNIT unit, INT index)
{
    int gate = 0; memcpy(&gate, GetState(unit)->values[1].bytes, 4);
    return index != 19 || gate != -1;
}
static HGLOBAL WINAPI Save(HUNIT unit)
{
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, sizeof(State));
    memcpy(GlobalLock(block), GetState(unit), sizeof(State)); GlobalUnlock(block); return block;
}
static PFN_INTERFACE WINAPI Interface(INT n)
{
    switch (n) {
    case ITF_CREATE_UNIT: return reinterpret_cast<PFN_INTERFACE>(Create);
    case ITF_GET_PROPERTY_DATA: return reinterpret_cast<PFN_INTERFACE>(Get);
    case ITF_NOTIFY_PROPERTY_CHANGED: return reinterpret_cast<PFN_INTERFACE>(Set);
    case ITF_PROPERTY_UPDATE_UI: return reinterpret_cast<PFN_INTERFACE>(Editable);
    case ITF_GET_ALL_PROPERTY_DATA: return reinterpret_cast<PFN_INTERFACE>(Save);
    default: return nullptr;
    }
}
static INT_PTR WINAPI Notify(INT n, DWORD_PTR p, DWORD_PTR)
{
    if (n == NRS_MFREE) free(reinterpret_cast<void*>(p));
    return 0;
}
extern "C" __declspec(dllexport) PLIB_INFO WINAPI GetNewInf()
{
    for (int i = 0; i < 20; ++i) {
        wsprintfA(names[i], "Value%d", i);
        properties[i] = {names[i], names[i], nullptr, kinds[i], _PROP_OS(__OS_WIN), nullptr};
    }
    type.m_szName = "Lib2Control"; type.m_dwState = LDT_WIN_UNIT;
    type.m_nPropertyCount = 20; type.m_pPropertyBegin = properties; type.m_pfnGetInterface = Interface;
    info.m_dwLibFormatVer = LIB_FORMAT_VER; info.m_szGuid = "6C9EE8C6E56343E6978F70B7EF9C8A91";
    info.m_szName = "Lib2Properties"; info.m_nMajorVersion = 1;
    info.m_nDataTypeCount = 1; info.m_pDataType = &type; info.m_pfnNotify = Notify;
    return &info;
}
