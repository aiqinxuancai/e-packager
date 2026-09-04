#include "CppEmitter.h"

#include <Windows.h>
#include "NativeWindowControl.h"
#include "../PathHelper.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <functional>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <queue>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ecompiler {
namespace {

constexpr std::uint16_t kCommandAllowAppendArgument = 1u << 5;
constexpr std::uint16_t kCommandReturnsArray = 1u << 6;
constexpr std::uint16_t kCommandObjectCopy = 1u << 7;
constexpr std::uint16_t kCommandObjectFree = 1u << 8;
constexpr std::uint16_t kCommandObjectConstruct = 1u << 9;
constexpr std::uint32_t kArgumentHasDefault = 1u << 0;
constexpr std::uint32_t kArgumentDefaultEmpty = 1u << 1;
constexpr std::uint32_t kArgumentReceivesVariable = 1u << 2;
constexpr std::uint32_t kArgumentReceivesVariableArray = 1u << 3;
constexpr std::uint32_t kArgumentReceivesVariableOrArray = 1u << 4;
constexpr std::uint32_t kArgumentReceivesArrayData = 1u << 5;
constexpr std::uint32_t kArgumentReceivesVariableOrOther = 1u << 9;
constexpr std::uint32_t kTypeStatement = 0x80000008u;
constexpr std::uint32_t T_WINDOW_UNIT=0x70000001u;

std::string EscapeCppString(const std::string& value)
{
	std::ostringstream output;
	output << '"';
	for (const unsigned char ch : value) {
		switch (ch) {
		case '\\': output << "\\\\"; break;
		case '"': output << "\\\""; break;
		case '\r': output << "\\r"; break;
		case '\n': output << "\\n"; break;
		case '\t': output << "\\t"; break;
		case 0: output << "\\000"; break;
		default:
			if (ch < 0x20 || ch >= 0x7f) {
				output << '\\' << std::oct << std::setw(3) << std::setfill('0') << static_cast<unsigned int>(ch) << std::dec;
			}
			else output << static_cast<char>(ch);
			break;
		}
	}
	output << '"';
	return output.str();
}

std::string Hex(const std::uint32_t value)
{
	std::ostringstream output;
	output << "0x" << std::hex << std::uppercase << value << 'u';
	return output.str();
}

std::string DateTimeValue(const std::string& text)
{
	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
	if (sscanf_s(text.c_str(), "[%d年%d月%d日%d时%d分%d秒]", &year, &month, &day, &hour, &minute, &second) < 3) return "0.0";
	SYSTEMTIME value{};
	value.wYear = static_cast<WORD>(year); value.wMonth = static_cast<WORD>(month); value.wDay = static_cast<WORD>(day);
	value.wHour = static_cast<WORD>(hour); value.wMinute = static_cast<WORD>(minute); value.wSecond = static_cast<WORD>(second);
	FILETIME fileTime{};
	if (!SystemTimeToFileTime(&value, &fileTime)) return "0.0";
	SYSTEMTIME epochValue{}; epochValue.wYear = 1899; epochValue.wMonth = 12; epochValue.wDay = 30;
	FILETIME epochFileTime{};
	if (!SystemTimeToFileTime(&epochValue, &epochFileTime)) return "0.0";
	ULARGE_INTEGER ticks{}; ticks.LowPart = fileTime.dwLowDateTime; ticks.HighPart = fileTime.dwHighDateTime;
	ULARGE_INTEGER epochTicks{}; epochTicks.LowPart = epochFileTime.dwLowDateTime; epochTicks.HighPart = epochFileTime.dwHighDateTime;
	const double oleDate = static_cast<double>(static_cast<long long>(ticks.QuadPart - epochTicks.QuadPart)) / 864000000000.0;
	return "Number(" + std::to_string(oleDate) + ")";
}

bool IsCppIdentifier(const std::string& value)
{
	if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_')) return false;
	return std::all_of(value.begin() + 1, value.end(), [](const unsigned char ch) {
		return std::isalnum(ch) != 0 || ch == '_';
	});
}

bool IsLvalue(const e2txt::SourceExpressionNode& node)
{
	return node.kind == e2txt::SourceExpressionKind::Name ||
		node.kind == e2txt::SourceExpressionKind::Index ||
		node.kind == e2txt::SourceExpressionKind::Member;
}

struct CommandBinding {
	std::size_t libraryIndex = 0;
	std::size_t commandIndex = 0;
	const support_library_public_info::CommandMetadata* command = nullptr;
	bool member = false;
};

bool IsCompilePrimitive(const support_library_public_info::CommandMetadata& command)
{
	static constexpr std::string_view names[] = {
		"MachineCode", "hex", "binary", "GetAppName", "XchgVar", "ForceXchgVar",
		"GetRuntimeDataType", "IsCondMacroDefined", "this",
		// These are language/runtime primitives. Their FNE entries describe the
		// source signature, while the generated Value runtime owns the actual
		// array state and must perform the mutation consistently across cores.
		"ReDim", "GetAryElementCount", "CopyAry", "AddElement", "InsElement",
		"RemoveElement", "RemoveAll", "dir", "not",
	};
	return std::find(std::begin(names), std::end(names), command.englishName) != std::end(names);
}

bool IsPlatformImportModule(const std::string& moduleName)
{
	std::filesystem::path modulePath = Utf8PathToPath(moduleName);
	std::string normalized = modulePath.filename().string();
	if (normalized.empty()) normalized = moduleName;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](const unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	if (std::filesystem::path(normalized).extension().empty()) normalized += ".dll";
	return normalized == "kernel32.dll" || normalized == "user32.dll" || normalized == "gdi32.dll" ||
		normalized == "advapi32.dll" || normalized == "shell32.dll" || normalized == "ole32.dll" ||
		normalized == "oleaut32.dll" || normalized == "comdlg32.dll" || normalized == "winmm.dll" ||
		normalized == "odbc32.dll" || normalized == "odbccp32.dll" || normalized == "ws2_32.dll";
}

// The runtime template is stored in this UTF-8 source file and is appended to
// another UTF-8 source file.  Keep its bytes unchanged so the generated
// compiler applies its normal source-to-execution charset conversion once.
std::string RuntimeSourceUtf8(const char* source)
{
	return source == nullptr ? std::string() : std::string(source);
}

const char* kRuntimeSource = R"CPP(
#include <Windows.h>
#include <algorithm>
#include <commdlg.h>
#include <commctrl.h>
#include <olectl.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <functional>
#include <memory>
#include <deque>
#include <shellapi.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#pragma comment(lib,"oleaut32.lib")

namespace ert {
using EPointer = std::uintptr_t;
using EIntPtr = std::intptr_t;
#if !defined(_WIN64) && defined(ECOMPILER_LEGACY_X86_RUNTIME)
// 旧版 FNE 可能直接引用 MSVCRT 的 getenv/_putenv。宿主使用现代 CRT
// 时它们可能维护独立的 CRT 环境副本，因此统一桥接到进程环境 API，
// 保证不同编译器运行时之间的环境变量状态一致。
extern "C" char* __cdecl getenv(const char* name) {
    static thread_local std::string value;
    value.clear();
    if(name==nullptr || *name==0) return nullptr;
    DWORD capacity=256;
    for(;;) {
        std::string buffer(capacity,'\0');
        const DWORD length=GetEnvironmentVariableA(name,buffer.data(),capacity);
        if(length==0) return nullptr;
        if(length<capacity-1) {
            buffer.resize(length);
            value=std::move(buffer);
            return value.data();
        }
        capacity=length+1;
    }
}
extern "C" int __cdecl _putenv(const char* assignment) {
    if(assignment==nullptr) return -1;
    const char* separator=std::strchr(assignment,'=');
    if(separator==nullptr || separator==assignment) return -1;
    const std::string name(assignment,separator);
    return SetEnvironmentVariableA(name.c_str(),separator+1)!=FALSE ? 0 : -1;
}
extern "C" int __cdecl LegacyVc6Swprintf(wchar_t* buffer,const wchar_t* format,...) {
    va_list arguments; va_start(arguments,format); const int result=wvsprintfW(buffer,format,arguments); va_end(arguments); return result;
}
#ifdef __argc
#undef __argc
#endif
#ifdef __argv
#undef __argv
#endif
#ifdef _mbctype
#undef _mbctype
#endif
extern "C" int __argc=0;
extern "C" char** __argv=nullptr;
extern "C" unsigned char _mbctype[257]{};
extern "C" unsigned char _iob[96]{};
// A few VC6-era archives still reference the legacy timezone data symbol
// rather than the modern CRT state-management accessor.  Keep the compatibility
// object under a private name and provide linker aliases so UCRT's `_timezone`
// macro/function declaration is not redefined in this translation unit.
#ifdef _timezone
#undef _timezone
#endif
extern "C" volatile long ecompiler_timezone=0;
#if defined(_M_IX86)
#pragma comment(linker,"/include:_ecompiler_timezone")
#else
#pragma comment(linker,"/include:ecompiler_timezone")
#endif
#pragma comment(linker,"/alternatename:_timezone=_ecompiler_timezone")
#pragma comment(linker,"/alternatename:__timezone=_ecompiler_timezone")
extern "C" int* __cdecl __p___argc();
extern "C" char*** __cdecl __p___argv();
extern "C" unsigned char* __cdecl __p__mbctype();
static void InitializeLegacyCrtData() {
    ecompiler_timezone = ecompiler_timezone;
    if(int* value=__p___argc())__argc=*value;
    if(char*** value=__p___argv())__argv=*value;
    if(unsigned char* value=__p__mbctype())for(std::size_t index=0;index<257;++index)_mbctype[index]=value[index];
    if(HMODULE module=GetModuleHandleA("msvcrt.dll")) {
        if(auto* value=reinterpret_cast<unsigned char*>(GetProcAddress(module,"_iob")))for(std::size_t index=0;index<96;++index)_iob[index]=value[index];
    }
}
static FILE* ResolveLegacyStandardStream(FILE* stream) {
    auto* address=reinterpret_cast<unsigned char*>(stream);
    if(address<_iob||address>=_iob+sizeof(_iob)) return stream;
    const std::ptrdiff_t offset=address-_iob;
    if((offset%32)!=0) return stream;
    HMODULE module=GetModuleHandleA("msvcrt.dll");
    auto* streams=module==nullptr?nullptr:reinterpret_cast<unsigned char*>(GetProcAddress(module,"_iob"));
    return streams==nullptr?stream:reinterpret_cast<FILE*>(streams+offset);
}
extern "C" int __cdecl fprintf(FILE* stream,const char* format,...) {
    (void)stream;
    va_list arguments; va_start(arguments,format);
    const char* text=nullptr;
    char formatted[2048]{};
    if(format!=nullptr && std::strcmp(format,"%s")==0) text=va_arg(arguments,const char*);
    else if(format!=nullptr) { _vsnprintf_s(formatted,sizeof(formatted),_TRUNCATE,format,arguments); text=formatted; }
    const std::size_t length=text==nullptr?0:std::strlen(text);
    DWORD written=0; HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE);
    if(output!=nullptr && output!=INVALID_HANDLE_VALUE && length!=0) WriteFile(output,text,static_cast<DWORD>(length),&written,nullptr);
    va_end(arguments); return static_cast<int>(written);
}
#endif
#if !defined(_WIN64) && !defined(ECOMPILER_LEGACY_X86_RUNTIME)
// VC6-era third-party archives may reference the process-wide timezone data
// symbol.  Keep a small x86-only compatibility object without enabling the
// broader legacy CRT bridge used by old FNEs.
#ifdef _timezone
#undef _timezone
#endif
extern "C" volatile long ecompiler_timezone_modern=0;
#pragma comment(linker,"/include:_ecompiler_timezone_modern")
#pragma comment(linker,"/alternatename:_timezone=_ecompiler_timezone_modern")
#pragma comment(linker,"/alternatename:__timezone=_ecompiler_timezone_modern")
static void InitializeLegacyTimezoneData() {
    ecompiler_timezone_modern = ecompiler_timezone_modern;
}
#endif
constexpr std::uint32_t T_NULL=0, T_ALL=0x80000000u, T_BYTE=0x80000101u, T_SHORT=0x80000201u;
constexpr std::uint32_t T_INT=0x80000301u, T_INT64=0x80000401u, T_FLOAT=0x80000501u;
constexpr std::uint32_t T_DOUBLE=0x80000601u, T_BOOL=0x80000002u, T_DATE=0x80000003u;
constexpr std::uint32_t T_TEXT=0x80000004u, T_BIN=0x80000005u, T_SUB=0x80000006u, T_ARRAY=0x20000000u;
constexpr std::uint32_t T_STATEMENT=0x80000008u;
constexpr std::uint32_t T_VARIABLE=0x20000000u;
constexpr std::uint32_t T_WINDOW_UNIT=0x70000001u;

// Runtime notification ABI shared by support libraries and the generated host.
enum : int {
    ecompiler_nrs_unit_destroyed=2000,
    ecompiler_nrs_convert_num_to_int=2001,
    ecompiler_nrs_get_cmd_line=2002,
    ecompiler_nrs_get_exe_path=2003,
    ecompiler_nrs_get_exe_name=2004,
    ecompiler_nrs_file_check=2011,
    ecompiler_nrs_file_register=2012,
    ecompiler_nrs_file_unregister=2013,
    ecompiler_nrs_do_events=2018,
    ecompiler_nrs_get_unit_data_type=2022,
    ecompiler_nrs_free_array=2023,
    ecompiler_nrs_malloc=2024,
    ecompiler_nrs_mfree=2025,
    ecompiler_nrs_mrealloc=2026,
    ecompiler_nrs_runtime_error=2027,
    ecompiler_nrs_exit_program=2028,
    ecompiler_nrs_get_program_type=2030,
    ecompiler_nl_sys_notify_function=1,
    ecompiler_nl_free_library_data=6,
};

#pragma pack(push,1)
struct MData {
    union { unsigned char byteValue; short shortValue; int intValue; long long int64Value; float floatValue;
        double doubleValue; char* textValue; unsigned char* binaryValue; void* pointerValue;
        unsigned char* bytePointer; short* shortPointer; int* intPointer; long long* int64Pointer;
        float* floatPointer; double* doublePointer; char** textPointer; unsigned char** binaryPointer; void** pointerPointer;
        struct { unsigned int statementAddress; unsigned int statementEbp; } statement; };
    std::uint32_t type;
};
#pragma pack(pop)
static_assert(sizeof(MData)==12);
using ExecuteCommand=void (__cdecl*)(MData*,int,MData*);

struct FieldDesc { std::uint32_t type; std::size_t offset; bool array; };
struct TypeDesc {
    std::uint32_t type; std::size_t size; const FieldDesc* fields; std::size_t fieldCount;
    ExecuteCommand constructor; ExecuteCommand destructor; ExecuteCommand copier; bool enumeration;
};
struct Value {
    std::uint32_t declared=T_NULL, type=T_NULL;
    bool declaredArray=false, missing=false;
    long long integer=0; double number=0;
    std::string text; std::vector<unsigned char> bytes; std::vector<Value> elements;
    std::vector<int> dimensions;
    std::vector<Value> fields; std::vector<unsigned char> object;
    Value()=default;
    Value(const Value& other);
    Value& operator=(const Value& other);
    Value(Value&& other) noexcept=default;
    Value& operator=(Value&& other) noexcept=default;
};
static void ReadObject(Value& value,const void* source);
struct ParamSpec { std::uint32_t type; std::uint32_t state; };
struct Arg {
    Value* reference=nullptr; std::shared_ptr<Value> temporary;
    std::function<Value()> statement;
    static Arg Ref(Value& value) { Arg result; result.reference=&value; return result; }
    static Arg Temp(Value value) { Arg result; result.temporary=std::make_shared<Value>(std::move(value)); return result; }
    static Arg Statement(std::function<Value()> evaluator) { Arg result; result.statement=std::move(evaluator); return result; }
    Value& Get() { return reference ? *reference : *temporary; }
};

extern const TypeDesc* TypeTable();
extern std::size_t TypeCount();
static const TypeDesc* FindType(std::uint32_t type) {
    type &= ~T_ARRAY;
    const TypeDesc* table=TypeTable();
    for(std::size_t index=0;index<TypeCount();++index) if(table[index].type==type) return &table[index];
    return nullptr;
}
static bool Numeric(std::uint32_t type) {
    type &= ~T_ARRAY;
    return type==T_BYTE||type==T_SHORT||type==T_INT||type==T_INT64||type==T_FLOAT||type==T_DOUBLE||type==T_BOOL||type==T_DATE;
}
static Value MakeVar(std::uint32_t type,bool array=false,bool invokeConstructor=true) {
    Value value; value.declared=type; value.type=type; value.declaredArray=array;
    if(array) return value;
    if(const auto* desc=FindType(type)) {
        value.object.resize((std::max)(desc->size,std::size_t(64)));
        for(std::size_t index=0;index<desc->fieldCount;++index) value.fields.push_back(MakeVar(desc->fields[index].type,desc->fields[index].array));
        if(invokeConstructor && !desc->enumeration && desc->constructor) {
            MData receiver{}; receiver.type=type; receiver.pointerValue=value.object.data();
            MData result{}; result.type=type; desc->constructor(&result,1,&receiver);
        }
    }
    return value;
}
static Value Missing() { Value value; value.missing=true; return value; }
static Value Integer(long long number,std::uint32_t type=T_INT) { Value value=MakeVar(type); value.integer=number; value.number=static_cast<double>(number); return value; }
static Value Number(double number) { Value value=MakeVar(T_DOUBLE); value.number=number; value.integer=static_cast<long long>(number); return value; }
static Value Boolean(bool state) { return Integer(state?1:0,T_BOOL); }
static Value Text(std::string text) { Value value=MakeVar(T_TEXT); value.text=std::move(text); return value; }
static Value Bytes(std::vector<unsigned char> bytes) { Value value=MakeVar(T_BIN); value.bytes=std::move(bytes); return value; }
static Value Empty() { return Value{}; }
static Value Exchange(Value& left,Value& right) { if(&left!=&right) std::swap(left,right); return Empty(); }
static Value RuntimeType(const Value& value) { return Integer(static_cast<long long>(value.type|(value.declaredArray?T_ARRAY:0))); }

static long long ToInteger(const Value& value) {
    if(value.type==T_TEXT) return value.text.empty()?0:_strtoi64(value.text.c_str(),nullptr,10);
    if(value.type==T_FLOAT||value.type==T_DOUBLE||value.type==T_DATE) return static_cast<long long>(value.number);
    return value.integer;
}
static double ToNumber(const Value& value) {
    if(value.type==T_TEXT) return value.text.empty()?0:std::strtod(value.text.c_str(),nullptr);
    return (value.type==T_FLOAT||value.type==T_DOUBLE||value.type==T_DATE)?value.number:static_cast<double>(value.integer);
}
static std::string ToText(const Value& value);
static std::wstring RuntimeWide(const std::string& text) {
    if(text.empty())return {};
    const int length=MultiByteToWideChar(CP_ACP,0,text.c_str(),-1,nullptr,0);
    if(length<=1)return {};
    std::wstring result(static_cast<std::size_t>(length),L'\0');
    MultiByteToWideChar(CP_ACP,0,text.c_str(),-1,result.data(),length);
    result.resize(static_cast<std::size_t>(length-1));
    return result;
}
static Value NativeMsgBox(const Value& message,const Value& buttons,const Value& title,const Value& parent) {
    const std::string messageText=message.missing?std::string():ToText(message);
    const std::wstring wideMessage=RuntimeWide(messageText);
    const std::wstring wideTitle=title.missing?RuntimeWide("\320\305\317\242\243\272"):RuntimeWide(ToText(title));
    HWND parentWindow=parent.missing?nullptr:reinterpret_cast<HWND>(static_cast<EPointer>(ToInteger(parent)));
    const int result=MessageBoxW(parentWindow,wideMessage.c_str(),wideTitle.c_str(),static_cast<UINT>(ToInteger(buttons)));
    return Integer(result>0?result-1:result);
}
// Execute the small, position-independent x86 instruction subset commonly
// used by 易语言 "置入代码" snippets.  x64 builds cannot execute those bytes
// natively, so they run against a virtual EBP parameter frame instead.  The
// interpreter deliberately rejects memory operands and opcodes it cannot
// model; silently guessing would be worse than a deterministic compile/run
// diagnostic.
static bool ExecuteX86MachineCode(const unsigned char* code,std::size_t size,std::vector<Value>& parameters,bool combineHighRegister,long long& result) {
    if(code==nullptr && size!=0) return false;
    std::vector<unsigned int> stack(parameters.size(),0);
    for(std::size_t index=0;index<parameters.size();++index) {
        const Value& value=parameters[index];
        if(value.declaredArray || value.type==T_TEXT || value.type==T_BIN || FindType(value.type)!=nullptr) return false;
        stack[index]=static_cast<unsigned int>(ToInteger(value));
    }
    unsigned int regs[8]{}; // eax,ecx,edx,ebx,esp,ebp,esi,edi
    bool zf=false,sf=false,of=false,cf=false,pf=false;
    bool operand16=false;
    auto parity=[&](unsigned int value){
        unsigned int bits=value&0xffu; bits^=bits>>4; bits^=bits>>2; bits^=bits>>1; return (bits&1u)==0;
    };
    auto setFlagsLogic=[&](unsigned int value,unsigned int mask){
        zf=(value&mask)==0; sf=(value&(mask==0xffffu?0x8000u:0x80000000u))!=0; of=false; cf=false; pf=parity(value);
    };
    auto stackIndex=[&](int displacement,std::size_t& index){
        if(displacement<8 || ((displacement-8)&3)!=0) return false;
        index=static_cast<std::size_t>((displacement-8)/4);
        return index<stack.size();
    };
    auto readReg=[&](unsigned int reg){ return regs[reg&7u]; };
    auto writeReg=[&](unsigned int reg,unsigned int value){ regs[reg&7u]=value; };
    auto readReg8=[&](unsigned int reg){
        const unsigned int index=reg&3u;
        return static_cast<unsigned char>((reg&4u)?(regs[index]>>8):(regs[index]&0xffu));
    };
    auto writeReg8=[&](unsigned int reg,unsigned char value){
        const unsigned int index=reg&3u;
        if(reg&4u) regs[index]=(regs[index]&0xffff00ffu)|(static_cast<unsigned int>(value)<<8);
        else regs[index]=(regs[index]&0xffffff00u)|value;
    };
    auto readMemory=[&](int displacement,unsigned int& value){
        std::size_t index=0; if(!stackIndex(displacement,index)) return false; value=stack[index]; return true;
    };
    auto writeMemory=[&](int displacement,unsigned int value){
        std::size_t index=0; if(!stackIndex(displacement,index)) return false; stack[index]=value; return true;
    };
    auto setFlagsAdd=[&](unsigned int left,unsigned int right,unsigned int value){
        const unsigned int mask=operand16?0xffffu:0xffffffffu;
        left&=mask; right&=mask; value&=mask;
        zf=value==0; sf=(value&(operand16?0x8000u:0x80000000u))!=0; pf=parity(value);
        cf=operand16 ? value < left : value < left;
        of=(((~(left^right))&(left^value))&(operand16?0x8000u:0x80000000u))!=0;
    };
    auto setFlagsSub=[&](unsigned int left,unsigned int right,unsigned int value){
        const unsigned int mask=operand16?0xffffu:0xffffffffu;
        left&=mask; right&=mask; value&=mask;
        zf=value==0; sf=(value&(operand16?0x8000u:0x80000000u))!=0; pf=parity(value);
        cf=left<right;
        of=(((left^right)&(left^value))&(operand16?0x8000u:0x80000000u))!=0;
    };
    auto condition=[&](unsigned int codeValue){
        switch(codeValue&0xfu) {
        case 0x0:return of; case 0x1:return !of; case 0x2:return cf; case 0x3:return !cf;
        case 0x4:return zf; case 0x5:return !zf; case 0x6:return cf||zf; case 0x7:return !cf&&!zf;
        case 0x8:return sf; case 0x9:return !sf; case 0xa:return pf; case 0xb:return !pf;
        case 0xc:return sf!=of; case 0xd:return sf==of; case 0xe:return zf||(sf!=of); case 0xf:return !zf&&(sf==of);
        default:return false;
        }
    };
    std::size_t ip=0;
    while(ip<size) {
        operand16=false;
        while(ip<size && code[ip]==0x66u){ operand16=!operand16; ++ip; }
        if(ip>=size) break;
        const unsigned char opcode=code[ip++];
        if(opcode==0x90u || opcode==0xF3u) continue;
        if(opcode>=0xB8u && opcode<=0xBFu) {
            if(ip+4>size) return false;
            unsigned int value=static_cast<unsigned int>(code[ip])|
                (static_cast<unsigned int>(code[ip+1])<<8)|
                (static_cast<unsigned int>(code[ip+2])<<16)|
                (static_cast<unsigned int>(code[ip+3])<<24); ip+=4;
            if(operand16) value&=0xffffu;
            if(operand16) regs[opcode-0xB8u]=(regs[opcode-0xB8u]&0xffff0000u)|value;
            else regs[opcode-0xB8u]=value;
            continue;
        }
        if(opcode>=0x40u && opcode<=0x47u){ const unsigned int mask=operand16?0xffffu:0xffffffffu; regs[opcode-0x40u]=(regs[opcode-0x40u]+1u)&mask; setFlagsAdd(regs[opcode-0x40u]-1u,1u,regs[opcode-0x40u]); continue; }
        if(opcode>=0x48u && opcode<=0x4Fu){ const unsigned int old=regs[opcode-0x48u]; const unsigned int mask=operand16?0xffffu:0xffffffffu; regs[opcode-0x48u]=(old-1u)&mask; setFlagsSub(old,1u,regs[opcode-0x48u]); continue; }
        if(opcode==0xC9u){ continue; }
        if(opcode==0xC3u || opcode==0xCBu || opcode==0xC2u || opcode==0xCAu){ break; }
        if(opcode==0xEBu){ if(ip>=size) return false; const int displacement=static_cast<signed char>(code[ip++]); const std::ptrdiff_t target=static_cast<std::ptrdiff_t>(ip)+displacement; if(target<0||target>static_cast<std::ptrdiff_t>(size)) return false; ip=static_cast<std::size_t>(target); continue; }
        if(opcode>=0x70u && opcode<=0x7Fu){ if(ip>=size) return false; const int displacement=static_cast<signed char>(code[ip++]); if(condition(opcode)){ const std::ptrdiff_t target=static_cast<std::ptrdiff_t>(ip)+displacement; if(target<0||target>static_cast<std::ptrdiff_t>(size)) return false; ip=static_cast<std::size_t>(target); } continue; }
        if(opcode==0xE9u){ if(ip+4>size) return false; const int displacement=static_cast<int>(static_cast<unsigned int>(code[ip])|(static_cast<unsigned int>(code[ip+1])<<8)|(static_cast<unsigned int>(code[ip+2])<<16)|(static_cast<unsigned int>(code[ip+3])<<24)); ip+=4; const std::ptrdiff_t target=static_cast<std::ptrdiff_t>(ip)+displacement; if(target<0||target>static_cast<std::ptrdiff_t>(size)) return false; ip=static_cast<std::size_t>(target); continue; }
        if(opcode==0x0Fu){
            if(ip>=size) return false; const unsigned char second=code[ip++];
            if(second<0x80u||second>0x8Fu||ip+4>size) return false;
            const int displacement=static_cast<int>(static_cast<unsigned int>(code[ip])|(static_cast<unsigned int>(code[ip+1])<<8)|(static_cast<unsigned int>(code[ip+2])<<16)|(static_cast<unsigned int>(code[ip+3])<<24)); ip+=4;
            if(condition(second)){ const std::ptrdiff_t target=static_cast<std::ptrdiff_t>(ip)+displacement; if(target<0||target>static_cast<std::ptrdiff_t>(size)) return false; ip=static_cast<std::size_t>(target); }
            continue;
        }
        if(opcode==0x05u || opcode==0x2Du){
            if(ip+4>size) return false; const unsigned int immediate=static_cast<unsigned int>(code[ip])|(static_cast<unsigned int>(code[ip+1])<<8)|(static_cast<unsigned int>(code[ip+2])<<16)|(static_cast<unsigned int>(code[ip+3])<<24); ip+=4;
            const unsigned int old=regs[0]; const unsigned int value=opcode==0x05u?old+immediate:old-immediate; regs[0]=operand16?((old&0xffff0000u)|(value&0xffffu)):value; if(opcode==0x05u) setFlagsAdd(old,immediate,value); else setFlagsSub(old,immediate,value); continue;
        }
        if(opcode==0xA9u){
            if(ip+4>size) return false; const unsigned int immediate=static_cast<unsigned int>(code[ip])|(static_cast<unsigned int>(code[ip+1])<<8)|(static_cast<unsigned int>(code[ip+2])<<16)|(static_cast<unsigned int>(code[ip+3])<<24); ip+=4; setFlagsLogic(regs[0]&immediate,operand16?0xffffu:0xffffffffu); continue;
        }
        if(opcode==0x31u || opcode==0x33u || opcode==0x85u || opcode==0x89u || opcode==0x8Bu || opcode==0x88u || opcode==0x8Au || opcode==0x03u || opcode==0x2Bu || opcode==0x3Bu || opcode==0x39u || opcode==0x3Au){
            if(ip>=size) return false; const unsigned char modrm=code[ip++]; const unsigned int mod=(modrm>>6)&3u, reg=(modrm>>3)&7u, rm=modrm&7u; int displacement=0;
            if(mod==0u && rm==5u){ if(ip+4>size)return false; displacement=static_cast<int>(static_cast<unsigned int>(code[ip])|(static_cast<unsigned int>(code[ip+1])<<8)|(static_cast<unsigned int>(code[ip+2])<<16)|(static_cast<unsigned int>(code[ip+3])<<24)); ip+=4; }
            else if(mod==1u && rm==5u){ if(ip>=size)return false; displacement=static_cast<signed char>(code[ip++]); }
            else if(mod==2u && rm==5u){ if(ip+4>size)return false; displacement=static_cast<int>(static_cast<unsigned int>(code[ip])|(static_cast<unsigned int>(code[ip+1])<<8)|(static_cast<unsigned int>(code[ip+2])<<16)|(static_cast<unsigned int>(code[ip+3])<<24)); ip+=4; }
            else if(mod==3u){
                const unsigned int left=readReg(reg), right=readReg(rm), value=left^right;
                if(opcode==0x31u||opcode==0x33u){ writeReg(opcode==0x31u?rm:reg,operand16?((readReg(opcode==0x31u?rm:reg)&0xffff0000u)|(value&0xffffu)):value); setFlagsLogic(value,operand16?0xffffu:0xffffffffu); continue; }
                if(opcode==0x39u||opcode==0x3Au){ const unsigned int a=opcode==0x39u?left:right,b=opcode==0x39u?right:left; setFlagsSub(a,b,a-b); continue; }
                return false;
            }
            else return false;
            unsigned int memory=0; if(!readMemory(displacement,memory)) return false;
            const unsigned int mask=operand16?0xffffu:0xffffffffu;
            if(opcode==0x8Bu){ writeReg(reg,operand16?((readReg(reg)&0xffff0000u)|(memory&0xffffu)):memory); continue; }
            if(opcode==0x89u){ if(!writeMemory(displacement,operand16?readReg(reg)&0xffffu:readReg(reg))) return false; continue; }
            if(opcode==0x8Au){ writeReg8(reg,static_cast<unsigned char>(memory&0xffu)); continue; }
            if(opcode==0x88u){ if(!writeMemory(displacement,(memory&0xffffff00u)|readReg8(reg))) return false; continue; }
            if(opcode==0x03u){ const unsigned int old=readReg(reg),value=(old+memory)&mask; writeReg(reg,operand16?((old&0xffff0000u)|(value&0xffffu)):value); setFlagsAdd(old,memory,value); continue; }
            if(opcode==0x2Bu){ const unsigned int old=readReg(reg),value=(old-memory)&mask; writeReg(reg,operand16?((old&0xffff0000u)|(value&0xffffu)):value); setFlagsSub(old,memory,value); continue; }
            if(opcode==0x3Bu){ const unsigned int old=readReg(reg),value=(old-memory)&mask; setFlagsSub(old,memory,value); continue; }
            if(opcode==0x85u){ const unsigned int value=readReg(reg)&memory; setFlagsLogic(value,mask); continue; }
            if(opcode==0x39u||opcode==0x3Au){ const unsigned int a=opcode==0x39u?readReg(reg):memory,b=opcode==0x39u?memory:readReg(reg); setFlagsSub(a,b,a-b); continue; }
            return false;
        }
        if(opcode==0xC1u || opcode==0xD1u || opcode==0xD3u){
            if(ip>=size) return false; const unsigned char modrm=code[ip++]; const unsigned int mod=(modrm>>6)&3u, sub=(modrm>>3)&7u, rm=modrm&7u; if(mod!=3u) return false; unsigned int count=opcode==0xC1u?(ip<size?code[ip++]:0):1; if(opcode==0xD3u) count=regs[1]&0xffu; count&=operand16?15u:31u; if(count==0u) continue; const unsigned int mask=operand16?0xffffu:0xffffffffu; unsigned int value=readReg(rm)&mask; if(sub==0u){ value=((value<<count)|(value>>(operand16?16-count:32-count)))&mask; } else if(sub==1u){ value=((value>>count)|(value<<(operand16?16-count:32-count)))&mask; } else if(sub==4u||sub==6u){ value=(value<<count)&mask; } else if(sub==5u){ value=(value>>count)&mask; } else if(sub==7u){ value=static_cast<unsigned int>(static_cast<int>(value)>>count)&mask; } else return false; writeReg(rm,operand16?((readReg(rm)&0xffff0000u)|value):value); setFlagsLogic(value,mask); continue;
        }
        return false;
    }
    for(std::size_t index=0;index<parameters.size()&&index<stack.size();++index){ parameters[index].integer=static_cast<long long>(static_cast<std::int32_t>(stack[index])); parameters[index].number=static_cast<double>(parameters[index].integer); }
    if(combineHighRegister) {
        const std::uint64_t combined=(static_cast<std::uint64_t>(regs[2])<<32)|regs[0];
        result=static_cast<long long>(combined);
    }
    else {
        result=static_cast<long long>(static_cast<std::int32_t>(regs[0]));
    }
    return true;
}
static bool ToBool(const Value& value) {
    if(value.declaredArray) return !value.elements.empty();
    if(value.type==T_TEXT) return !value.text.empty();
    if(value.type==T_BIN) return !value.bytes.empty();
    return ToNumber(value)!=0;
}
static std::string ToText(const Value& value) {
    if(value.type==T_TEXT) return value.text;
    if(value.type==T_BOOL) return value.integer?"\xD5\xE6":"\xBC\xD9";
    char buffer[96]{};
    if(value.type==T_FLOAT||value.type==T_DOUBLE||value.type==T_DATE) {
        std::snprintf(buffer,sizeof(buffer),"%.15g",value.number);
    } else std::snprintf(buffer,sizeof(buffer),"%lld",value.integer);
    return buffer;
}
static Value ConditionMacro(const Value& value,const char* active) {
    std::string wanted=ToText(value);
    std::transform(wanted.begin(),wanted.end(),wanted.begin(),[](const unsigned char ch){return static_cast<char>(std::toupper(ch));});
    if(wanted.empty()||active==nullptr) return Boolean(false);
    const std::string list(active);
    std::size_t begin=0;
    while(begin<=list.size()) {
        const std::size_t end=list.find(',',begin);
        std::string item=list.substr(begin,end==std::string::npos?std::string::npos:end-begin);
        while(!item.empty()&&std::isspace(static_cast<unsigned char>(item.front()))!=0)item.erase(item.begin());
        while(!item.empty()&&std::isspace(static_cast<unsigned char>(item.back()))!=0)item.pop_back();
        std::transform(item.begin(),item.end(),item.begin(),[](const unsigned char ch){return static_cast<char>(std::toupper(ch));});
        if(item==wanted) return Boolean(true);
        if(end==std::string::npos) break;
        begin=end+1;
    }
    return Boolean(false);
}
static Value ParseHexValue(const Value& value) {
    const std::string text=ToText(value); if(text.empty()) return Integer(0);
    char* end=nullptr; const unsigned long long result=std::strtoull(text.c_str(),&end,16);
    return Integer(end==text.c_str()?0:static_cast<long long>(result));
}
static Value ParseBinaryValue(const Value& value) {
    const std::string text=ToText(value); unsigned long long result=0; bool any=false;
    for(const char ch:text) { if(ch=='0'||ch=='1'){result=(result<<1)|static_cast<unsigned long long>(ch-'0'); any=true;} else if(ch!=' '&&ch!='\t') return Integer(0); }
    return Integer(any?static_cast<long long>(result):0);
}
static Value Convert(Value value,std::uint32_t type) {
    if(type==T_NULL||type==T_ALL) return value;
    if(type==T_TEXT) return Text(ToText(value));
    if(type==T_BOOL) return Boolean(ToBool(value));
    if(type==T_FLOAT||type==T_DOUBLE||type==T_DATE) { Value result=MakeVar(type); result.number=ToNumber(value); result.integer=static_cast<long long>(result.number); return result; }
    if(Numeric(type)) return Integer(ToInteger(value),type);
    value.declared=type; value.type=type; return value;
}
static bool CopyCompound(Value& target,const Value& source) {
    if(target.type!=source.type) return false;
    const auto* descriptor=FindType(target.type);
    if(descriptor==nullptr || descriptor->enumeration || descriptor->copier==nullptr || target.object.empty() || source.object.empty()) return false;
    MData arguments[2]{};
    arguments[0].type=target.type; arguments[0].pointerValue=target.object.data();
    arguments[1].type=source.type; arguments[1].pointerValue=const_cast<unsigned char*>(source.object.data());
    MData result{}; result.type=target.type;
    descriptor->copier(&result,2,arguments);
    return true;
}
static Value& Assign(Value& target,Value value) {
    if(target.declaredArray) { target=std::move(value); target.declaredArray=true; return target; }
    const auto declared=target.declared;
    if(declared!=T_NULL && !target.declaredArray && !value.declaredArray && CopyCompound(target,value)) {
        if(const auto* descriptor=FindType(target.type)) {
            for(std::size_t index=0;index<descriptor->fieldCount&&index<target.fields.size();++index) target.fields[index]=MakeVar(descriptor->fields[index].type,descriptor->fields[index].array,false);
            ReadObject(target,target.object.data());
        }
        return target;
    }
    if(declared!=T_NULL) value=Convert(std::move(value),declared);
    target=std::move(value); target.declared=declared==T_NULL?target.type:declared; return target;
}
static Value Add(const Value& a,const Value& b) {
    if(a.type==T_BIN || b.type==T_BIN) {
        std::vector<unsigned char> bytes;
        bytes.reserve(a.bytes.size()+b.bytes.size());
        bytes.insert(bytes.end(),a.bytes.begin(),a.bytes.end());
        bytes.insert(bytes.end(),b.bytes.begin(),b.bytes.end());
        return Bytes(std::move(bytes));
    }
    return (a.type==T_TEXT||b.type==T_TEXT)?Text(ToText(a)+ToText(b)):Number(ToNumber(a)+ToNumber(b));
}
static Value Sub(const Value& a,const Value& b) { return Number(ToNumber(a)-ToNumber(b)); }
static Value Mul(const Value& a,const Value& b) { return Number(ToNumber(a)*ToNumber(b)); }
static Value Div(const Value& a,const Value& b) { const double d=ToNumber(b); return Number(d==0?0:ToNumber(a)/d); }
static Value IDiv(const Value& a,const Value& b) { const auto d=ToInteger(b); return Integer(d==0?0:ToInteger(a)/d); }
static Value Mod(const Value& a,const Value& b) { const auto d=ToInteger(b); return Integer(d==0?0:ToInteger(a)%d); }
static Value Neg(const Value& a) { return Number(-ToNumber(a)); }
static Value Eq(const Value& a,const Value& b) { return Boolean((a.type==T_TEXT||b.type==T_TEXT)?ToText(a)==ToText(b):ToNumber(a)==ToNumber(b)); }
static Value Ne(const Value& a,const Value& b) { return Boolean(!ToBool(Eq(a,b))); }
static Value Lt(const Value& a,const Value& b) { return Boolean((a.type==T_TEXT||b.type==T_TEXT)?ToText(a)<ToText(b):ToNumber(a)<ToNumber(b)); }
static Value Le(const Value& a,const Value& b) { return Boolean(ToBool(Lt(a,b))||ToBool(Eq(a,b))); }
static Value Gt(const Value& a,const Value& b) { return Boolean(!ToBool(Le(a,b))); }
static Value Ge(const Value& a,const Value& b) { return Boolean(!ToBool(Lt(a,b))); }
static Value And(const Value& a,const Value& b) { return Boolean(ToBool(a)&&ToBool(b)); }
static Value Or(const Value& a,const Value& b) { return Boolean(ToBool(a)||ToBool(b)); }
static Value Not(const Value& a) { return Boolean(!ToBool(a)); }

static Value& Index(Value& value,long long index) {
    if(index<1||static_cast<std::size_t>(index)>value.elements.size()) {
        std::fputs("ecompiler: array index out of bounds\r\n",stderr); OutputDebugStringA("ecompiler: array index out of bounds\r\n"); ExitProcess(87);
    }
    return value.elements[static_cast<std::size_t>(index-1)];
}
static Value& IndexPath(Value& value,const std::vector<long long>& indexes) {
    if(indexes.empty()) return value;
    if(value.dimensions.empty() && indexes.size()==1) return Index(value,indexes.front());
    if(indexes.size()!=value.dimensions.size()) {
        std::fputs("ecompiler: array dimension mismatch\r\n",stderr); OutputDebugStringA("ecompiler: array dimension mismatch\r\n"); ExitProcess(87);
    }
    std::size_t flat=0;
    std::size_t stride=1;
    for(std::size_t reverse=0;reverse<indexes.size();++reverse) {
        const std::size_t dimensionIndex=indexes.size()-1-reverse;
        const long long index=indexes[dimensionIndex];
        const int dimension=value.dimensions[dimensionIndex];
        if(index<1 || index>dimension) { std::fputs("ecompiler: array index out of bounds\r\n",stderr); OutputDebugStringA("ecompiler: array index out of bounds\r\n"); ExitProcess(87); }
        flat += static_cast<std::size_t>(index-1)*stride;
        stride*=static_cast<std::size_t>(dimension);
    }
    if(flat>=value.elements.size()) { std::fputs("ecompiler: array index out of bounds\r\n",stderr); OutputDebugStringA("ecompiler: array index out of bounds\r\n"); ExitProcess(87); }
    return value.elements[flat];
}
static Value& Field(Value& value,std::size_t index) {
    if(index>=value.fields.size()) { std::fputs("ecompiler: compound field out of bounds\r\n",stderr); OutputDebugStringA("ecompiler: compound field out of bounds\r\n"); ExitProcess(87); }
    return value.fields[index];
}
static void Redim(Value& value,const std::vector<int>& dimensions,bool preserve) {
    std::vector<int> normalized=dimensions;
    for(int& dimension:normalized) dimension=(std::max)(dimension,0);
    std::size_t count=1; for(int dimension:normalized) count*=static_cast<std::size_t>(dimension);
    std::vector<Value> old=preserve?std::move(value.elements):std::vector<Value>{};
    value.elements.clear(); value.elements.reserve(count); value.declaredArray=true; value.dimensions=std::move(normalized);
    for(std::size_t i=0;i<count;++i) {
        if(i<old.size()) value.elements.push_back(std::move(old[i]));
        else value.elements.push_back(MakeVar(value.declared));
    }
}
static Value AryCount(const Value& value) { return Integer(value.declaredArray?static_cast<long long>(value.elements.size()):-1); }
static Value CopyAry(Value& source,Value& target) { target.elements=source.elements; target.dimensions=source.dimensions; target.declared=source.declared; target.type=source.type; target.declaredArray=true; return Empty(); }
static Value AddElement(Value& target,Value item) { Value value=MakeVar(target.declared); Assign(value,std::move(item)); target.elements.push_back(std::move(value)); target.declaredArray=true; if(target.dimensions.size()==1) target.dimensions[0]=static_cast<int>(target.elements.size()); else if(target.dimensions.empty()) target.dimensions={static_cast<int>(target.elements.size())}; return Empty(); }
static Value InsertElement(Value& target,int position,Value item) {
    position=(std::max)(1,(std::min)(position,static_cast<int>(target.elements.size()+1)));
    Value value=MakeVar(target.declared); Assign(value,std::move(item)); target.elements.insert(target.elements.begin()+position-1,std::move(value)); target.declaredArray=true; if(target.dimensions.size()==1) target.dimensions[0]=static_cast<int>(target.elements.size()); else if(target.dimensions.empty()) target.dimensions={static_cast<int>(target.elements.size())}; return Empty();
}
static Value RemoveElement(Value& target,int position,int count) {
    if(position<1||position>static_cast<int>(target.elements.size())||count<=0) return Integer(0);
    count=(std::min)(count,static_cast<int>(target.elements.size())-position+1);
    target.elements.erase(target.elements.begin()+position-1,target.elements.begin()+position-1+count); if(target.dimensions.size()==1) target.dimensions[0]=static_cast<int>(target.elements.size()); return Integer(count);
}
static Value RemoveAll(Value& target) { target.elements.clear(); target.dimensions.clear(); target.declaredArray=true; return Empty(); }
static Value FindFile(const Value& pattern) {
    static HANDLE search=INVALID_HANDLE_VALUE; static WIN32_FIND_DATAA data{}; static bool hasCurrent=false;
    const std::string text=ToText(pattern);
    if(!text.empty()) {
        if(search!=INVALID_HANDLE_VALUE) FindClose(search);
        search=FindFirstFileA(text.c_str(),&data); hasCurrent=search!=INVALID_HANDLE_VALUE;
    }
    while(hasCurrent) {
        const std::string name=data.cFileName;
        hasCurrent=FindNextFileA(search,&data)!=FALSE;
        if(name!="."&&name!="..") return Text(name);
    }
    if(search!=INVALID_HANDLE_VALUE) { FindClose(search); search=INVALID_HANDLE_VALUE; }
    return Text("");
}
extern "C" EIntPtr __stdcall BlackMoonFuncForeLibNotifySys(int,EPointer,EPointer);
static void RuntimeFree(void* pointer) {
    if(pointer) BlackMoonFuncForeLibNotifySys(ecompiler_nrs_mfree,reinterpret_cast<EPointer>(pointer),0);
}
static void RuntimeFreeArray(std::uint32_t type,void* pointer) { if(pointer) BlackMoonFuncForeLibNotifySys(ecompiler_nrs_free_array,type,reinterpret_cast<EPointer>(pointer)); }
static char* RuntimeText(const std::string& text) {
    auto* result=reinterpret_cast<char*>(BlackMoonFuncForeLibNotifySys(ecompiler_nrs_malloc,static_cast<EPointer>(text.size()+1),0));
    if(result!=nullptr) std::memcpy(result,text.c_str(),text.size()+1); return result;
}
static unsigned char* RuntimeBinary(const std::vector<unsigned char>& bytes) {
    auto* result=reinterpret_cast<unsigned char*>(BlackMoonFuncForeLibNotifySys(ecompiler_nrs_malloc,static_cast<EPointer>(bytes.size()+8),0));
    if(result==nullptr) return nullptr;
    *reinterpret_cast<int*>(result)=1; *reinterpret_cast<int*>(result+4)=static_cast<int>(bytes.size());
    if(!bytes.empty()) std::memcpy(result+8,bytes.data(),bytes.size()); return result;
}
static std::size_t ScalarSize(std::uint32_t type) {
    switch(type&~T_ARRAY) { case T_BYTE:return 1; case T_SHORT:return 2; case T_INT:case T_FLOAT:case T_BOOL:return 4; case T_TEXT:case T_BIN:case T_SUB:return sizeof(void*); case T_INT64:case T_DOUBLE:case T_DATE:return 8; default:return 4; }
}
static void ReadObject(Value& value,const void* source);
static void ReadArray(Value& value,std::uint32_t type,const unsigned char* raw) {
    value.declaredArray=true;
    value.elements.clear();
    if(raw==nullptr) return;
    const int dimensions=*reinterpret_cast<const int*>(raw);
    if(dimensions<=0 || dimensions>32) return;
    int count=1;
    value.dimensions.clear();
    for(int index=0;index<dimensions;++index) {
        const int dimension=*reinterpret_cast<const int*>(raw+4+index*4);
        value.dimensions.push_back(dimension);
        count*=dimension;
    }
    const unsigned char* data=raw+4+dimensions*4;
    const std::uint32_t base=type&~T_ARRAY;
    const std::size_t itemSize=(base==T_TEXT||base==T_BIN||FindType(base))?sizeof(void*):ScalarSize(base);
    for(int index=0;index<count;++index) {
        Value item=MakeVar(base);
        if(const auto* descriptor=FindType(base); descriptor!=nullptr && descriptor->enumeration) {
            item.integer=*reinterpret_cast<const int*>(data);
            value.elements.push_back(std::move(item));
            data+=itemSize;
            continue;
        }
        switch(base) {
        case T_BYTE:item.integer=*data;break;
        case T_SHORT:item.integer=*reinterpret_cast<const short*>(data);break;
        case T_INT:case T_BOOL:item.integer=*reinterpret_cast<const int*>(data);break;
        case T_SUB:item.integer=static_cast<long long>(reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void* const*>(data)));break;
        case T_INT64:item.integer=*reinterpret_cast<const long long*>(data);break;
        case T_FLOAT:item.number=*reinterpret_cast<const float*>(data);item.integer=static_cast<long long>(item.number);break;
        case T_DOUBLE:case T_DATE:item.number=*reinterpret_cast<const double*>(data);item.integer=static_cast<long long>(item.number);break;
        case T_TEXT:{const auto* text=*reinterpret_cast<char* const*>(data);if(text)item.text=text;break;}
        case T_BIN:{const auto* bytes=*reinterpret_cast<unsigned char* const*>(data);if(bytes){const int nestedDimensions=*reinterpret_cast<const int*>(bytes);int nestedCount=1;for(int nested=0;nested<nestedDimensions;++nested)nestedCount*=*reinterpret_cast<const int*>(bytes+4+nested*4);const auto* nestedData=bytes+4+nestedDimensions*4;item.bytes.assign(nestedData,nestedData+nestedCount);}break;}
        default:{const auto* object=*reinterpret_cast<void* const*>(data);if(object)ReadObject(item,object);break;}
        }
        value.elements.push_back(std::move(item));
        data+=itemSize;
    }
}
static void ReadObject(Value& value,const void* source) {
    if(source==nullptr) return;
    const auto* descriptor=FindType(value.type);
    if(descriptor==nullptr) return;
    if(value.object.size()<(std::max)(descriptor->size,std::size_t(64))) value.object.resize((std::max)(descriptor->size,std::size_t(64)));
    std::memcpy(value.object.data(),source,(std::min)(value.object.size(),descriptor->size));
    if(value.fields.size()<descriptor->fieldCount) for(std::size_t index=value.fields.size();index<descriptor->fieldCount;++index) value.fields.push_back(MakeVar(descriptor->fields[index].type,descriptor->fields[index].array,false));
    for(std::size_t index=0;index<descriptor->fieldCount;++index) {
        auto& field=value.fields[index];
        const auto& info=descriptor->fields[index];
        const auto* address=static_cast<const unsigned char*>(source)+info.offset;
        if(info.array) { ReadArray(field,info.type,*reinterpret_cast<unsigned char* const*>(address)); continue; }
        switch(info.type) {
        case T_BYTE:field.integer=*address;break;
        case T_SHORT:field.integer=*reinterpret_cast<const short*>(address);break;
        case T_INT:case T_BOOL:field.integer=*reinterpret_cast<const int*>(address);break;
        case T_INT64:field.integer=*reinterpret_cast<const long long*>(address);break;
        case T_FLOAT:field.number=*reinterpret_cast<const float*>(address);field.integer=static_cast<long long>(field.number);break;
        case T_DOUBLE:case T_DATE:field.number=*reinterpret_cast<const double*>(address);field.integer=static_cast<long long>(field.number);break;
        case T_TEXT:{const auto* text=*reinterpret_cast<char* const*>(address);field.text=text==nullptr?std::string():text;break;}
        case T_BIN:{const auto* bytes=*reinterpret_cast<unsigned char* const*>(address);if(bytes){const int dimensions=*reinterpret_cast<const int*>(bytes);int count=1;for(int dimension=0;dimension<dimensions;++dimension)count*=*reinterpret_cast<const int*>(bytes+4+dimension*4);const auto* data=bytes+4+dimensions*4;field.bytes.assign(data,data+count);}else field.bytes.clear();break;}
        default:{const auto* object=*reinterpret_cast<void* const*>(address);if(object)ReadObject(field,object);break;}
        }
    }
}
static void PrepareObject(Value& value) {
    const auto* desc=FindType(value.type); if(!desc) return;
    if(value.object.size()<(std::max)(desc->size,std::size_t(64))) value.object.resize((std::max)(desc->size,std::size_t(64)));
    for(std::size_t i=0;i<desc->fieldCount&&i<value.fields.size();++i) {
        auto& field=value.fields[i]; const auto& info=desc->fields[i]; unsigned char* dest=value.object.data()+info.offset;
        if(info.array) { *reinterpret_cast<void**>(dest)=nullptr; continue; }
        switch(info.type) {
        case T_BYTE:*dest=static_cast<unsigned char>(ToInteger(field));break; case T_SHORT:*reinterpret_cast<short*>(dest)=static_cast<short>(ToInteger(field));break;
        case T_INT:case T_BOOL:*reinterpret_cast<int*>(dest)=static_cast<int>(ToInteger(field));break; case T_INT64:*reinterpret_cast<long long*>(dest)=ToInteger(field);break;
        case T_FLOAT:*reinterpret_cast<float*>(dest)=static_cast<float>(ToNumber(field));break; case T_DOUBLE:case T_DATE:*reinterpret_cast<double*>(dest)=ToNumber(field);break;
        case T_TEXT:*reinterpret_cast<char**>(dest)=field.text.empty()?nullptr:field.text.data();break;
        default: PrepareObject(field); *reinterpret_cast<void**>(dest)=field.object.empty()?nullptr:field.object.data(); break;
        }
    }
}
struct Writeback {
    Value* value=nullptr;
    std::uint32_t type=T_NULL;
    char** textSlot=nullptr;
    unsigned char** binarySlot=nullptr;
    void* arraySlot=nullptr;
    void* scalarSlot=nullptr;
    void* originalArray=nullptr;
    bool arrayDataOnly=false;
    void** objectSlot=nullptr;
    void* objectData=nullptr;
};
struct OwnedArray { std::uint32_t type=T_NULL; void* pointer=nullptr; };
struct Arena {
    std::deque<std::vector<unsigned char>> blocks;
    std::vector<Writeback> writebacks;
    std::vector<OwnedArray> ownedArrays;
    std::vector<void*> ownedValues;
    std::unordered_set<void*> releasedArrays;
};
static void PrepareObjectWithArena(Value& value,Arena& arena);
static void* RuntimeAlloc(std::size_t size) {
    return reinterpret_cast<void*>(static_cast<EPointer>(BlackMoonFuncForeLibNotifySys(ecompiler_nrs_malloc,static_cast<EPointer>(size),0)));
}
static void* MarshalArray(Value& value,std::uint32_t type,Arena& arena,bool track=true) {
    const auto base=type&~T_ARRAY; const std::size_t itemSize=(base==T_TEXT||base==T_BIN||FindType(base))?sizeof(void*):ScalarSize(base);
    const std::vector<int> dimensions=value.dimensions.empty()?std::vector<int>{static_cast<int>(value.elements.size())}:value.dimensions;
    std::size_t dimensionBytes=4+dimensions.size()*4;
    auto* block=static_cast<unsigned char*>(RuntimeAlloc(dimensionBytes+itemSize*value.elements.size()));
    if(block==nullptr) return nullptr;
    *reinterpret_cast<int*>(block)=static_cast<int>(dimensions.size());
    for(std::size_t index=0;index<dimensions.size();++index) *reinterpret_cast<int*>(block+4+index*4)=dimensions[index];
    unsigned char* dest=block+dimensionBytes;
    if(track) arena.ownedArrays.push_back({base,block});
    for(auto& item:value.elements) {
        if(const auto* descriptor=FindType(base); descriptor!=nullptr && descriptor->enumeration) {
            *reinterpret_cast<int*>(dest)=static_cast<int>(ToInteger(item));
            dest+=itemSize;
            continue;
        }
        switch(base) {
        case T_BYTE:*dest=static_cast<unsigned char>(ToInteger(item));break; case T_SHORT:*reinterpret_cast<short*>(dest)=static_cast<short>(ToInteger(item));break;
        case T_INT:case T_BOOL:*reinterpret_cast<int*>(dest)=static_cast<int>(ToInteger(item));break; case T_INT64:*reinterpret_cast<long long*>(dest)=ToInteger(item);break;
        case T_FLOAT:*reinterpret_cast<float*>(dest)=static_cast<float>(ToNumber(item));break; case T_DOUBLE:case T_DATE:*reinterpret_cast<double*>(dest)=ToNumber(item);break;
        case T_TEXT:*reinterpret_cast<char**>(dest)=item.text.empty()?nullptr:RuntimeText(item.text);break;
        case T_BIN:*reinterpret_cast<void**>(dest)=MarshalArray(item,T_BYTE|T_ARRAY,arena,false);break;
        default: PrepareObjectWithArena(item,arena); *reinterpret_cast<void**>(dest)=item.object.data();break;
        }
        dest+=itemSize;
    }
    return block;
}
static void PrepareObjectWithArena(Value& value,Arena& arena) {
    PrepareObject(value);
    const auto* descriptor=FindType(value.type); if(descriptor==nullptr) return;
    for(std::size_t index=0;index<descriptor->fieldCount&&index<value.fields.size();++index) {
        auto& field=value.fields[index]; const auto& info=descriptor->fields[index];
        auto* destination=value.object.data()+info.offset;
        if(info.array) {
            *reinterpret_cast<void**>(destination)=MarshalArray(field,info.type|T_ARRAY,arena);
        } else if(FindType(info.type)!=nullptr) {
            PrepareObjectWithArena(field,arena);
            *reinterpret_cast<void**>(destination)=field.object.empty()?nullptr:field.object.data();
        }
    }
}
static void MarshalValue(Value& value,std::uint32_t expected,MData& out,Arena& arena) {
    std::memset(&out,0,sizeof(out)); if(value.missing) return;
    // Some historical x86 FNEs describe an address as an integer. Preserve a
    // real subroutine/native pointer instead of truncating it on x64; support
    // libraries can then opt into the pointer ABI by inspecting T_SUB.
    if(expected==T_INT && value.type==T_SUB) {
        out.type=T_SUB;
        out.pointerValue=reinterpret_cast<void*>(static_cast<EPointer>(ToInteger(value)));
        return;
    }
    std::uint32_t type=expected==T_ALL||expected==T_NULL?value.type:expected;
    if(value.declaredArray) { out.type=type|T_ARRAY; out.pointerValue=MarshalArray(value,type|T_ARRAY,arena); return; }
    out.type=type;
    if(const auto* descriptor=FindType(type); descriptor!=nullptr && descriptor->enumeration) {
        out.type=T_INT;
        out.intValue=static_cast<int>(ToInteger(value));
        return;
    }
    switch(type) {
    case T_BYTE:out.byteValue=static_cast<unsigned char>(ToInteger(value));break; case T_SHORT:out.shortValue=static_cast<short>(ToInteger(value));break;
    case T_INT:case T_BOOL:out.intValue=static_cast<int>(ToInteger(value));break;
    case T_SUB:out.pointerValue=reinterpret_cast<void*>(static_cast<EPointer>(ToInteger(value)));break;
    case T_INT64:out.int64Value=ToInteger(value);break;
    case T_FLOAT:out.floatValue=static_cast<float>(ToNumber(value));break; case T_DOUBLE:case T_DATE:out.doubleValue=ToNumber(value);break;
    case T_TEXT:out.textValue=value.text.empty()?nullptr:value.text.data();break;
    case T_BIN: { auto* block=static_cast<unsigned char*>(RuntimeAlloc(8+value.bytes.size())); if(block==nullptr)break; *reinterpret_cast<int*>(block)=1; *reinterpret_cast<int*>(block+4)=static_cast<int>(value.bytes.size()); if(!value.bytes.empty())std::memcpy(block+8,value.bytes.data(),value.bytes.size()); out.pointerValue=block; arena.ownedValues.push_back(block); break; }
    default:
        PrepareObjectWithArena(value,arena); out.pointerValue=value.object.data();
        Writeback objectWriteback; objectWriteback.value=&value; objectWriteback.type=type; objectWriteback.objectData=value.object.data(); arena.writebacks.push_back(objectWriteback);
        break;
    }
}
static void MarshalByReference(Value& value,std::uint32_t expected,std::uint32_t state,MData& out,Arena& arena) {
    const std::uint32_t type=(expected==T_ALL||expected==T_NULL)?value.type:expected;
    std::memset(&out,0,sizeof(out)); out.type=type|T_ARRAY;
    if(value.declaredArray || (state&(1u<<5))!=0) {
        out.type=type|T_ARRAY;
        void* arrayData=MarshalArray(value,type|T_ARRAY,arena);
        if((state&(1u<<5))!=0) {
            out.pointerValue=arrayData;
            arena.writebacks.push_back({&value,type,nullptr,nullptr,arrayData,nullptr,arrayData,true});
        } else {
            arena.blocks.emplace_back(sizeof(void*));
            auto** arraySlot=reinterpret_cast<void**>(arena.blocks.back().data());
            *arraySlot=arrayData;
            out.pointerPointer=arraySlot;
            arena.writebacks.push_back({&value,type,nullptr,nullptr,arraySlot,nullptr,arrayData,false});
        }
        return;
    }
    out.type=type|T_VARIABLE;
    arena.blocks.emplace_back(8);
    auto& scalar=arena.blocks.back();
    if(const auto* descriptor=FindType(type); descriptor!=nullptr && descriptor->enumeration) {
        out.type=T_INT|T_VARIABLE;
        *reinterpret_cast<int*>(scalar.data())=static_cast<int>(ToInteger(value));
        out.intPointer=reinterpret_cast<int*>(scalar.data());
        arena.writebacks.push_back({&value,type,nullptr,nullptr,nullptr,scalar.data()});
        return;
    }
    switch(type) {
    case T_BYTE: *reinterpret_cast<unsigned char*>(scalar.data())=static_cast<unsigned char>(ToInteger(value)); out.bytePointer=scalar.data(); break;
    case T_SHORT: *reinterpret_cast<short*>(scalar.data())=static_cast<short>(ToInteger(value)); out.shortPointer=reinterpret_cast<short*>(scalar.data()); break;
    case T_INT: case T_BOOL: *reinterpret_cast<int*>(scalar.data())=static_cast<int>(ToInteger(value)); out.intPointer=reinterpret_cast<int*>(scalar.data()); break;
    case T_SUB: *reinterpret_cast<std::uintptr_t*>(scalar.data())=static_cast<std::uintptr_t>(ToInteger(value)); out.pointerPointer=reinterpret_cast<void**>(scalar.data()); break;
    case T_INT64: *reinterpret_cast<long long*>(scalar.data())=ToInteger(value); out.int64Pointer=reinterpret_cast<long long*>(scalar.data()); break;
    case T_FLOAT: *reinterpret_cast<float*>(scalar.data())=static_cast<float>(ToNumber(value)); out.floatPointer=reinterpret_cast<float*>(scalar.data()); break;
    case T_DOUBLE: case T_DATE: *reinterpret_cast<double*>(scalar.data())=ToNumber(value); out.doublePointer=reinterpret_cast<double*>(scalar.data()); break;
    case T_TEXT: {
        arena.blocks.emplace_back(sizeof(char*));
        auto** slot=reinterpret_cast<char**>(arena.blocks.back().data());
        *slot=RuntimeText(value.text); out.textPointer=slot;
        arena.writebacks.push_back({&value,type,slot,nullptr,nullptr,nullptr});
        break;
    }
    case T_BIN: {
        arena.blocks.emplace_back(sizeof(unsigned char*));
        auto** slot=reinterpret_cast<unsigned char**>(arena.blocks.back().data());
        *slot=RuntimeBinary(value.bytes); out.binaryPointer=slot;
        arena.writebacks.push_back({&value,type,nullptr,slot,nullptr,nullptr});
        break;
    }
    default:
        PrepareObjectWithArena(value,arena);
        arena.blocks.emplace_back(sizeof(void*));
        auto** objectSlot=reinterpret_cast<void**>(arena.blocks.back().data());
        *objectSlot=value.object.data();
        out.pointerPointer=objectSlot;
        Writeback objectWriteback;
        objectWriteback.value=&value;
        objectWriteback.type=type;
        objectWriteback.objectSlot=objectSlot;
        arena.writebacks.push_back(objectWriteback);
        break;
    }
    if(type!=T_TEXT && type!=T_BIN) arena.writebacks.push_back({&value,type,nullptr,nullptr,nullptr,scalar.data()});
}
static void ApplyWritebacks(Arena& arena) {
    for(auto& item:arena.writebacks) {
        if(!item.value) continue;
        if(item.originalArray!=nullptr) {
            // A variable-array callee owns the original slot and may replace it.
            // Mark it consumed before looking at a possible replacement so the
            // arena never frees a pointer twice.
            arena.releasedArrays.insert(item.originalArray);
            unsigned char* raw=item.arrayDataOnly?static_cast<unsigned char*>(item.arraySlot):(item.arraySlot?*reinterpret_cast<unsigned char**>(item.arraySlot):nullptr);
            if(raw) {
                const std::uint32_t type=item.type;
                int dimensions=*reinterpret_cast<int*>(raw), count=1;
                item.value->dimensions.clear();
                for(int i=0;i<dimensions;++i) {
                    const int dimension=*reinterpret_cast<int*>(raw+4+i*4);
                    item.value->dimensions.push_back(dimension);
                    count*=dimension;
                }
                unsigned char* source=static_cast<unsigned char*>(raw)+4+dimensions*4;
                const std::size_t itemSize=(type==T_TEXT||type==T_BIN||FindType(type))?sizeof(void*):ScalarSize(type);
                item.value->elements.clear(); item.value->declaredArray=true;
                for(int index=0;index<count;++index) {
                    Value element=MakeVar(type);
                    if(const auto* descriptor=FindType(type); descriptor!=nullptr && descriptor->enumeration) {
                        element.integer=*reinterpret_cast<int*>(source);
                        item.value->elements.push_back(std::move(element));
                        source+=itemSize;
                        continue;
                    }
                    switch(type) {
                    case T_BYTE: element.integer=*source; break;
                    case T_SHORT: element.integer=*reinterpret_cast<short*>(source); break;
                    case T_INT: case T_BOOL: element.integer=*reinterpret_cast<int*>(source); break;
                    case T_INT64: element.integer=*reinterpret_cast<long long*>(source); break;
                    case T_FLOAT: element.number=*reinterpret_cast<float*>(source); element.integer=static_cast<long long>(element.number); break;
                    case T_DOUBLE: case T_DATE: element.number=*reinterpret_cast<double*>(source); element.integer=static_cast<long long>(element.number); break;
                    case T_TEXT: { auto* text=*reinterpret_cast<char**>(source); if(text) element.text=text; break; }
                    case T_BIN: {
                        auto* bytes=*reinterpret_cast<unsigned char**>(source);
                        if(bytes!=nullptr) {
                            const int nestedDimensions=*reinterpret_cast<int*>(bytes);
                            int nestedCount=1;
                            for(int nested=0;nested<nestedDimensions;++nested) nestedCount*=*reinterpret_cast<int*>(bytes+4+nested*4);
                            const auto* nestedData=bytes+4+nestedDimensions*4;
                            element.bytes.assign(nestedData,nestedData+nestedCount);
                        }
                        break;
                    }
                    default: break;
                    }
                    item.value->elements.push_back(std::move(element)); source+=itemSize;
                }
                if(raw!=nullptr) {
                    RuntimeFreeArray(type,raw);
                    arena.releasedArrays.insert(raw);
                }
            }
            continue;
        }
        switch(item.type) {
        case T_TEXT: if(item.textSlot && *item.textSlot){item.value->text=*item.textSlot; RuntimeFree(*item.textSlot);} break;
        case T_BIN: if(item.binarySlot && *item.binarySlot){auto* raw=*item.binarySlot;int dimensions=*reinterpret_cast<int*>(raw),count=1;for(int i=0;i<dimensions;++i)count*=*reinterpret_cast<int*>(raw+4+i*4);item.value->bytes.assign(raw+4+dimensions*4,raw+4+dimensions*4+count);RuntimeFree(raw);} break;
        case T_BYTE: item.value->integer=*reinterpret_cast<unsigned char*>(item.scalarSlot); break;
        case T_SHORT: item.value->integer=*reinterpret_cast<short*>(item.scalarSlot); break;
        case T_INT: case T_BOOL: item.value->integer=*reinterpret_cast<int*>(item.scalarSlot); break;
        case T_SUB: item.value->integer=static_cast<long long>(*reinterpret_cast<std::uintptr_t*>(item.scalarSlot)); break;
        case T_INT64: item.value->integer=*reinterpret_cast<long long*>(item.scalarSlot); break;
        case T_FLOAT: item.value->number=*reinterpret_cast<float*>(item.scalarSlot); item.value->integer=static_cast<long long>(item.value->number); break;
        case T_DOUBLE: case T_DATE: item.value->number=*reinterpret_cast<double*>(item.scalarSlot); item.value->integer=static_cast<long long>(item.value->number); break;
        default:
            if(const auto* descriptor=FindType(item.type); descriptor!=nullptr && descriptor->enumeration) item.value->integer=*reinterpret_cast<int*>(item.scalarSlot);
            else if(item.objectSlot && *item.objectSlot) ReadObject(*item.value,*item.objectSlot);
            else if(item.objectData) ReadObject(*item.value,item.objectData);
            break;
        }
    }
}
static Value CopyReturned(MData& data,std::uint32_t fallback,bool returnsArray) {
    std::uint32_t type=data.type?data.type:fallback; returnsArray=returnsArray||((type&T_ARRAY)!=0); type&=~T_ARRAY;
    if(returnsArray) {
        Value result=MakeVar(type,true); auto* raw=static_cast<unsigned char*>(data.pointerValue); if(!raw)return result;
        int dimensions=*reinterpret_cast<int*>(raw), count=1;
        result.dimensions.clear();
        for(int i=0;i<dimensions;++i) { const int dimension=*reinterpret_cast<int*>(raw+4+i*4); result.dimensions.push_back(dimension); count*=dimension; }
        unsigned char* source=raw+4+dimensions*4; const std::size_t itemSize=(type==T_TEXT||type==T_BIN||FindType(type))?sizeof(void*):ScalarSize(type);
        for(int i=0;i<count;++i) { Value item=MakeVar(type); if(const auto* descriptor=FindType(type); descriptor!=nullptr && descriptor->enumeration) { item.integer=*reinterpret_cast<int*>(source); result.elements.push_back(std::move(item)); source+=itemSize; continue; } switch(type) {
            case T_BYTE:item.integer=*source;break; case T_SHORT:item.integer=*reinterpret_cast<short*>(source);break; case T_INT:case T_BOOL:item.integer=*reinterpret_cast<int*>(source);break;
            case T_INT64:item.integer=*reinterpret_cast<long long*>(source);break; case T_FLOAT:item.number=*reinterpret_cast<float*>(source);break; case T_DOUBLE:case T_DATE:item.number=*reinterpret_cast<double*>(source);break;
            case T_TEXT:{auto* text=*reinterpret_cast<char**>(source);if(text)item.text=text;break;}
            case T_BIN:{auto* bytes=*reinterpret_cast<unsigned char**>(source);if(bytes){int nestedDimensions=*reinterpret_cast<int*>(bytes),nestedCount=1;for(int nested=0;nested<nestedDimensions;++nested)nestedCount*=*reinterpret_cast<int*>(bytes+4+nested*4);item.bytes.assign(bytes+4+nestedDimensions*4,bytes+4+nestedDimensions*4+nestedCount);}break;}
            default:{auto* object=*reinterpret_cast<void**>(source);if(object)ReadObject(item,object);break;} }
            result.elements.push_back(std::move(item)); source+=itemSize;
        }
        RuntimeFreeArray(type,raw); return result;
    }
    Value result=MakeVar(type,false,false); if(const auto* descriptor=FindType(type); descriptor!=nullptr && descriptor->enumeration) { result.integer=data.intValue; return result; } switch(type) {
    case T_BYTE:result.integer=data.byteValue;break; case T_SHORT:result.integer=data.shortValue;break; case T_INT:case T_BOOL:result.integer=data.intValue;break;
    case T_SUB:result.integer=static_cast<long long>(reinterpret_cast<std::uintptr_t>(data.pointerValue));break;
    case T_INT64:result.integer=data.int64Value;break; case T_FLOAT:result.number=data.floatValue;break; case T_DOUBLE:case T_DATE:result.number=data.doubleValue;break;
    case T_TEXT:if(data.textValue){result.text=data.textValue;RuntimeFree(data.textValue);}break;
    case T_BIN:if(data.pointerValue){auto* raw=static_cast<unsigned char*>(data.pointerValue);int dimensions=*reinterpret_cast<int*>(raw),count=1;for(int i=0;i<dimensions;++i)count*=*reinterpret_cast<int*>(raw+4+i*4);auto* bytes=raw+4+dimensions*4;result.bytes.assign(bytes,bytes+count);RuntimeFree(raw);}break;
    default:if(data.pointerValue)ReadObject(result,data.pointerValue);break;
    } return result;
}
__declspec(noinline) static bool InvokeCommand(ExecuteCommand command,MData* result,int count,MData* arguments) {
    command(result,count,arguments);
    return true;
}
static thread_local std::function<Value()> currentStatement;
static int __cdecl EvaluateStatement() { return currentStatement ? (ToBool(currentStatement()) ? 1 : 0) : 0; }
#if defined(_WIN64)
// The legacy statement ABI stores a 32-bit code address and cannot represent
// an x64 function pointer.  x64 core libraries do not execute these callbacks;
// keep a normal callable stub so ordinary command marshalling remains valid.
extern "C" int __cdecl StatementThunk() { return EvaluateStatement(); }
#else
extern "C" __declspec(naked) void __cdecl StatementThunk() {
    __asm {
        push ebp
        mov ebp,esp
        call EvaluateStatement
        mov ecx,080000002h
        mov esp,ebp
        pop ebp
        ret
    }
}
#endif
static unsigned int StatementAddress() {
#if defined(_WIN64)
    return 0;
#else
    return reinterpret_cast<unsigned int>(&StatementThunk);
#endif
}
static void ReleaseArena(Arena& arena) {
    for(const auto& item:arena.ownedArrays) {
        if(item.pointer!=nullptr && !arena.releasedArrays.contains(item.pointer)) {
            RuntimeFreeArray(item.type,item.pointer);
            arena.releasedArrays.insert(item.pointer);
        }
    }
    for(void* pointer:arena.ownedValues) if(pointer!=nullptr) RuntimeFree(pointer);
}
struct ArenaScope {
    Arena arena;
    ~ArenaScope() { ReleaseArena(arena); }
};

static const char* DllText(Value& value) {
    static thread_local std::string converted;
    if(value.type==T_TEXT) return value.text.c_str();
    converted=ToText(value);
    return converted.c_str();
}
static unsigned char* DllBinary(Value& value) {
    return value.type==T_BIN && !value.bytes.empty()?value.bytes.data():nullptr;
}
static void* DllObject(Value& value) {
    PrepareObject(value);
    return value.object.empty()?nullptr:value.object.data();
}
static void* DllArray(Value& value,Arena& arena) {
    return MarshalArray(value,value.type|T_ARRAY,arena);
}
static char* DllTextReference(Value& value) { return RuntimeText(value.type==T_TEXT?value.text:ToText(value)); }
static void DllCaptureText(Value& value,char* current,char* initial) {
    value.text=current==nullptr?std::string():std::string(current);
    if(current==initial) RuntimeFree(current);
}
static Value DllTextResult(const char* value) { return Text(value==nullptr?std::string():std::string(value)); }
static Value DllBinaryResult(const unsigned char* value) {
    if(value==nullptr) return Bytes({});
    const int dimensions=*reinterpret_cast<const int*>(value);
    if(dimensions<=0 || dimensions>32) return Bytes({});
    int count=1;
    for(int index=0;index<dimensions;++index) {
        const int dimension=*reinterpret_cast<const int*>(value+4+index*4);
        if(dimension<0 || dimension>0x1000000) return Bytes({});
        count*=dimension;
    }
    const auto* data=value+4+dimensions*4;
    return Bytes(std::vector<unsigned char>(data,data+count));
}
static void DestroyValue(Value& value) {
    if(value.declaredArray) {
        for(auto& element:value.elements) DestroyValue(element);
        value.elements.clear(); value.dimensions.clear();
        return;
    }
    const auto* descriptor=FindType(value.type);
    if(descriptor!=nullptr && !descriptor->enumeration && descriptor->destructor!=nullptr && !value.object.empty()) {
        MData receiver{}; receiver.type=value.type; receiver.pointerValue=value.object.data();
        MData result{}; result.type=value.type; descriptor->destructor(&result,1,&receiver);
        std::memset(value.object.data(),0,(std::min)(value.object.size(),descriptor->size));
        for(auto& field:value.fields) {
            if(field.type==T_TEXT) field.text.clear();
            else if(field.type==T_BIN) field.bytes.clear();
            else if(!FindType(field.type)) { field.integer=0; field.number=0; }
        }
        return;
    }
    if(descriptor!=nullptr) {
        // 文本和字节集由 Value 自身的容器管理；不需要也不能按对象
        // 析构路径处理。只有嵌套复合字段可能注册生命周期回调。
        for(auto& field:value.fields) if(FindType(field.type)!=nullptr) DestroyValue(field);
    }
    value.object.clear();
}
Value::Value(const Value& other)
    : declared(other.declared), type(other.type), declaredArray(other.declaredArray), missing(other.missing),
      integer(other.integer), number(other.number), text(other.text), bytes(other.bytes), elements(other.elements),
      dimensions(other.dimensions), fields(other.fields), object(other.object) {
    const auto* descriptor=FindType(type);
    if(!declaredArray && descriptor!=nullptr && !descriptor->enumeration && descriptor->copier!=nullptr && !object.empty() && !other.object.empty()) {
        MData arguments[2]{};
        arguments[0].type=type; arguments[0].pointerValue=object.data();
        arguments[1].type=type; arguments[1].pointerValue=const_cast<unsigned char*>(other.object.data());
        MData result{}; result.type=type; descriptor->copier(&result,2,arguments);
    }
}
Value& Value::operator=(const Value& other) {
    if(this==&other) return *this;
    DestroyValue(*this);
    declared=other.declared; type=other.type; declaredArray=other.declaredArray; missing=other.missing;
    integer=other.integer; number=other.number; text=other.text; bytes=other.bytes;
    elements=other.elements; dimensions=other.dimensions; fields=other.fields; object=other.object;
    const auto* descriptor=FindType(type);
    if(!declaredArray && descriptor!=nullptr && !descriptor->enumeration && descriptor->copier!=nullptr && !object.empty() && !other.object.empty()) {
        MData arguments[2]{}; arguments[0].type=type; arguments[0].pointerValue=object.data(); arguments[1].type=type; arguments[1].pointerValue=const_cast<unsigned char*>(other.object.data());
        MData result{}; result.type=type; descriptor->copier(&result,2,arguments);
    }
    return *this;
}
struct MethodValueScope {
    std::vector<Value>* parameters=nullptr;
    std::vector<Value>* locals=nullptr;
    std::function<void()> writeback;
    ~MethodValueScope() {
        if(writeback) writeback();
        if(parameters) for(auto& value:*parameters) DestroyValue(value);
        if(locals) for(auto& value:*locals) DestroyValue(value);
    }
};
struct StatementScope {
    std::function<Value()> previous;
    explicit StatementScope(std::function<Value()> evaluator) : previous(std::move(currentStatement)) { currentStatement=std::move(evaluator); }
    ~StatementScope() { currentStatement=std::move(previous); }
};
static Value CallFne(const char* name,ExecuteCommand command,std::uint32_t returnType,bool returnsArray,std::vector<Arg> args,std::vector<ParamSpec> specs) {
    (void)name;
    std::vector<MData> raw((std::max)(args.size(),std::size_t(1))); Arena arena;
    std::unique_ptr<StatementScope> statementScope;
    for(std::size_t i=0;i<args.size();++i) {
        if(specs[i].type==T_STATEMENT && args[i].statement) {
            statementScope=std::make_unique<StatementScope>(args[i].statement);
			raw[i].type=T_STATEMENT; raw[i].statement.statementAddress=StatementAddress(); raw[i].statement.statementEbp=0;
            continue;
        }
		const bool byReference=(specs[i].state&(1u<<2|1u<<3|1u<<4|1u<<5|1u<<9))!=0 && args[i].reference!=nullptr;
        if(byReference) MarshalByReference(args[i].Get(),specs[i].type,specs[i].state,raw[i],arena);
        else MarshalValue(args[i].Get(),specs[i].type,raw[i],arena);
    }
    MData result{}; result.type=returnType|(returnsArray?T_ARRAY:0); InvokeCommand(command,&result,static_cast<int>(args.size()),raw.data());
    ApplyWritebacks(arena);
    Value returned=CopyReturned(result,returnType,returnsArray);
    ReleaseArena(arena);
    return returned;
}
} // namespace ert
)CPP";

class Emitter {
public:
	explicit Emitter(const Program& program) : program_(program) {}

	bool Run(GeneratedSource& result, std::string& error)
	{
		error_ = &error;
		result_ = &result;
		result = {};
		if (program_.useLegacyX86RuntimeBridge) {
			result.text = "#define ECOMPILER_LEGACY_X86_RUNTIME 1\n";
		}
		result.text += RuntimeSourceUtf8(kRuntimeSource);
		body_ << "\nusing namespace ert;\n";
		EmitGlobals();
		const auto startup = program_.methodByName.find("_启动子程序");
		if (startup == program_.methodByName.end()) return Fail("startup_method_not_found:_启动子程序");
		QueueMethod(startup->second);
		for (const auto& form : program_.windows) {
			for (const auto& event : form.events) QueueMethod(event.methodId);
			for (const auto& control : form.controls) {
				// Keep the emitter defensive when a caller supplies a hand-built
				// Program instead of going through BuildWindowModel().  Unsupported
				// support-library units must never make their event handlers reachable.
				if (!HasNativeWin32Class(control.typeName)) continue;
				for (const auto& event : control.events) QueueMethod(event.methodId);
			}
		}
		if (program_.buildDll) {
			for (const Method& method : program_.methods) if (method.isPublic) QueueMethod(method.id);
		}
		for (std::size_t index = 0; index < pendingMethods_.size(); ++index) {
			if (!EmitMethod(program_.methods[pendingMethods_[index]])) return false;
		}
		for (const Method& method : program_.methods) {
			if (program_.buildDll && method.isPublic) EmitExportWrapper(method);
		}
		if (!EmitTypes()) return false;
		EmitDeclarationsAndStartup();
		result.reachableLibraries = reachableLibraries_;
		result.exports = exports_;
		result.imports = imports_;
		result.reachableMethodCount = emittedMethods_.size();
		result.reachableCommandCount = reachableCommands_.size();
		return true;
	}

private:
	struct LifecycleBinding {
		std::string constructor;
		std::string destructor;
		std::string copier;
	};

	bool Fail(const std::string& message)
	{
		if (error_->empty()) *error_ = message;
		return false;
	}

	void Write(const std::string& text) { body_ << text; }
	void Line(const int indent, const std::string& text) { body_ << std::string(static_cast<std::size_t>(indent) * 4, ' ') << text << "\n"; }
	void SourceLine(const std::string& file, const std::size_t line)
	{
		if (line == 0) return;
		std::string normalized = file;
		const int utf8Length = MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, normalized.data(), static_cast<int>(normalized.size()), nullptr, 0);
		if (utf8Length <= 0) {
			const int localLength = MultiByteToWideChar(
				CP_ACP, 0, normalized.data(), static_cast<int>(normalized.size()), nullptr, 0);
			if (localLength > 0) {
				std::wstring wide(static_cast<std::size_t>(localLength), L'\0');
				if (MultiByteToWideChar(
						CP_ACP, 0, normalized.data(), static_cast<int>(normalized.size()), wide.data(), localLength) > 0) {
					normalized = WideToUtf8Text(wide);
				}
			}
		}
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		normalized.erase(std::remove(normalized.begin(), normalized.end(), '"'), normalized.end());
		body_ << "#line " << line << " \"" << normalized << "\"\n";
	}

	bool EmitTypes()
	{
		lifecycle_.assign(program_.types.size(), LifecycleBinding {});
		std::set<std::size_t> usedTypeIndexes;
		for (const std::uint32_t code : usedTypes_) {
			const auto found = program_.typeByCode.find(code & ~kTypeArrayFlag);
			if (found != program_.typeByCode.end()) usedTypeIndexes.insert(found->second);
		}
		std::queue<std::size_t> usedTypeQueue;
		for (const std::size_t typeIndex : usedTypeIndexes) usedTypeQueue.push(typeIndex);
		while (!usedTypeQueue.empty()) {
			const std::size_t typeIndex = usedTypeQueue.front();
			usedTypeQueue.pop();
			for (const auto& element : program_.types[typeIndex].elements) {
				const auto found = program_.typeByCode.find(element.type.code & ~kTypeArrayFlag);
				if (found != program_.typeByCode.end() && usedTypeIndexes.insert(found->second).second) usedTypeQueue.push(found->second);
			}
		}
		for (std::size_t typeIndex = 0; typeIndex < program_.types.size(); ++typeIndex) {
			const TypeInfo& type = program_.types[typeIndex];
			if (!usedTypeIndexes.contains(typeIndex)) continue;
			if (!type.layoutComplete) {
				return Fail("incomplete_support_library_type:" + type.name);
			}
			if (type.libraryIndex >= program_.libraries.size()) continue;
			const auto& commands = program_.libraries[type.libraryIndex].metadata.commands;
			for (const std::size_t commandIndex : type.memberCommandIndexes) {
				if (commandIndex >= commands.size()) continue;
				const auto& command = commands[commandIndex];
				if (command.executeSymbol.empty() || !IsCppIdentifier(command.executeSymbol)) continue;
				if ((command.state & kCommandObjectConstruct) != 0) lifecycle_[typeIndex].constructor = command.executeSymbol;
				if ((command.state & kCommandObjectFree) != 0) lifecycle_[typeIndex].destructor = command.executeSymbol;
				if ((command.state & kCommandObjectCopy) != 0) lifecycle_[typeIndex].copier = command.executeSymbol;
				if ((command.state & (kCommandObjectConstruct | kCommandObjectFree | kCommandObjectCopy)) != 0) {
					reachableLibraries_.insert(type.libraryIndex);
					reachableSymbols_.insert(command.executeSymbol);
				}
			}
		}
		for (const auto& binding : lifecycle_) {
			for (const auto& symbol : { binding.constructor, binding.destructor, binding.copier }) {
				if (!symbol.empty()) body_ << "extern \"C\" void __cdecl " << symbol << "(ert::MData*,int,ert::MData*);\n";
			}
		}
		body_ << "\nnamespace ert {\n";
		for (std::size_t typeIndex = 0; typeIndex < program_.types.size(); ++typeIndex) {
			const TypeInfo& type = program_.types[typeIndex];
			if (type.elements.empty()) continue;
			body_ << "static const FieldDesc type_fields_" << typeIndex << "[]={";
			for (const TypeElement& element : type.elements) {
				body_ << '{' << Hex(element.type.code) << ',' << element.offset << ',' << (element.type.isArray ? "true" : "false") << "},";
			}
			body_ << "};\n";
		}
		body_ << "static const TypeDesc type_table[]={\n";
		for (std::size_t typeIndex = 0; typeIndex < program_.types.size(); ++typeIndex) {
			const TypeInfo& type = program_.types[typeIndex];
			body_ << "    {" << Hex(type.type.code) << ',' << (std::max)(type.size, std::size_t(64)) << ',';
			if (type.elements.empty()) body_ << "nullptr,0";
			else body_ << "type_fields_" << typeIndex << ',' << type.elements.size();
			const auto& lifecycle = lifecycle_[typeIndex];
			body_ << "," << (lifecycle.constructor.empty() ? "nullptr" : "&" + lifecycle.constructor)
				<< "," << (lifecycle.destructor.empty() ? "nullptr" : "&" + lifecycle.destructor)
				<< "," << (lifecycle.copier.empty() ? "nullptr" : "&" + lifecycle.copier)
				<< "," << (type.isEnum ? "true" : "false");
			body_ << "},\n";
		}
		body_ << "};\nconst TypeDesc* TypeTable(){return type_table;}\n"
			"std::size_t TypeCount(){return sizeof(type_table)/sizeof(type_table[0]);}\n}\nusing namespace ert;\n";
		return true;
	}

	void EmitGlobals()
	{
		for (std::size_t index = 0; index < program_.globals.size(); ++index) {
			const Variable& variable = program_.globals[index];
			usedTypes_.insert(variable.type.code);
			body_ << "static Value& g_global_" << index << "(){static Value* value=nullptr;if(value==nullptr){value=new Value(MakeVar("
				<< Hex(variable.type.code) << ',' << (variable.type.isArray ? "true" : "false") << "));";
			if (variable.type.isArray && !variable.arrayDimensions.empty()) {
				body_ << "Redim(*value,std::vector<int>{";
				for (const int dimension : variable.arrayDimensions) body_ << dimension << ',';
				body_ << "},false);";
			}
			body_ << "}return *value;}\n";
		}
		for (std::size_t assemblyIndex = 0; assemblyIndex < program_.assemblies.size(); ++assemblyIndex) {
			const auto& assembly = program_.assemblies[assemblyIndex];
			for (std::size_t index = 0; index < assembly.variables.size(); ++index) {
				const Variable& variable = assembly.variables[index];
				usedTypes_.insert(variable.type.code);
				body_ << "static Value& g_" << assemblyIndex << '_' << index << "(){static Value* value=nullptr;if(value==nullptr){value=new Value(MakeVar("
					<< Hex(variable.type.code) << ',' << (variable.type.isArray ? "true" : "false") << "));";
				if (variable.type.isArray && !variable.arrayDimensions.empty()) {
					body_ << "Redim(*value,std::vector<int>{";
					for (const int dimension : variable.arrayDimensions) body_ << dimension << ',';
					body_ << "},false);";
				}
				body_ << "}return *value;}\n";
			}
		}
	}

	const WindowForm* WindowForMethod(const Method& method) const
	{
		for (const auto& form : program_.windows) {
			if (form.assemblyIndex == method.assemblyIndex) return &form;
		}
		return nullptr;
	}

	const WindowControl* WindowControlByName(const Method& method, const std::string& name) const
	{
		const WindowForm* form = WindowForMethod(method);
		if (form == nullptr) return nullptr;
		for (const auto& control : form->controls)
			if (HasNativeWin32Class(control.typeName) && control.name == name) return &control;
		return nullptr;
	}

	std::optional<std::uint32_t> WindowTargetId(const Method& method, const std::string& name) const
	{
		if (const auto* control = WindowControlByName(method, name)) return control->id;
		if (const auto* form = WindowForMethod(method)) {
			if (name == form->name || name == form->className) return form->id;
		}
		return std::nullopt;
	}

	std::optional<std::string> WindowMemberOperation(
		const Method& method,
		const e2txt::SourceExpressionNode& receiver,
		const std::string& name) const
	{
		if (receiver.kind != e2txt::SourceExpressionKind::Name) return std::nullopt;
		const auto* control = WindowControlByName(method, receiver.text);
		const auto* form = WindowForMethod(method);
		if (control == nullptr && (form == nullptr || (receiver.text != form->name && receiver.text != form->className))) return std::nullopt;
		const auto matches = [&name](const std::initializer_list<const char*> aliases) {
			return std::any_of(aliases.begin(), aliases.end(), [&name](const char* alias) { return name == alias; });
		};
		// These commands are supplied by the core window type and are shared by
		// forms and ordinary controls.  Keep them here instead of duplicating
		// aliases in every control-specific branch below.
		if (matches({ "取窗口句柄", "GetHWnd" })) return "GetHWnd";
		if (matches({ "销毁", "destroy", "Destroy" })) return "Destroy";
		if (matches({ "获取焦点", "SetFocus" })) return "SetFocus";
		if (matches({ "可有焦点", "IsFocus" })) return "IsFocus";
		if (matches({ "取用户区宽度", "GetClientWidth" })) return "GetClientWidth";
		if (matches({ "取用户区高度", "GetClientHeight" })) return "GetClientHeight";
		if (matches({ "禁止重画", "LockWindowUpdate" })) return "LockWindowUpdate";
		if (matches({ "允许重画", "UnlockWindowUpdate" })) return "UnlockWindowUpdate";
		if (matches({ "重画", "invalidate", "Invalidate" })) return "Invalidate";
		if (matches({ "部分重画", "InvalidateRect" })) return "InvalidateRect";
		if (matches({ "取消重画", "validate", "Validate" })) return "Validate";
		if (matches({ "刷新显示", "UpdateWindow" })) return "UpdateWindow";
		if (matches({ "移动", "move", "Move" })) return "Move";
		if (matches({ "调整层次", "ZOrder" })) return "ZOrder";
		if (matches({ "发送信息", "SendMessage" })) return "SendMessage";
		if (matches({ "投递信息", "PostMessage" })) return "PostMessage";
		if (matches({ "弹出菜单", "PopupMenu" })) return "PopupMenu";
		if (matches({ "激活", "Activate" })) return "Activate";
		if (matches({ "置托盘图标", "SetTrayIcon" })) return "SetTrayIcon";
		if (matches({ "弹出托盘菜单", "PopupTrayMenu" })) return "PopupTrayMenu";
		if (matches({ "置父窗口", "SetParentWnd" })) return "SetParentWnd";
		if (matches({ "取标记组件", "GetSpecTagUnit" })) return "GetSpecTagUnit";
		if (matches({ "置外形图片", "SetShapePic" })) return "SetShapePic";
		if (control == nullptr) return std::nullopt;
		if (control->typeName == "画板") {
			if (matches({ "取设备句柄", "GetHDC" })) return "GetHDC";
			if (matches({ "取点", "GetPixel" })) return "GetPixel";
			if (matches({ "画点", "SetPixel" })) return "SetPixel";
			if (matches({ "画直线", "LineTo" })) return "LineTo";
			if (matches({ "画椭圆", "ellipse", "Ellipse" })) return "Ellipse";
			if (matches({ "画弧线", "ArcTo", "Arc" })) return "ArcTo";
			if (matches({ "画弦", "chord", "Chord" })) return "Chord";
			if (matches({ "画饼", "pie", "Pie" })) return "Pie";
			if (matches({ "画矩形", "DrawRect" })) return "DrawRect";
			if (matches({ "填充矩形", "FillRect" })) return "FillRect";
			if (matches({ "画圆角矩形", "RoundRect" })) return "RoundRect";
			if (matches({ "翻转矩形区", "InvertRect" })) return "InvertRect";
			if (matches({ "清除", "cls", "Clear" })) return "CanvasClear";
			if (matches({ "画渐变矩形", "DrawJBRect" })) return "GradientRect";
			if (matches({ "画图片", "DrawPic" })) return "DrawPic";
			if (matches({ "取图片宽度", "GetPicWidth" })) return "GetPicWidth";
			if (matches({ "取图片高度", "GetPicHeight" })) return "GetPicHeight";
			if (matches({ "复制", "copy", "Copy" })) return "CopyCanvas";
			if (matches({ "画多边形", "polygon", "Polygon" })) return "Polygon";
			if (matches({ "置写出位置", "SetWritePos" })) return "SetWritePos";
			if (matches({ "写文本行", "print", "Print" })) return "PrintLine";
			if (matches({ "滚动写行", "sprint", "ScrollPrint" })) return "ScrollPrint";
			if (matches({ "取图片", "GetPic" })) return "GetCanvasPic";
			if (matches({ "单位转换", "UnitCnv" })) return "UnitCnv";
			if (matches({ "写出", "write", "Write" })) return "Write";
			if (matches({ "定位写出", "say", "Say" })) return "Say";
			if (matches({ "取宽度", "GetWidth" })) return "CanvasGetWidth";
			if (matches({ "取高度", "GetHeight" })) return "CanvasGetHeight";
		}
		if (control->typeName == "编辑框" && matches({ "加入文本", "AddText" })) return "AddText";
		if (control->typeName == "标签" && matches({ "调用反馈事件", "SendLabelMsg" })) return "SendLabelMsg";
		if (control->typeName == "超级链接框" && matches({ "跳转", "goto", "Goto" })) return "Goto";
		if (control->typeName == "选择夹" && matches({ "取子夹数目", "GetCount" })) return "GetTabCount";
		if (control->typeName == "选择夹" && matches({ "取子夹名称", "GetName" })) return "GetTabName";
		if (control->typeName == "选择夹" && matches({ "置子夹名称", "SetName" })) return "SetTabName";
		const bool listLike = control->typeName == "列表框" || control->typeName == "选择列表框";
		const bool itemList = listLike || control->typeName == "组合框";
		if (!itemList) return std::nullopt;
		if (matches({ "取顶端可见项目", "GetTopIndex" })) return "GetTopIndex";
		if (matches({ "置顶端可见项目", "SetTopIndex" })) return "SetTopIndex";
		if (matches({ "取项目数", "GetCount" })) return "GetCount";
		if (matches({ "取项目数值", "GetItemData" })) return "GetItemData";
		if (matches({ "置项目数值", "SetItemData" })) return "SetItemData";
		if (matches({ "取项目文本", "GetItemText" })) return "GetItemText";
		if (matches({ "置项目文本", "SetItemtext", "SetItemText" })) return "SetItemText";
		if (matches({ "加入项目", "AddString" })) return "AddString";
		if (matches({ "插入项目", "InsertString" })) return "InsertString";
		if (matches({ "删除项目", "DeleteString" })) return "DeleteString";
		if (matches({ "清空", "clear", "Clear" })) return "Clear";
		if (matches({ "选择", "SelItem" })) return "SelItem";
		if (control->typeName != "列表框") {
			if (control->typeName == "选择列表框") {
				if (matches({ "是否被选中", "IsChecked" })) return "IsChecked";
				if (matches({ "选中项目", "SetCheck" })) return "SetCheck";
				if (matches({ "是否被允许", "IsEnabled" })) return "IsEnabled";
				if (matches({ "允许", "enable", "Enable" })) return "Enable";
			}
			return std::nullopt;
		}
		if (matches({ "取已选择项目数", "GetSelCount" })) return "GetSelCount";
		if (matches({ "取所有被选择项目", "GetSelItems" })) return "GetSelItems";
		if (matches({ "是否被选择", "IsSelected" })) return "IsSelected";
		if (matches({ "选择项目", "select", "Select" })) return "SelectItem";
		if (matches({ "取焦点项目", "GetCaretIndex" })) return "GetCaretIndex";
		if (matches({ "置焦点项目", "SetCaretIndex" })) return "SetCaretIndex";
		return std::nullopt;
	}

	TypeRef WindowMemberReturnType(const std::string& operation) const
	{
		if (operation == "GetHWnd" || operation == "GetClientWidth" || operation == "GetClientHeight" || operation == "SendMessage")
			return { kTypeInt, false, true };
		if (operation == "GetHDC" || operation == "GetPixel") return { kTypeInt, false, true };
		if (operation == "GetSpecTagUnit") return { kTypeWindowUnit, false, true };
		if (operation == "IsFocus") return { kTypeBool, false, true };
		if (operation == "SetShapePic") return { kTypeBool, false, true };
		if (operation == "OpenDialog") return { kTypeBool, false, true };
		if (operation == "Destroy" || operation == "SetFocus" || operation == "LockWindowUpdate" ||
			operation == "UnlockWindowUpdate" || operation == "Invalidate" || operation == "InvalidateRect" ||
			operation == "Validate" || operation == "UpdateWindow" || operation == "Move" || operation == "ZOrder" ||
			operation == "PostMessage" || operation == "PopupMenu" || operation == "Activate" || operation == "SetTrayIcon" || operation == "PopupTrayMenu" || operation == "SetParentWnd")
			return { kTypeNull, false, true };
		if (operation == "GetItemText" || operation == "GetTabName") return { kTypeText, false, true };
		if (operation == "GetSelItems") return { kTypeInt, true, true };
		if (operation == "AddText" || operation == "Clear" || operation == "Goto" || operation == "DrawPic" || operation == "CopyCanvas") return { kTypeNull, false, true };
		if (operation == "SetPixel" || operation == "LineTo" || operation == "Ellipse" || operation == "ArcTo" ||
			operation == "Chord" || operation == "Pie" || operation == "DrawRect" || operation == "FillRect" ||
			operation == "RoundRect" || operation == "InvertRect" || operation == "CanvasClear" || operation == "Polygon" ||
			operation == "SetWritePos" || operation == "PrintLine" || operation == "ScrollPrint" || operation == "Write" || operation == "Say") return { kTypeNull, false, true };
		if (operation == "CanvasGetWidth" || operation == "CanvasGetHeight" || operation == "GetPicWidth" || operation == "GetPicHeight" || operation == "UnitCnv") return { kTypeInt, false, true };
		if (operation == "GetCanvasPic") return { kTypeBinary, false, true };
		if (operation == "SendLabelMsg") return { kTypeInt, false, true };
		if (operation == "SetTopIndex" || operation == "SetItemData" || operation == "SetItemText" ||
			operation == "DeleteString" || operation == "IsSelected" || operation == "SelectItem" ||
			operation == "SetCaretIndex" || operation == "IsChecked" || operation == "SetCheck" ||
			operation == "IsEnabled" || operation == "Enable" || operation == "SetTabName")
			return { kTypeBool, false, true };
		return { kTypeInt, false, true };
	}

	bool IsWindowRootProperty(const Method& method, const std::string& name) const
	{
		return name == "标题" && WindowForMethod(method) != nullptr && !FindVariable(method, name).has_value();
	}

	const WindowControl* WindowPropertyTarget(
		const Method& method,
		const e2txt::SourceExpressionNode& node,
		std::string& property) const
	{
		if (node.kind != e2txt::SourceExpressionKind::Member || node.children.empty() ||
			node.children.front()->kind != e2txt::SourceExpressionKind::Name) return nullptr;
		const auto* control = WindowControlByName(method, node.children.front()->text);
		if (control == nullptr) return nullptr;
		property = node.text;
		return control;
	}

	bool IsWindowPropertyTarget(
		const Method& method,
		const e2txt::SourceExpressionNode& node,
		std::uint32_t& unitId,
		std::string& property) const
	{
		if (node.kind == e2txt::SourceExpressionKind::Name && IsWindowRootProperty(method, node.text)) {
			unitId = WindowForMethod(method)->id;
			property = node.text;
			return true;
		}
		if (const auto* control = WindowPropertyTarget(method, node, property)) {
			unitId = control->id;
			return true;
		}
		return false;
	}

	void QueueMethod(const std::size_t id)
	{
		if (emittedMethods_.insert(id).second) pendingMethods_.push_back(id);
	}

	std::optional<std::pair<std::string, TypeRef>> FindVariable(const Method& method, const std::string& name) const
	{
		for (std::size_t index = 0; index < method.parameters.size(); ++index)
			if (method.parameters[index].name == name) return std::pair { "p[" + std::to_string(index) + "]", method.parameters[index].type };
		for (std::size_t index = 0; index < method.locals.size(); ++index)
			if (method.locals[index].name == name) return std::pair { "v[" + std::to_string(index) + "]", method.locals[index].type };
		const auto& assembly = program_.assemblies[method.assemblyIndex];
		if (method.ownerType.valid) {
			for (std::size_t index = 0; index < assembly.variables.size(); ++index)
				if (assembly.variables[index].name == name) return std::pair { "(self!=nullptr?self->fields[" + std::to_string(index) + "]:g_" + std::to_string(method.assemblyIndex) + '_' + std::to_string(index) + "())", assembly.variables[index].type };
		}
		for (std::size_t index = 0; index < assembly.variables.size(); ++index)
			if (assembly.variables[index].name == name) return std::pair { "g_" + std::to_string(method.assemblyIndex) + '_' + std::to_string(index) + "()", assembly.variables[index].type };
		for (std::size_t index = 0; index < program_.globals.size(); ++index)
			if (program_.globals[index].name == name) return std::pair { "g_global_" + std::to_string(index) + "()", program_.globals[index].type };
		if (const auto* control = WindowControlByName(method, name)) {
			return std::pair {
				"Integer(static_cast<long long>(" + std::to_string(control->id) + "),T_WINDOW_UNIT)",
				TypeRef { kTypeWindowUnit, false, true } };
		}
		return std::nullopt;
	}

	bool CommandArityMatches(const support_library_public_info::CommandMetadata& command, const std::size_t count) const
	{
		std::size_t required = 0;
		for (const auto& argument : command.arguments) {
			if ((argument.state & (kArgumentHasDefault | kArgumentDefaultEmpty)) == 0) ++required;
		}
		if (count < required) return false;
		return count <= command.arguments.size() || (command.state & kCommandAllowAppendArgument) != 0;
	}

	std::optional<CommandBinding> ResolveGlobalCommand(const std::string& name, const std::size_t argumentCount) const
	{
		const auto found = program_.globalCommands.find(name);
		if (found != program_.globalCommands.end()) {
			for (const auto [libraryIndex, commandIndex] : found->second) {
				const auto& command = program_.libraries[libraryIndex].metadata.commands[commandIndex];
				if (CommandArityMatches(command, argumentCount)) return CommandBinding { libraryIndex, commandIndex, &command, false };
			}
		}
		// Keep command resolution independent from the order and representation
		// used to build the name index. This matters for FNEs reconstructed from
		// different metadata encodings where the canonical map can miss a name.
		for (std::size_t libraryIndex = 0; libraryIndex < program_.libraries.size(); ++libraryIndex) {
			const auto& commands = program_.libraries[libraryIndex].metadata.commands;
			for (std::size_t commandIndex = 0; commandIndex < commands.size(); ++commandIndex) {
				const auto& command = commands[commandIndex];
				if (command.name == name && CommandArityMatches(command, argumentCount))
					return CommandBinding { libraryIndex, commandIndex, &command, false };
			}
		}
		return std::nullopt;
	}

	std::optional<CommandBinding> ResolveMemberCommand(const TypeRef receiver, const std::string& name, const std::size_t argumentCount) const
	{
		const TypeInfo* type = program_.FindType(receiver.code);
		if (type == nullptr || type->libraryIndex >= program_.libraries.size()) return std::nullopt;
		const auto& commands = program_.libraries[type->libraryIndex].metadata.commands;
		for (const std::size_t commandIndex : type->memberCommandIndexes) {
			if (commandIndex >= commands.size()) continue;
			const auto& command = commands[commandIndex];
			if (command.name == name && CommandArityMatches(command, argumentCount))
				return CommandBinding { type->libraryIndex, commandIndex, &command, true };
		}
		return std::nullopt;
	}

	const Method* ResolveMemberMethod(const TypeRef receiver, const std::string& name, const std::size_t argumentCount) const
	{
		const TypeInfo* type = program_.FindType(receiver.code);
		if (type == nullptr) return nullptr;
		for (const std::size_t methodId : type->memberMethodIds) {
			if (methodId >= program_.methods.size()) continue;
			const Method& method = program_.methods[methodId];
			if (method.name == name && argumentCount <= method.parameters.size()) return &method;
		}
		return nullptr;
	}

	const Method* ResolveOwnedMethod(const Method& owner, const std::string& name, const std::size_t argumentCount) const
	{
		return owner.ownerType.valid ? ResolveMemberMethod(owner.ownerType, name, argumentCount) : nullptr;
	}

	const Method* ResolveUnqualifiedMethod(const Method& owner, const std::string& name, const std::size_t argumentCount) const
	{
		// Unqualified calls in an ordinary assembly first bind to that
		// assembly, matching the IDE's namespace rules.
		for (const Method& candidate : program_.methods) {
			if (candidate.assemblyIndex != owner.assemblyIndex || candidate.ownerType.valid ||
				candidate.name != name || argumentCount > candidate.parameters.size()) continue;
			return &candidate;
		}
		if (const auto indexed = program_.methodByName.find(name); indexed != program_.methodByName.end() &&
			indexed->second < program_.methods.size()) {
			const Method& method = program_.methods[indexed->second];
			if (!method.ownerType.valid && argumentCount <= method.parameters.size()) return &method;
		}
		// Reconstructed bundles can contain methods whose qualified-name index was
		// built before all source pages were loaded. Resolve ordinary assembly
		// methods from the canonical method vector as a deterministic fallback.
		for (const Method& method : program_.methods) {
			if (!method.ownerType.valid && method.name == name && argumentCount <= method.parameters.size()) return &method;
		}
		return nullptr;
	}

	const DllCommand* ResolveDllCommand(const std::string& name, const std::size_t argumentCount) const
	{
		const auto found = program_.dllCommandByName.find(name);
		if (found == program_.dllCommandByName.end() || found->second >= program_.dllCommands.size()) return nullptr;
		const DllCommand& command = program_.dllCommands[found->second];
		std::size_t required = 0;
		for (const Variable& parameter : command.parameters) if (!parameter.nullable) ++required;
		if (argumentCount < required || argumentCount > command.parameters.size()) return nullptr;
		return &command;
	}

	TypeRef Infer(const Method& method, const e2txt::SourceExpressionNode& node)
	{
		using Kind = e2txt::SourceExpressionKind;
		switch (node.kind) {
		case Kind::NumberLiteral: return { node.text.find('.') == std::string::npos ? kTypeInt : kTypeDouble, false, true };
		case Kind::TextLiteral: return { kTypeText, false, true };
		case Kind::LogicalLiteral: return { kTypeBool, false, true };
		case Kind::DateTimeLiteral: return { kTypeDateTime, false, true };
		case Kind::ByteSetLiteral: return { kTypeBinary, false, true };
		case Kind::Name:
			if (const auto variable = FindVariable(method, node.text)) return variable->second;
			if (const auto constant = program_.constants.find(node.text); constant != program_.constants.end()) return { constant->second.type, false, true };
			return {};
		case Kind::Group: case Kind::Unary: case Kind::AddressOf:
			return node.children.empty() ? TypeRef{} : Infer(method, *node.children.front());
		case Kind::Index: {
			if (node.children.empty()) return {};
			TypeRef type = Infer(method, *node.children.front()); type.isArray = false; return type;
		}
		case Kind::Member: {
			if (node.children.empty()) return {};
			std::string property;
			if (const auto* control = WindowPropertyTarget(method, node, property)) {
				(void)control;
				if (property == "选中" || property == "可视" || property == "禁止" || property == "单选")
					return { kTypeBool, false, true };
				if (property == "今天" || property == "最小日期" || property == "最大日期" ||
					property == "首选择日" || property == "尾选择日")
					return { kTypeDateTime, false, true };
				if (property == "现行选中项" || property == "位置" ||
					property == "现行子夹" || property == "起始选择位置" ||
					property == "被选择字符数" || property == "最大文本长度" ||
					property == "最大允许长度" || property == "类型" ||
					property == "行间距" || property == "滚动条" ||
					property == "最小位置" || property == "最大位置" || property == "页改变值" ||
					property == "行改变值" || property == "单位刻度值" || property == "首选择位置" ||
					property == "选择长度" || property == "最多选择天数" || property == "滚动月数" ||
					property == "开始星期首日" || property == "外形" || property == "线条效果" ||
					property == "线型" || property == "线宽" || property == "显示方式" ||
					property == "左边" || property == "顶边" || property == "边框" ||
					property == "方向" || property == "附件类型" || property == "调节器方式" ||
					property == "调节器底限值" || property == "调节器上限值" ||
					property == "刻度类型" || property == "表头方向" ||
					property == "对齐方式" || property == "滚动条" ||
					property == "宽度" || property == "高度" || property == "画板宽度" || property == "画板高度" || property == "字体大小" ||
					property == "渐变背景方式" || property == "渐变边框宽度" ||
					property == "播放次数" || property == "取子夹数目")
					return { kTypeInt, false, true };
				if (property == "隐藏选择" || property == "是否允许多行" ||
					property == "自动排序" || property == "允许选择多项" ||
					property == "隐藏自身" || property == "可停留焦点" || property == "允许编辑" ||
					property == "允许多行表头" || property == "是否填充背景" ||
					property == "允许选择多天" || property == "不显示今天" || property == "不圈注今天" ||
					property == "显示星期序号" || property == "允许选择" || property == "自动重画" ||
					property == "按钮形式" || property == "平面" || property == "选中" ||
					property == "标题居左" ||
					property == "居中播放" || property == "透明背景" || property == "播放动画" ||
					property == "加粗" || property == "倾斜" || property == "删除线" || property == "下划线" ||
					property == "允许透明" || property == "通常" || property == "存档" ||
					property == "只读" || property == "系统" || property == "隐藏" || property == "热点跟踪")
					return { kTypeBool, false, true };
				if (property == "文本颜色" || property == "背景颜色" || property == "内背景颜色" ||
					property == "标题颜色" || property == "标题背景颜色" || property == "非本月颜色" ||
					property == "颜色" ||
					property == "线条颜色" || property == "填充颜色" || property == "画笔颜色" ||
					property == "刷子颜色" || property == "文本背景颜色" || property == "渐变边框颜色1" ||
					property == "渐变边框颜色2" || property == "渐变边框颜色3" || property == "渐变背景颜色1" ||
					property == "渐变背景颜色2" || property == "渐变背景颜色3")
					return { kTypeInt, false, true };
				if (property == "图片" || property == "底图" || property == "鼠标指针") return { kTypeBinary, false, true };
				return { kTypeText, false, true };
			}
			if (node.children.front()->kind == Kind::Name && !node.children.front()->text.empty() && node.children.front()->text.front() == '#') {
				const auto constant = program_.constants.find(node.children.front()->text + "." + node.text);
				if (constant != program_.constants.end()) return { constant->second.type, false, true };
			}
			const TypeRef base = Infer(method, *node.children.front());
			if (const TypeInfo* type = program_.FindType(base.code)) {
				for (const auto& field : type->elements) if (field.name == node.text) return field.type;
			}
			return {};
		}
		case Kind::Binary:
			if (node.text == "＝" || node.text == "=" || node.text == "==" || node.text == "≠" || node.text == "!=" || node.text == "<>" ||
				node.text == "＜" || node.text == "<" || node.text == "＞" || node.text == ">" || node.text == "≤" || node.text == "<=" || node.text == "≥" || node.text == ">=" || node.text == "且" || node.text == "或")
				return { kTypeBool, false, true };
			if (node.children.size() == 2) {
				const TypeRef left = Infer(method, *node.children[0]); const TypeRef right = Infer(method, *node.children[1]);
				if (node.text == "＋" || node.text == "+") {
					if (left.code == kTypeText || right.code == kTypeText) return { kTypeText, false, true };
				}
				return left.valid ? left : right;
			}
			return {};
		case Kind::Call: {
			if (node.children.empty()) return {};
			const auto& callee = *node.children.front(); const std::size_t count = node.children.size() - 1;
			if (callee.kind == Kind::Name) {
				if (const Method* owned = ResolveOwnedMethod(method, callee.text, count)) return owned->returnType;
				if (const Method* targetMethod = ResolveUnqualifiedMethod(method, callee.text, count)) return targetMethod->returnType;
				if (const DllCommand* dll = ResolveDllCommand(callee.text, count)) return dll->returnType;
				if (const auto command = ResolveGlobalCommand(callee.text, count)) {
					return { program_.NormalizeLibraryType(command->libraryIndex, command->command->returnType), (command->command->state & kCommandReturnsArray) != 0, true };
				}
			}
			else if (callee.kind == Kind::Member && !callee.children.empty()) {
				if (const auto operation = WindowMemberOperation(method, *callee.children.front(), callee.text))
					return WindowMemberReturnType(*operation);
				if (const Method* member = ResolveMemberMethod(Infer(method, *callee.children.front()), callee.text, count)) return member->returnType;
				if (const auto command = ResolveMemberCommand(Infer(method, *callee.children.front()), callee.text, count))
					return { program_.NormalizeLibraryType(command->libraryIndex, command->command->returnType), (command->command->state & kCommandReturnsArray) != 0, true };
			}
			return {};
		}
		default: return {};
		}
	}

	std::string EmitConstant(const Constant& constant) const
	{
		if (constant.type == kTypeText) return "Text(" + EscapeCppString(constant.textValue) + ")";
		if (constant.type == kTypeBool) return std::string("Boolean(") + (constant.numberValue != 0 ? "true" : "false") + ")";
		std::ostringstream value; value << std::setprecision(17) << constant.numberValue;
		return "Number(" + value.str() + ")";
	}

	std::string EmitLvalue(const Method& method, const e2txt::SourceExpressionNode& node)
	{
		using Kind = e2txt::SourceExpressionKind;
		if (node.kind == Kind::Name) {
			if (const auto variable = FindVariable(method, node.text)) return variable->first;
			if (IsWindowRootProperty(method, node.text)) {
				return "WindowGetProperty(" + std::to_string(WindowForMethod(method)->id) + "," + EscapeCppString(node.text) + ")";
			}
			Fail(method.sourceFile + ":" + std::to_string(method.sourceLine) + ": unknown_variable:" + node.text); return "*static_cast<Value*>(nullptr)";
		}
		if (node.kind == Kind::Index && node.children.size() >= 2) {
			std::string indexes = "{";
			for (std::size_t index = 1; index < node.children.size(); ++index)
				indexes += "ToInteger(" + EmitExpression(method, *node.children[index]) + "),";
			indexes += "}";
			return "IndexPath(" + EmitLvalue(method, *node.children.front()) + "," + indexes + ")";
		}
		if (node.kind == Kind::Member && !node.children.empty()) {
			std::string property;
			if (const auto* control = WindowPropertyTarget(method, node, property)) {
				return "WindowGetProperty(" + std::to_string(control->id) + "," + EscapeCppString(property) + ")";
			}
			const TypeRef base = Infer(method, *node.children.front()); const TypeInfo* type = program_.FindType(base.code);
			if (type != nullptr) {
				for (std::size_t index = 0; index < type->elements.size(); ++index)
					if (type->elements[index].name == node.text) return "Field(" + EmitLvalue(method, *node.children.front()) + ',' + std::to_string(index) + ')';
			}
			Fail(method.sourceFile + ":" + std::to_string(method.sourceLine) + ": unknown_member:" + node.text); return "*static_cast<Value*>(nullptr)";
		}
		Fail(method.sourceFile + ":" + std::to_string(method.sourceLine) + ": expression_is_not_assignable"); return "*static_cast<Value*>(nullptr)";
	}

	std::string EmitArg(const Method& method, const e2txt::SourceExpressionNode& node, const bool byReference)
	{
		bool referenceable = IsLvalue(node);
		std::uint32_t ignoredUnitId = 0;
		std::string ignoredProperty;
		if (IsWindowPropertyTarget(method, node, ignoredUnitId, ignoredProperty)) referenceable = false;
		// A named constant is an expression, not a mutable variable.  This is
		// important for generic all-type parameters such as console output.
		if (node.kind == e2txt::SourceExpressionKind::Name && !FindVariable(method, node.text)) referenceable = false;
		return byReference && referenceable ? "Arg::Ref(" + EmitLvalue(method, node) + ')' : "Arg::Temp(" + EmitExpression(method, node) + ')';
	}

	std::string EmitBuiltin(const Method& method, const CommandBinding& binding, const e2txt::SourceExpressionNode& call)
	{
		const std::string& operation = binding.command->englishName;
		const auto arg = [&](const std::size_t index) -> const e2txt::SourceExpressionNode& { return *call.children[index + 1]; };
		if (operation == "MachineCode") {
			Fail(method.sourceFile + ":" + std::to_string(method.sourceLine) + ": machine_code_must_be_statement");
			return "Empty()";
		}
		if (operation == "hex") return call.children.size() >= 2 ? "ParseHexValue(" + EmitExpression(method, arg(0)) + ")" : "Integer(0)";
		if (operation == "binary") return call.children.size() >= 2 ? "ParseBinaryValue(" + EmitExpression(method, arg(0)) + ")" : "Integer(0)";
		if (operation == "XchgVar" || operation == "ForceXchgVar") {
			if (call.children.size() < 3 || !IsLvalue(arg(0)) || !IsLvalue(arg(1))) {
				Fail(method.sourceFile + ":" + std::to_string(method.sourceLine) + ": exchange_requires_two_variables");
				return "Empty()";
			}
			return "Exchange(" + EmitLvalue(method, arg(0)) + "," + EmitLvalue(method, arg(1)) + ")";
		}
		if (operation == "GetRuntimeDataType") return call.children.size() >= 2 ? "RuntimeType(" + EmitExpression(method, arg(0)) + ")" : "Integer(0)";
		if (operation == "this") return "Integer(static_cast<long long>(reinterpret_cast<std::uintptr_t>(self)),T_SUB)";
		if (operation == "IsCondMacroDefined") {
			return call.children.size() >= 2 ? "ConditionMacro(" + EmitExpression(method, arg(0)) + "," + EscapeCppString(ConditionMacroList()) + ")" : "Boolean(false)";
		}
		if (operation == "GetAppName") {
			if (call.children.size() < 2) return "Text(\"\")";
			long long selector = 0;
			if (!TryEvaluateInteger(method, arg(0), selector)) return "Text(\"\")";
			std::string value;
			if (selector == 1) value = method.name;
			else if (selector == 2 && method.assemblyIndex < program_.assemblies.size()) value = program_.assemblies[method.assemblyIndex].name;
			else if (selector == 3) value = program_.bundle.sourcePath;
			else if (selector == 4) value = program_.bundle.projectName;
			else if (selector == 6) {
				const std::size_t slash = program_.bundle.sourcePath.find_last_of("\\/");
				value = program_.bundle.sourcePath.substr(slash == std::string::npos ? 0 : slash + 1);
				const std::size_t dot = value.find_last_of('.');
				if (dot != std::string::npos) value.erase(dot);
			}
			return "Text(" + EscapeCppString(value) + ")";
		}
		if (operation == "ReDim") {
			std::string dimensions = "std::vector<int>{";
			for (std::size_t index = 2; index + 1 < call.children.size(); ++index) dimensions += "static_cast<int>(ToInteger(" + EmitExpression(method, arg(index)) + ")),";
			dimensions += '}';
			return "(Redim(" + EmitLvalue(method, arg(0)) + ',' + dimensions + ",ToBool(" + EmitExpression(method, arg(1)) + ")),Empty())";
		}
		if (operation == "GetAryElementCount") return "AryCount(" + EmitLvalue(method, arg(0)) + ')';
		if (operation == "CopyAry") return "CopyAry(" + EmitLvalue(method, arg(0)) + ',' + EmitLvalue(method, arg(1)) + ')';
		if (operation == "AddElement") return "AddElement(" + EmitLvalue(method, arg(0)) + ',' + EmitExpression(method, arg(1)) + ')';
		if (operation == "InsElement") return "InsertElement(" + EmitLvalue(method, arg(0)) + ",static_cast<int>(ToInteger(" + EmitExpression(method, arg(1)) + "))," + EmitExpression(method, arg(2)) + ')';
		if (operation == "RemoveElement") {
			const std::string count = call.children.size() <= 3 || arg(2).kind == e2txt::SourceExpressionKind::Missing ? "1" : "static_cast<int>(ToInteger(" + EmitExpression(method, arg(2)) + "))";
			return "RemoveElement(" + EmitLvalue(method, arg(0)) + ",static_cast<int>(ToInteger(" + EmitExpression(method, arg(1)) + "))," + count + ')';
		}
		if (operation == "RemoveAll") return "RemoveAll(" + EmitLvalue(method, arg(0)) + ')';
		if (operation == "dir") return "FindFile(" + EmitExpression(method, arg(0)) + ')';
		if (operation == "not") return "Not(" + EmitExpression(method, arg(0)) + ')';
		Fail("unsupported_compiler_primitive:" + binding.command->name + ':' + operation); return "Empty()";
	}

	std::string DllCType(const TypeRef type, const bool byReference) const
	{
		if (type.isArray) return byReference ? "void**" : "void*";
		std::string base;
		switch (type.code) {
		case kTypeByte: base = "unsigned char"; break;
		case kTypeShort: base = "short"; break;
		case kTypeInt: case kTypeBool: case kTypeAll: base = "int"; break;
		// A subroutine value is an address.  It is four bytes in the x86
		// language ABI and eight bytes in an x64 process; void* keeps the
		// generated public/import signature pointer-sized on both targets.
		case kTypeSubroutine: base = "void*"; break;
		case kTypeInt64: base = "long long"; break;
		case kTypeFloat: base = "float"; break;
		case kTypeDouble: case kTypeDateTime: base = "double"; break;
		case kTypeText: base = "char*"; break;
		case kTypeBinary: base = "unsigned char*"; break;
		default: base = "void*"; break;
		}
		if (!byReference) {
			if (type.code == kTypeText) return "const char*";
			return base;
		}
		return base + "*";
	}

	std::string PlatformImportCType(const TypeRef type) const
	{
		if (type.isArray) return "void*";
		switch (type.code) {
		case kTypeByte: return "unsigned char";
		case kTypeShort: return "short";
		case kTypeInt: case kTypeBool: return "int";
		case kTypeSubroutine: return "void*";
		case kTypeInt64: return "long long";
		case kTypeFloat: return "float";
		case kTypeDouble: case kTypeDateTime: return "double";
		case kTypeText: return "const char*";
		default: return "void*";
		}
	}

	std::string DllReturnCType(const TypeRef type) const
	{
		return type.code == kTypeNull ? "void" : DllCType(type, false);
	}

	std::string ImportLinkerSymbol(
		const std::string& name,
		const bool isCdecl,
		const std::size_t stackBytes) const
	{
		if (program_.targetArchitecture == TargetArchitecture::X64) return name;
		return "_" + name + (isCdecl ? std::string() : std::string("@") + std::to_string(stackBytes));
	}

	std::size_t AbiStackBytes(const TypeRef type, const bool byReference) const
	{
		if (program_.targetArchitecture == TargetArchitecture::X64) return 0;
		if (byReference || type.isArray || type.code == kTypeText ||
			type.code == kTypeBinary || type.code == kTypeSubroutine ||
			program_.FindType(type.code) != nullptr) return 4;
		switch (type.code) {
		case kTypeInt64:
		case kTypeDouble:
		case kTypeDateTime:
			return 8;
		default:
			return 4;
		}
	}

	std::size_t AbiParameterBytes(const std::vector<Variable>& parameters) const
	{
		std::size_t bytes = 0;
		for (const Variable& parameter : parameters) bytes += AbiStackBytes(parameter.type, parameter.byReference);
		return bytes;
	}

	static std::string LinkerDirectiveName(std::string value)
	{
		for (char& character : value) {
			if (character == '"' || character == '\\' || character == '\r' || character == '\n') character = '_';
		}
		return value;
	}

	std::string DllValueExpression(const TypeRef type, const std::string& valueName) const
	{
		if (type.isArray) return valueName + ".missing?nullptr:__dll_array_" + valueName;
		switch (type.code) {
		case kTypeText: return valueName + ".missing?nullptr:DllText(" + valueName + ")";
		case kTypeBinary: return valueName + ".missing?nullptr:DllBinary(" + valueName + ")";
		case kTypeSubroutine: return "reinterpret_cast<void*>(static_cast<std::uintptr_t>(ToInteger(" + valueName + ")))";
		default:
			if (program_.FindType(type.code) != nullptr) return "DllObject(" + valueName + ")";
			if (type.code == kTypeFloat || type.code == kTypeDouble || type.code == kTypeDateTime)
				return "static_cast<" + DllCType(type, false) + ">(ToNumber(" + valueName + "))";
			return "static_cast<" + DllCType(type, false) + ">(ToInteger(" + valueName + "))";
		}
	}

	std::string EmitDllCall(const Method& method, const DllCommand& command, const e2txt::SourceExpressionNode& call)
	{
		const TypeRef returnType = command.returnType;
		const std::size_t commandIndex = static_cast<std::size_t>(&command - program_.dllCommands.data());
		const std::string importSymbol = RegisterDllImport(command, commandIndex);
		std::ostringstream result;
		result << "([&](){ ArenaScope __dll_scope;";
		std::vector<std::string> callArguments;
		std::vector<std::string> syncStatements;
		for (std::size_t index = 0; index < command.parameters.size(); ++index) {
			const Variable& parameter = command.parameters[index];
			const bool provided = index + 1 < call.children.size() && call.children[index + 1]->kind != e2txt::SourceExpressionKind::Missing;
			const e2txt::SourceExpressionNode* source = provided ? call.children[index + 1].get() : nullptr;
			const bool referenceable = source != nullptr && IsLvalue(*source) &&
				!(source->kind == e2txt::SourceExpressionKind::Name && !FindVariable(method, source->text));
			const TypeRef effectiveType = parameter.type.code == kTypeAll && source != nullptr ? Infer(method, *source) : parameter.type;
			const TypeRef type = effectiveType.valid ? effectiveType : TypeRef { kTypeInt, false, true };
			const std::string valueName = "__dll_value_" + std::to_string(index);
			if (referenceable && parameter.byReference) result << "Value& __dll_target_" << index << "=" << EmitLvalue(method, *source) << ";";
			result << "Value " << valueName << "=" << (source == nullptr ? "Missing()" : EmitExpression(method, *source)) << ";";
			if (type.isArray) {
				result << "void* __dll_array_" << valueName << "=" << valueName << ".missing?nullptr:DllArray(" << valueName << ",__dll_scope.arena);";
				if (parameter.byReference) {
					result << "void* __dll_array_slot_" << index << "=__dll_array_" << valueName << ";";
					callArguments.push_back("&__dll_array_slot_" + std::to_string(index));
					if (referenceable) syncStatements.push_back("if(__dll_array_slot_" + std::to_string(index) + ") { ReadArray(" + valueName + "," + Hex(type.code) + ",static_cast<const unsigned char*>(__dll_array_slot_" + std::to_string(index) + ")); Assign(__dll_target_" + std::to_string(index) + "," + valueName + "); }");
				}
				else callArguments.push_back("__dll_array_" + valueName);
				continue;
			}
			if (!parameter.byReference) {
				const bool platformComposite = IsPlatformImportModule(command.fileName) &&
					program_.FindType(type.code) != nullptr;
				if (referenceable && platformComposite) {
					result << "Value& __dll_target_" << index << "=" << EmitLvalue(method, *source) << ";";
					syncStatements.push_back(
						"if(!" + valueName + ".object.empty()) { ReadObject(" + valueName + "," + valueName + ".object.data()); Assign(__dll_target_" +
						std::to_string(index) + "," + valueName + "); }");
				}
				callArguments.push_back(DllValueExpression(type, valueName));
				continue;
			}
			const std::string cType = DllCType(type, false);
			if (type.code == kTypeText) {
				result << "char* __dll_ref_" << index << "=" << valueName << ".missing?nullptr:DllTextReference(" << valueName << "); char* __dll_initial_" << index << "=__dll_ref_" << index << ";";
				callArguments.push_back("&__dll_ref_" + std::to_string(index));
				if (referenceable) syncStatements.push_back("DllCaptureText(" + valueName + ",__dll_ref_" + std::to_string(index) + ",__dll_initial_" + std::to_string(index) + "); Assign(__dll_target_" + std::to_string(index) + "," + valueName + ");");
				else syncStatements.push_back("if(__dll_ref_" + std::to_string(index) + "==__dll_initial_" + std::to_string(index) + ") RuntimeFree(__dll_initial_" + std::to_string(index) + ");");
				continue;
			}
			if (type.code == kTypeBinary) {
				result << "unsigned char* __dll_ref_" << index << "=DllBinary(" << valueName << ");";
				callArguments.push_back("&__dll_ref_" + std::to_string(index));
				if (referenceable) syncStatements.push_back("Assign(__dll_target_" + std::to_string(index) + "," + valueName + ");");
				continue;
			}
			if (type.code != kTypeByte && type.code != kTypeShort && type.code != kTypeInt &&
				type.code != kTypeBool && type.code != kTypeSubroutine && type.code != kTypeInt64 &&
				type.code != kTypeFloat && type.code != kTypeDouble && type.code != kTypeDateTime) {
				result << "void* __dll_ref_" << index << "=DllObject(" << valueName << ");";
				callArguments.push_back(IsPlatformImportModule(command.fileName) ? "__dll_ref_" + std::to_string(index) : "&__dll_ref_" + std::to_string(index));
				if (referenceable) syncStatements.push_back("if(__dll_ref_" + std::to_string(index) + ") ReadObject(" + valueName + ",__dll_ref_" + std::to_string(index) + "); Assign(__dll_target_" + std::to_string(index) + "," + valueName + ");");
				continue;
			}
			result << cType << " __dll_ref_" << index << "=" << DllValueExpression(type, valueName) << ";";
			callArguments.push_back("&__dll_ref_" + std::to_string(index));
			if (referenceable) {
				const std::string converted = (type.code == kTypeFloat || type.code == kTypeDouble || type.code == kTypeDateTime)
					? "Number(__dll_ref_" + std::to_string(index) + ")"
					: "Integer(__dll_ref_" + std::to_string(index) + "," + Hex(type.code) + ")";
				syncStatements.push_back("Assign(__dll_target_" + std::to_string(index) + "," + converted + ");");
			}
		}
		const std::string joinedArguments = [&]() {
			std::ostringstream output;
			for (std::size_t index = 0; index < callArguments.size(); ++index) { if (index != 0) output << ','; output << callArguments[index]; }
			return output.str();
		}();
		// Every DLL declaration goes through a compiler-generated import alias.
		// Calling a platform entry by its header name (for example
		// InterlockedIncrement or abs) is ambiguous in C++ and also bypasses the
		// PE import-library path.  The alias has the exact declaration assembled
		// in RegisterDllImport and is mapped to the real import symbol by the
		// generated linker directive.
		const std::string invoked = importSymbol + "(" + joinedArguments + ")";
		if (returnType.code == kTypeNull) {
			result << invoked << ";";
		}
		else {
			result << "auto __dll_result=" << invoked << ";";
		}
		for (const std::string& statement : syncStatements) result << statement;
		if (returnType.code == kTypeNull) result << "return Empty();";
		else if (returnType.isArray) result << "MData __dll_data{}; __dll_data.type=" << Hex(returnType.code) << "|T_ARRAY; __dll_data.pointerValue=__dll_result; return CopyReturned(__dll_data," << Hex(returnType.code) << ",true);";
		else if (returnType.code == kTypeText) result << "return DllTextResult(__dll_result);";
		else if (returnType.code == kTypeBinary) result << "return DllBinaryResult(__dll_result);";
		else if (returnType.code == kTypeFloat || returnType.code == kTypeDouble || returnType.code == kTypeDateTime) result << "return Number(__dll_result);";
		else if (returnType.code == kTypeBool) result << "return Boolean(__dll_result!=0);";
		else if (returnType.code == kTypeSubroutine) result << "return Integer(static_cast<long long>(reinterpret_cast<std::uintptr_t>(__dll_result))," << Hex(returnType.code) << ");";
		else if (returnType.code == kTypeByte || returnType.code == kTypeShort || returnType.code == kTypeInt || returnType.code == kTypeInt64) result << "return Integer(__dll_result," << Hex(returnType.code) << ");";
		else result << "Value __dll_value=MakeVar(" << Hex(returnType.code) << ",false,false); if(__dll_result) ReadObject(__dll_value,__dll_result); return __dll_value;";
		result << "})()";
		return result.str();
	}

	std::string RegisterDllImport(const DllCommand& command, const std::size_t commandIndex)
	{
		const auto found = dllImportSymbols_.find(commandIndex);
		if (found != dllImportSymbols_.end()) return found->second;
		// Keep the local name independent from the DLL entry name.  Besides
		// avoiding collisions with C/C++ declarations, this makes platform and
		// third-party DLLs follow one import-table contract.
		const std::string symbol = "ecompiler_import_" + std::to_string(commandIndex);
		const std::string returnType = DllReturnCType(command.returnType);
		declarations_ << "extern \"C\" __declspec(dllimport) " << returnType << ' '
			<< (command.usesCdecl ? "__cdecl" : "__stdcall") << ' ' << symbol << "(";
		for (std::size_t index = 0; index < command.parameters.size(); ++index) {
			if (index != 0) declarations_ << ',';
			const TypeRef type = command.parameters[index].type.valid ? command.parameters[index].type : TypeRef { kTypeInt, false, true };
			// A by-reference composite passed to a Windows API is a pointer to
			// the object storage, while an ordinary E DLL uses the pointer slot
			// described by DllCType.  Keep this distinction in the declaration as
			// well as in EmitDllCall's argument marshalling.
			declarations_ << ((IsPlatformImportModule(command.fileName) && command.parameters[index].byReference && program_.FindType(type.code) != nullptr)
				? "void*"
				: DllCType(type, command.parameters[index].byReference));
		}
		declarations_ << ");\n";
		const std::string localSymbol = ImportLinkerSymbol(
			symbol,
			command.usesCdecl,
			AbiParameterBytes(command.parameters));
		const std::string targetSymbol = ImportLinkerSymbol(
			command.entryName,
			true,
			0);
		const std::string localImportSymbol = LinkerDirectiveName("__imp_" + localSymbol);
		const std::string targetImportSymbol = LinkerDirectiveName("__imp_" + targetSymbol);
		declarations_ << "#pragma comment(linker,\"/alternatename:" << localImportSymbol
			<< "=" << targetImportSymbol << "\")\n";
		// The non-__imp form is useful when the compiler folds a call into a
		// direct reference (and is harmless for a normal dllimport thunk).
		declarations_ << "#pragma comment(linker,\"/alternatename:" << LinkerDirectiveName(localSymbol)
			<< "=" << LinkerDirectiveName(targetSymbol) << "\")\n";
		imports_.push_back({commandIndex, command.fileName, command.entryName, symbol, command.usesCdecl, AbiParameterBytes(command.parameters)});
		dllImportSymbols_.emplace(commandIndex, symbol);
		return symbol;
	}

	std::string EmitFneCall(const Method& method, const CommandBinding& binding, const e2txt::SourceExpressionNode& call, const e2txt::SourceExpressionNode* receiver)
	{
		const auto& command = *binding.command;
		if (IsCompilePrimitive(command)) return EmitBuiltin(method, binding, call);
		const bool isMsgBox = receiver == nullptr &&
			(command.englishName == "MsgBox" || command.name == "信息框");
		if (program_.targetArchitecture == TargetArchitecture::X64 && isMsgBox) {
			const auto argument = [&](const std::size_t index, const char* fallback) {
				return index < call.children.size() && call.children[index]->kind != e2txt::SourceExpressionKind::Missing
					? EmitExpression(method, *call.children[index])
					: std::string(fallback);
			};
			return "NativeMsgBox(" + argument(1, "Text(\"\")") + "," + argument(2, "Integer(0)") + "," +
				argument(3, "Missing()") + "," + argument(4, "Missing()") + ")";
		}
		if (command.executeSymbol.empty()) {
			const std::string libraryName = binding.libraryIndex < program_.libraries.size()
				? program_.libraries[binding.libraryIndex].dependency.name
				: std::string();
			Fail(method.sourceFile + ":" + std::to_string(method.sourceLine) +
				": support_library_implementation_unavailable:" +
				(libraryName.empty() ? std::string("<unknown>") : libraryName) + ":" + command.name);
			return "Empty()";
		}
		if (!IsCppIdentifier(command.executeSymbol)) { Fail("invalid_fne_execute_symbol:" + command.executeSymbol); return "Empty()"; }
		reachableLibraries_.insert(binding.libraryIndex);
		reachableSymbols_.insert(command.executeSymbol);
		reachableCommands_.insert({ binding.libraryIndex, binding.commandIndex });
		const std::uint32_t returnType = program_.NormalizeLibraryType(binding.libraryIndex, command.returnType);
		usedTypes_.insert(returnType);
		std::string arguments = "{"; std::string specs = "{";
		if (receiver != nullptr) {
			arguments += EmitArg(method, *receiver, true) + ',';
			const TypeRef type = Infer(method, *receiver); specs += "{" + Hex(type.code) + ",0},";
			usedTypes_.insert(type.code);
		}
		// The legacy MsgBox ABI gives the title a default value inside the core.
		// The modern adapter's default is UTF-8 while this FNE text ABI is CP936,
		// so materialize the omitted title in the generated call.  Emit the
		// preceding omitted button argument as well when a call has only one arg.
		const std::size_t sourceArgumentCount = call.children.size() - 1;
		const std::size_t emittedArgumentCount = isMsgBox
			? (std::max)(sourceArgumentCount, std::size_t(3))
			: sourceArgumentCount;
		for (std::size_t index = 1; index <= emittedArgumentCount; ++index) {
			const std::size_t metadataIndex = (std::min)(index - 1, command.arguments.empty() ? std::size_t(0) : command.arguments.size() - 1);
			std::uint32_t argumentState = 0;
			std::uint32_t argumentType = kTypeAll;
			if (command.arguments.empty()) specs += "{" + Hex(kTypeAll) + ",0},";
			else {
				const auto& metadata = command.arguments[metadataIndex];
				argumentState = metadata.state;
				argumentType = program_.NormalizeLibraryType(binding.libraryIndex, metadata.dataType);
				usedTypes_.insert(argumentType);
				specs += "{" + Hex(argumentType) + ',' + Hex(metadata.state) + "},";
			}
			if (argumentType == kTypeStatement) {
				if (index >= call.children.size()) {
					arguments += "Arg::Temp(Missing()),";
				}
				else {
					arguments += "Arg::Statement([&](){return " + EmitExpression(method, *call.children[index]) + ";}),";
				}
				continue;
			}
			const bool byReference = (argumentState & (kArgumentReceivesVariable | kArgumentReceivesVariableArray |
				kArgumentReceivesVariableOrArray | kArgumentReceivesArrayData |
				kArgumentReceivesVariableOrOther)) != 0;
			const bool missingTitle = isMsgBox && index == 3 &&
				(index >= call.children.size() ||
				 call.children[index]->kind == e2txt::SourceExpressionKind::Missing);
			if (missingTitle) {
				// CP936 bytes for the IDE/core default title "信息：".
				arguments += "Arg::Temp(Text(\"\\320\\305\\317\\242\\243\\272\")),";
			}
			else if (index >= call.children.size()) {
				arguments += "Arg::Temp(Missing()),";
			}
			else {
				arguments += EmitArg(method, *call.children[index], byReference) + ',';
			}
		}
		arguments += '}'; specs += '}';
		return "CallFne(\"" + command.executeSymbol + "\",&" + command.executeSymbol + ',' + Hex(returnType) + ',' + ((command.state & kCommandReturnsArray) != 0 ? "true" : "false") + ',' + arguments + ',' + specs + ')';
	}

	std::string EmitCall(const Method& method, const e2txt::SourceExpressionNode& node)
	{
		using Kind = e2txt::SourceExpressionKind;
		if (node.children.empty()) { Fail("call_target_missing"); return "Empty()"; }
		const auto& callee = *node.children.front(); const std::size_t argumentCount = node.children.size() - 1;
		if (callee.kind == Kind::Name) {
			if (const Method* owned = ResolveOwnedMethod(method, callee.text, argumentCount)) {
				if (argumentCount > owned->parameters.size()) { Fail("too_many_owned_method_arguments:" + callee.text); return "Empty()"; }
				QueueMethod(owned->id); std::string arguments = "{";
				for (std::size_t index = 1; index < node.children.size(); ++index) {
					const bool byReference = index - 1 < owned->parameters.size() && owned->parameters[index - 1].byReference;
					arguments += EmitArg(method, *node.children[index], byReference) + ',';
				}
				arguments += '}'; return "method_" + std::to_string(owned->id) + '(' + arguments + ',' + (method.ownerType.valid ? "self" : "nullptr") + ')';
			}
			if (const Method* targetMethod = ResolveUnqualifiedMethod(method, callee.text, argumentCount)) {
				if (argumentCount > targetMethod->parameters.size()) { Fail("too_many_method_arguments:" + callee.text); return "Empty()"; }
				QueueMethod(targetMethod->id); std::string arguments = "{";
				for (std::size_t index = 1; index < node.children.size(); ++index) {
					const std::size_t parameterIndex = index - 1;
					const bool byReference = parameterIndex < targetMethod->parameters.size() && targetMethod->parameters[parameterIndex].byReference;
					const bool referenceable = IsLvalue(*node.children[index]) && !(node.children[index]->kind == Kind::Name && !FindVariable(method, node.children[index]->text));
					if (byReference && !referenceable) { Fail(method.sourceFile + ": reference_parameter_requires_variable:" + callee.text); return "Empty()"; }
					arguments += EmitArg(method, *node.children[index], byReference) + ',';
				}
				arguments += '}'; return "method_" + std::to_string(targetMethod->id) + '(' + arguments + ",nullptr)";
			}
			if (const DllCommand* dll = ResolveDllCommand(callee.text, argumentCount)) return EmitDllCall(method, *dll, node);
			const auto binding = ResolveGlobalCommand(callee.text, argumentCount);
			if (!binding) {
				Fail(method.sourceFile + ": unknown_call:" + callee.text + "/" + std::to_string(argumentCount));
				return "Empty()";
			}
			return EmitFneCall(method, *binding, node, nullptr);
		}
		if (callee.kind == Kind::Member && !callee.children.empty()) {
			if (const auto operation = WindowMemberOperation(method, *callee.children.front(), callee.text)) {
				const auto targetId = WindowTargetId(method, callee.children.front()->text);
				if (!targetId) { Fail(method.sourceFile + ": unknown_window_target:" + callee.children.front()->text); return "Empty()"; }
				std::string arguments = "{";
				for (std::size_t index = 1; index < node.children.size(); ++index)
					arguments += EmitExpression(method, *node.children[index]) + ',';
				arguments += '}';
				return "WindowInvokeMember(" + std::to_string(*targetId) + "u," +
					EscapeCppString(*operation) + ",std::vector<Value>" + arguments + ")";
			}
			const TypeRef receiverType = Infer(method, *callee.children.front());
			if (const Method* member = ResolveMemberMethod(receiverType, callee.text, argumentCount)) {
				if (argumentCount > member->parameters.size()) { Fail("too_many_member_method_arguments:" + callee.text); return "Empty()"; }
				QueueMethod(member->id);
				std::string arguments = "{";
				for (std::size_t index = 1; index < node.children.size(); ++index) {
					const bool byReference = index - 1 < member->parameters.size() && member->parameters[index - 1].byReference;
					arguments += EmitArg(method, *node.children[index], byReference) + ',';
				}
				arguments += '}';
				return "method_" + std::to_string(member->id) + '(' + arguments + ",&" + EmitLvalue(method, *callee.children.front()) + ')';
			}
			const auto binding = ResolveMemberCommand(receiverType, callee.text, argumentCount);
			if (!binding) { Fail(method.sourceFile + ": unknown_member_call:" + callee.text + "/" + std::to_string(argumentCount)); return "Empty()"; }
			return EmitFneCall(method, *binding, node, callee.children.front().get());
		}
		Fail("unsupported_call_target"); return "Empty()";
	}

	bool TryEvaluateInteger(const Method& method, const e2txt::SourceExpressionNode& node, long long& result) const
	{
		using Kind = e2txt::SourceExpressionKind;
		if (node.kind == Kind::NumberLiteral) {
			char* end = nullptr; const long long value = std::strtoll(node.text.c_str(), &end, 10);
			if (end != nullptr && end != node.text.c_str() && *end == '\0') { result = value; return true; }
			return false;
		}
		if (node.kind == Kind::LogicalLiteral) { result = node.text == "真" ? 1 : 0; return true; }
		if (node.kind == Kind::Group && node.children.size() == 1) return TryEvaluateInteger(method, *node.children.front(), result);
		if (node.kind == Kind::Unary && node.children.size() == 1) { long long value = 0; if (!TryEvaluateInteger(method, *node.children.front(), value)) return false; if (node.text == "－" || node.text == "-") value = -value; result = value; return true; }
		if (node.kind == Kind::Name) {
			const auto constant = program_.constants.find(node.text);
			if (constant != program_.constants.end() && constant->second.type != kTypeText) { result = static_cast<long long>(constant->second.numberValue); return true; }
		}
		if (node.kind == Kind::Binary && node.children.size() == 2) {
			long long left = 0, right = 0; if (!TryEvaluateInteger(method, *node.children[0], left) || !TryEvaluateInteger(method, *node.children[1], right)) return false;
			if (node.text == "＋" || node.text == "+") result = left + right;
			else if (node.text == "－" || node.text == "-") result = left - right;
			else if (node.text == "×" || node.text == "*") result = left * right;
			else if (node.text == "\\") result = right == 0 ? 0 : left / right;
			else if (node.text == "%") result = right == 0 ? 0 : left % right;
			else return false;
			return true;
		}
		return false;
	}

	std::string ConditionMacroList() const
	{
		std::vector<std::string> macros(program_.conditionMacros.begin(), program_.conditionMacros.end());
		std::sort(macros.begin(), macros.end());
		std::string result;
		for (const std::string& macro : macros) { if (!result.empty()) result.push_back(','); result += macro; }
		return result;
	}

	std::string EmitExpression(const Method& method, const e2txt::SourceExpressionNode& node)
	{
		using Kind = e2txt::SourceExpressionKind;
		switch (node.kind) {
		case Kind::Missing: return "Missing()";
		case Kind::NumberLiteral:
			return node.text.find('.') == std::string::npos ? "Integer(" + node.text + "LL)" : "Number(" + node.text + ')';
		case Kind::TextLiteral: return "Text(" + EscapeCppString(node.text) + ')';
		case Kind::LogicalLiteral: return std::string("Boolean(") + (node.text == "真" ? "true" : "false") + ')';
		case Kind::DateTimeLiteral: return DateTimeValue(node.text);
		case Kind::ByteSetLiteral: {
			std::string result = "Bytes({"; for (const auto& child : node.children) result += "static_cast<unsigned char>(ToInteger(" + EmitExpression(method, *child) + ")),"; return result + "})";
		}
		case Kind::Name:
			if (IsWindowRootProperty(method, node.text)) {
				return "WindowGetProperty(" + std::to_string(WindowForMethod(method)->id) + "," + EscapeCppString(node.text) + ")";
			}
			if (const auto variable = FindVariable(method, node.text)) return variable->first;
			if (const auto constant = program_.constants.find(node.text); constant != program_.constants.end()) return EmitConstant(constant->second);
			Fail(method.sourceFile + ": unknown_name:" + node.text); return "Empty()";
		case Kind::Call: return EmitCall(method, node);
		case Kind::Member:
			{
				std::string property;
				if (const auto* control = WindowPropertyTarget(method, node, property)) {
					return "WindowGetProperty(" + std::to_string(control->id) + "," + EscapeCppString(property) + ")";
				}
			}
			if (!node.children.empty() && node.children.front()->kind == Kind::Name && !node.children.front()->text.empty() && node.children.front()->text.front() == '#') {
				const std::string qualified = node.children.front()->text + "." + node.text;
				if (const auto constant = program_.constants.find(qualified); constant != program_.constants.end()) return EmitConstant(constant->second);
			}
			return EmitLvalue(method, node);
		case Kind::Index: return EmitLvalue(method, node);
		case Kind::Group: return node.children.empty() ? "Empty()" : '(' + EmitExpression(method, *node.children.front()) + ')';
		case Kind::AddressOf: return node.children.empty() ? "Empty()" : EmitExpression(method, *node.children.front());
		case Kind::Unary:
			if (node.children.empty()) return "Empty()";
			if (node.text == "－" || node.text == "-") return "Neg(" + EmitExpression(method, *node.children.front()) + ')';
			if (node.text == "!" ) return "Not(" + EmitExpression(method, *node.children.front()) + ')';
			return EmitExpression(method, *node.children.front());
		case Kind::Binary: {
			if (node.children.size() != 2) { Fail("binary_operand_count"); return "Empty()"; }
			const std::string left = EmitExpression(method, *node.children[0]); const std::string right = EmitExpression(method, *node.children[1]);
			const std::string& op = node.text;
			if (op == "＋" || op == "+") return "Add(" + left + ',' + right + ')';
			if (op == "－" || op == "-") return "Sub(" + left + ',' + right + ')';
			if (op == "×" || op == "*") return "Mul(" + left + ',' + right + ')';
			if (op == "÷" || op == "/") return "Div(" + left + ',' + right + ')';
			if (op == "\\") return "IDiv(" + left + ',' + right + ')';
			if (op == "%" || op == "％") return "Mod(" + left + ',' + right + ')';
			if (op == "＝" || op == "=" || op == "==" || op == "?=") return "Eq(" + left + ',' + right + ')';
			if (op == "≠" || op == "!=" || op == "<>") return "Ne(" + left + ',' + right + ')';
			if (op == "＜" || op == "<") return "Lt(" + left + ',' + right + ')';
			if (op == "≤" || op == "<=") return "Le(" + left + ',' + right + ')';
			if (op == "＞" || op == ">") return "Gt(" + left + ',' + right + ')';
			if (op == "≥" || op == ">=") return "Ge(" + left + ',' + right + ')';
			if (op == "且" || op == "&") return "And(" + left + ',' + right + ')';
			if (op == "或" || op == "|") return "Or(" + left + ',' + right + ')';
			Fail("unsupported_operator:" + op); return "Empty()";
		}
		default: Fail("unsupported_expression_kind"); return "Empty()";
		}
	}

	bool EmitStatements(const Method& method, const std::vector<Statement>& statements, const int indent)
	{
		for (const Statement& statement : statements) {
			SourceLine(method.sourceFile, statement.sourceLine);
			switch (statement.kind) {
			case StatementKind::Expression: Line(indent, "(void)" + EmitExpression(method, *statement.expression) + ";"); break;
			case StatementKind::Assignment: {
				std::uint32_t unitId = 0;
				std::string property;
				if (IsWindowPropertyTarget(method, *statement.target, unitId, property)) {
					Line(indent, "WindowSetProperty(" + std::to_string(unitId) + "," + EscapeCppString(property) + "," + EmitExpression(method, *statement.expression) + ");");
				}
				else {
					Line(indent, "Assign(" + EmitLvalue(method, *statement.target) + ',' + EmitExpression(method, *statement.expression) + ");");
				}
				break;
			}
			case StatementKind::Return: Line(indent, "return " + (statement.expression ? EmitExpression(method, *statement.expression) : "Empty()") + ";"); break;
			case StatementKind::IfTrue: case StatementKind::IfElse:
				Line(indent, "if(ToBool(" + EmitExpression(method, *statement.expression) + ")) {"); if (!EmitStatements(method, statement.body, indent + 1)) return false; Line(indent, "}");
				if (!statement.elseBody.empty()) { Line(indent, "else {"); if (!EmitStatements(method, statement.elseBody, indent + 1)) return false; Line(indent, "}"); } break;
			case StatementKind::Switch:
				for (std::size_t index = 0; index < statement.branches.size(); ++index) {
					Line(indent, std::string(index == 0 ? "if" : "else if") + "(ToBool(" + EmitExpression(method, *statement.branches[index].condition) + ")) {");
					if (!EmitStatements(method, statement.branches[index].body, indent + 1)) return false; Line(indent, "}");
				}
				if (!statement.elseBody.empty()) { Line(indent, "else {"); if (!EmitStatements(method, statement.elseBody, indent + 1)) return false; Line(indent, "}"); } break;
			case StatementKind::While:
				Line(indent, "while(ToBool(" + EmitExpression(method, *statement.expression) + ")) {"); if (!EmitStatements(method, statement.body, indent + 1)) return false; Line(indent, "}"); break;
			case StatementKind::DoWhile:
				Line(indent, "do {"); if (!EmitStatements(method, statement.body, indent + 1)) return false; Line(indent, "} while(ToBool(" + EmitExpression(method, *statement.expression) + ")); "); break;
			case StatementKind::CountLoop: {
				if (statement.arguments.empty()) return Fail(method.sourceFile + ": count_loop_argument_missing");
				const std::string counter = statement.arguments.size() >= 2 && statement.arguments[1]->kind != e2txt::SourceExpressionKind::Missing ? EmitLvalue(method, *statement.arguments[1]) : "__counter";
				if (counter == "__counter") Line(indent, "Value __counter=MakeVar(T_INT);");
				Line(indent, "for(int __limit=static_cast<int>(ToInteger(" + EmitExpression(method, *statement.arguments[0]) + ")),__i=1;__i<=__limit;++__i) {");
				Line(indent + 1, "Assign(" + counter + ",Integer(__i));"); if (!EmitStatements(method, statement.body, indent + 1)) return false; Line(indent, "}"); break;
			}
            case StatementKind::ForLoop: {
                if (statement.arguments.size() < 3) return Fail(method.sourceFile + ": variable_loop_requires_three_arguments");
                const bool hasVariable = statement.arguments.size() >= 4 && statement.arguments[3]->kind != e2txt::SourceExpressionKind::Missing;
                const std::string variable = hasVariable ? EmitLvalue(method, *statement.arguments[3]) : "__for_counter";
                Line(indent, "{ double __begin=ToNumber(" + EmitExpression(method, *statement.arguments[0]) + "),__end=ToNumber(" + EmitExpression(method, *statement.arguments[1]) + "),__step=ToNumber(" + EmitExpression(method, *statement.arguments[2]) + ");");
                if (!hasVariable) Line(indent + 1, "Value __for_counter=MakeVar(T_DOUBLE);");
				Line(indent + 1, "if(__step!=0) for(double __i=__begin;__step>0?__i<=__end:__i>=__end;__i+=__step) {"); Line(indent + 2, "Assign(" + variable + ",Number(__i));");
				if (!EmitStatements(method, statement.body, indent + 2)) return false; Line(indent + 1, "}"); Line(indent, "}"); break;
			}
			case StatementKind::Break: Line(indent, "break;"); break;
			case StatementKind::Continue: Line(indent, "continue;"); break;
			case StatementKind::MachineCode: return Fail(method.sourceFile + ": machine_code_internal_error");
			}
			if (!error_->empty()) return false;
		}
		return true;
	}

	bool EmitX64MachineMethod(const Method& method, const Statement& machineStatement)
	{
		const auto isIntegerType = [](const TypeRef type) {
			return !type.isArray && (type.code == kTypeByte || type.code == kTypeShort ||
				type.code == kTypeInt || type.code == kTypeInt64 || type.code == kTypeBool);
		};
		for (const auto& parameter : method.parameters) {
			if (!isIntegerType(parameter.type)) {
				return Fail(method.sourceFile + ": x64_machine_code_parameter_type_not_supported:" + parameter.typeName);
			}
		}
		if (method.returnType.code != kTypeNull && !isIntegerType(method.returnType)) {
			return Fail(method.sourceFile + ": x64_machine_code_return_type_not_supported");
		}
		body_ << "\nstatic Value method_" << method.id << "(std::vector<Arg> a,Value* self) {\n";
		Line(1, "std::vector<Value> p; p.reserve(" + std::to_string(method.parameters.size()) + ");");
		for (std::size_t index = 0; index < method.parameters.size(); ++index) {
			const auto& parameter = method.parameters[index];
			Line(1, "p.push_back(MakeVar(" + Hex(parameter.type.code) + ",false));");
			Line(1, "if(a.size()>" + std::to_string(index) + ") Assign(p[" + std::to_string(index) + "],a[" + std::to_string(index) + "].Get());");
		}
		Line(1, "std::vector<Value> v; v.reserve(" + std::to_string(method.locals.size()) + ");");
		for (const auto& local : method.locals) {
			Line(1, "v.push_back(MakeVar(" + Hex(local.type.code) + "," + (local.type.isArray ? "true" : "false") + ")); ");
		}
		std::string writeback = "[&](){";
		for (std::size_t index = 0; index < method.parameters.size(); ++index) {
			if (method.parameters[index].byReference) {
				writeback += "if(a.size()>" + std::to_string(index) + " && a[" + std::to_string(index) + "].reference) Assign(*a[" + std::to_string(index) + "].reference,p[" + std::to_string(index) + "]);";
			}
		}
		writeback += "}";
		Line(1, "MethodValueScope __scope{&p,&v," + writeback + "};");
		body_ << "    static const unsigned char __machine_code[] = {";
		for (const std::uint8_t byte : machineStatement.machineCode) {
			body_ << static_cast<unsigned int>(byte) << ',';
		}
		body_ << "};\n";
		Line(1, "long long __machine_result=0;");
		Line(1, "if(!ExecuteX86MachineCode(__machine_code,sizeof(__machine_code),p," + std::string(method.returnType.code == kTypeInt64 ? "true" : "false") + ",__machine_result)){std::fputs(\"ecompiler: unsupported x86 machine code for x64\\r\\n\",stderr);OutputDebugStringA(\"ecompiler: unsupported x86 machine code for x64\\r\\n\");ExitProcess(87);}");
		if (method.returnType.code == kTypeNull) {
			Line(1, "return Empty();");
		}
		else if (method.returnType.code == kTypeInt64) {
			Line(1, "Value __machine_value=MakeVar(" + Hex(method.returnType.code) + "); __machine_value.integer=__machine_result; __machine_value.number=static_cast<double>(__machine_result); return __machine_value;");
		}
		else {
			Line(1, "return Integer(__machine_result," + Hex(method.returnType.code) + ");");
		}
		body_ << "}\n";
		return true;
	}

	bool EmitMethod(const Method& method)
	{
		SourceLine(method.sourceFile, method.sourceLine);
		const Statement* machineStatement = nullptr;
		for (const Statement& statement : method.body) {
			if (statement.kind == StatementKind::MachineCode) {
				if (machineStatement != nullptr) return Fail(method.sourceFile + ": multiple_machine_code_statements");
				machineStatement = &statement;
				continue;
			}
			if (machineStatement != nullptr && statement.kind == StatementKind::Return) continue;
			if (machineStatement != nullptr) return Fail(method.sourceFile + ": machine_code_must_be_only_method_statement");
		}
		if (machineStatement != nullptr) {
			if (program_.targetArchitecture == TargetArchitecture::X64) {
				return EmitX64MachineMethod(method, *machineStatement);
			}
			const std::string helper = "ecompiler_machine_" + std::to_string(method.id);
			const bool hasExplicitReturn = std::any_of(
				machineStatement->machineCode.begin(),
				machineStatement->machineCode.end(),
				[](const std::uint8_t byte) { return byte == 0xC2 || byte == 0xC3 || byte == 0xCA || byte == 0xCB; });
			// 不带 ret 的片段是 IDE 嵌入到正常子程序框架中的代码；
			// 该框架提供 EBP 参数槽及尾声。带 ret 的片段自行管理 ABI。
			const bool usesEbpFrame = !hasExplicitReturn ||
				std::find(machineStatement->machineCode.begin(), machineStatement->machineCode.end(), static_cast<std::uint8_t>(0xC9)) != machineStatement->machineCode.end() ||
				std::find(machineStatement->machineCode.begin(), machineStatement->machineCode.end(), static_cast<std::uint8_t>(0x55)) != machineStatement->machineCode.end();
			const bool returnsLong = method.returnType.code == kTypeInt64;
			const std::string helperReturn = returnsLong ? "long long" : "int";
			body_ << "\nextern \"C\" __declspec(naked) " << helperReturn << ' ' << (method.usesCdecl ? "__cdecl" : "__stdcall") << ' ' << helper << "(";
			for (std::size_t index = 0; index < method.parameters.size(); ++index) {
				if (index != 0) body_ << ',';
				body_ << "int arg" << index;
			}
			body_ << ") {\n    __asm {\n";
			if (usesEbpFrame) body_ << "        push ebp\n        mov ebp, esp\n";
			for (const std::uint8_t byte : machineStatement->machineCode) {
				std::ostringstream instruction;
				instruction << "        _emit 0x" << std::hex << std::uppercase << static_cast<unsigned int>(byte) << "\n";
				body_ << instruction.str();
			}
			if (!hasExplicitReturn) {
				if (usesEbpFrame) body_ << "        mov esp, ebp\n        pop ebp\n";
				if (method.usesCdecl || method.parameters.empty()) body_ << "        ret\n";
				else body_ << "        ret " << (method.parameters.size() * sizeof(std::uint32_t)) << "\n";
			}
			body_ << "    }\n}\n";
			body_ << "\nstatic Value method_" << method.id << "(std::vector<Arg> a,Value* self) {\n";
			Line(1, "std::vector<Value> p; p.reserve(" + std::to_string(method.parameters.size()) + ");");
			for (std::size_t index = 0; index < method.parameters.size(); ++index) {
				const auto& parameter = method.parameters[index];
				Line(1, "p.push_back(MakeVar(" + Hex(parameter.type.code) + "," + (parameter.type.isArray ? "true" : "false") + "));" );
				Line(1, "if(a.size()>" + std::to_string(index) + ") Assign(p.back(),a[" + std::to_string(index) + "].Get());");
			}
			std::string call = helper + "(";
			for (std::size_t index = 0; index < method.parameters.size(); ++index) {
				if (index != 0) call += ',';
				call += "static_cast<int>(ToInteger(p[" + std::to_string(index) + "]))";
			}
			call += ")";
			if (method.returnType.code == kTypeNull) Line(1, "(void)" + call + ";");
			else if (returnsLong) {
				Line(1, "long long __machine_result=" + call + ";");
				Line(1, "Value __machine_value=MakeVar(" + Hex(method.returnType.code) + "); __machine_value.integer=__machine_result; return __machine_value;");
			}
			else Line(1, "return Integer(" + call + "," + Hex(method.returnType.code) + ");");
			if (method.returnType.code == kTypeNull) Line(1, "return Empty();");
			body_ << "}\n";
			return true;
		}
		body_ << "\nstatic Value method_" << method.id << "(std::vector<Arg> a,Value* self) {\n";
		for (const auto& parameter : method.parameters) usedTypes_.insert(parameter.type.code);
		for (const auto& local : method.locals) usedTypes_.insert(local.type.code);
		Line(1, "std::vector<Value> p; p.reserve(" + std::to_string(method.parameters.size()) + ");");
		for (std::size_t index = 0; index < method.parameters.size(); ++index) {
			const auto& parameter = method.parameters[index]; Line(1, "p.push_back(MakeVar(" + Hex(parameter.type.code) + ',' + (parameter.type.isArray ? "true" : "false") + "));");
			if (parameter.type.isArray && !parameter.arrayDimensions.empty()) {
				std::string dimensions = "std::vector<int>{";
				for (const int dimension : parameter.arrayDimensions) dimensions += std::to_string(dimension) + ',';
				dimensions += "}";
				Line(1, "Redim(p.back()," + dimensions + ",false);");
			}
			Line(1, "if(a.size()>" + std::to_string(index) + ") Assign(p.back(),a[" + std::to_string(index) + "].Get());");
		}
		Line(1, "std::vector<Value> v; v.reserve(" + std::to_string(method.locals.size()) + ");");
		for (const auto& local : method.locals) {
			Line(1, "v.push_back(MakeVar(" + Hex(local.type.code) + ',' + (local.type.isArray ? "true" : "false") + ")); ");
			const TypeInfo* localType = program_.FindType(local.type.code);
			if (localType != nullptr && !local.type.isArray) {
				const auto initializer = std::find_if(localType->memberMethodIds.begin(), localType->memberMethodIds.end(), [&](const std::size_t methodId) {
					return methodId < program_.methods.size() && program_.methods[methodId].name == "_初始化";
				});
				if (initializer != localType->memberMethodIds.end()) {
					QueueMethod(*initializer);
					Line(1, "method_" + std::to_string(*initializer) + "({},&v[" + std::to_string(&local - method.locals.data()) + "]); ");
				}
			}
			if (local.type.isArray && !local.arrayDimensions.empty()) {
				std::string dimensions = "std::vector<int>{";
				for (const int dimension : local.arrayDimensions) dimensions += std::to_string(dimension) + ',';
				dimensions += "}";
				Line(1, "Redim(v.back()," + dimensions + ",false);");
			}
		}
		std::string writeback = "[&](){";
		for (std::size_t index = 0; index < method.parameters.size(); ++index) {
			if (method.parameters[index].byReference) {
				writeback += "if(a.size()>" + std::to_string(index) + " && a[" + std::to_string(index) + "].reference) Assign(*a[" + std::to_string(index) + "].reference,p[" + std::to_string(index) + "]);";
			}
		}
		writeback += "}";
		Line(1, "MethodValueScope __scope{&p,&v," + writeback + "};");
		if (!EmitStatements(method, method.body, 1)) return false;
		Line(1, "return Empty();"); body_ << "}\n"; return true;
	}

	std::string ExportCType(const TypeRef type, const bool byReference) const
	{
		if (type.isArray) return "void*";
		std::string base;
		switch (type.code) {
		case kTypeByte: base = "unsigned char"; break;
		case kTypeShort: base = "short"; break;
		case kTypeInt: case kTypeBool: base = "int"; break;
		case kTypeSubroutine: base = "void*"; break;
		case kTypeInt64: base = "long long"; break;
		case kTypeFloat: base = "float"; break;
		case kTypeDouble: case kTypeDateTime: base = "double"; break;
		case kTypeText: base = "char*"; break;
		case kTypeBinary: base = "unsigned char*"; break;
		default: base = "void*"; break;
		}
		if (type.code == kTypeText && !byReference) return "const char*";
		return byReference ? base + "*" : base;
	}

	void EmitExportWrapper(const Method& method)
	{
		const std::string symbol = "ecompiler_export_" + std::to_string(method.id);
			const std::string convention = method.usesCdecl ? "__cdecl" : "__stdcall";
		const std::string returnType = method.returnType.code == kTypeNull ? "void" : ExportCType(method.returnType, false);
		body_ << "\nextern \"C\" " << returnType << " " << convention << " " << symbol << "(";
		for (std::size_t index = 0; index < method.parameters.size(); ++index) {
			if (index != 0) body_ << ',';
			body_ << ExportCType(method.parameters[index].type, method.parameters[index].byReference) << " arg" << index;
		}
		body_ << ") {\n";
		body_ << "    std::vector<Arg> args; args.reserve(" << method.parameters.size() << ");\n";
		for (std::size_t index = 0; index < method.parameters.size(); ++index) {
			const Variable& parameter = method.parameters[index];
			body_ << "    Value value" << index << "=MakeVar(" << Hex(parameter.type.code) << ',' << (parameter.type.isArray ? "true" : "false") << ");\n";
				if (parameter.type.isArray) {
					const std::string arrayPointer = parameter.byReference ? "*arg" + std::to_string(index) : "arg" + std::to_string(index);
					body_ << "    if(" << arrayPointer << "!=nullptr) ReadArray(value" << index << "," << Hex(parameter.type.code) << ",static_cast<const unsigned char*>(" << arrayPointer << ")); else value" << index << ".declaredArray=true;\n";
			}
			else if (parameter.type.code == kTypeText) body_ << "    value" << index << ".text=" << (parameter.byReference ? "(arg" + std::to_string(index) + "==nullptr||*arg" + std::to_string(index) + "==nullptr)?std::string():std::string(*arg" + std::to_string(index) + ")" : "arg" + std::to_string(index) + "==nullptr?std::string():std::string(arg" + std::to_string(index) + ")") << ";\n";
			else if (parameter.type.code == kTypeBinary) body_ << "    if(arg" << index << "!=nullptr) ReadArray(value" << index << ",T_BYTE,static_cast<const unsigned char*>(arg" << index << "));\n";
			else if (parameter.type.code == kTypeFloat || parameter.type.code == kTypeDouble || parameter.type.code == kTypeDateTime) body_ << "    value" << index << ".number=arg" << index << ";\n";
			else if (program_.FindType(parameter.type.code) != nullptr) body_ << "    if(arg" << index << "!=nullptr) ReadObject(value" << index << ",arg" << index << ");\n";
			else if (parameter.byReference) body_ << "    value" << index << ".integer=arg" << index << "==nullptr?0:*arg" << index << ";\n";
			else body_ << "    value" << index << ".integer=arg" << index << ";\n";
			body_ << "    args.push_back(Arg::Ref(value" << index << "));\n";
		}
		body_ << "    Value result=method_" << method.id << "(std::move(args),nullptr);\n";
		for (std::size_t index = 0; index < method.parameters.size(); ++index) {
			const Variable& parameter = method.parameters[index];
			if (!parameter.byReference) continue;
			if (parameter.type.code == kTypeText) body_ << "    if(arg" << index << "!=nullptr){if(*arg" << index << ") RuntimeFree(*arg" << index << ");*arg" << index << "=RuntimeText(value" << index << ".text);}\n";
			else if (parameter.type.code == kTypeBinary) body_ << "    if(arg" << index << "!=nullptr)*arg" << index << "=RuntimeBinary(value" << index << ".bytes);\n";
			else if (parameter.type.code == kTypeFloat || parameter.type.code == kTypeDouble || parameter.type.code == kTypeDateTime) body_ << "    if(arg" << index << "!=nullptr) *arg" << index << "=value" << index << ".number;\n";
			else if (program_.FindType(parameter.type.code) != nullptr) body_ << "    if(arg" << index << "!=nullptr) PrepareObject(value" << index << "),std::memcpy(arg" << index << ",value" << index << ".object.data(),FindType(value" << index << ".type)->size);\n";
			else body_ << "    if(arg" << index << "!=nullptr) *arg" << index << "=static_cast<" << ExportCType(parameter.type, false) << ">(value" << index << ".integer);\n";
		}
		if (method.returnType.code == kTypeNull) body_ << "    return;\n";
		else if (method.returnType.code == kTypeText) body_ << "    return RuntimeText(result.text);\n";
		else if (method.returnType.code == kTypeBinary) body_ << "    return RuntimeBinary(result.bytes);\n";
		else if (method.returnType.code == kTypeFloat || method.returnType.code == kTypeDouble || method.returnType.code == kTypeDateTime) body_ << "    return static_cast<" << returnType << ">(result.number);\n";
		else if (program_.FindType(method.returnType.code) != nullptr) body_ << "    return result.object.empty()?nullptr:result.object.data();\n";
		else if (method.returnType.code == kTypeSubroutine) body_ << "    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(result.integer));\n";
		else body_ << "    return static_cast<" << returnType << ">(result.integer);\n";
		body_ << "}\n";
		exports_.push_back({ method.exportName.empty() ? method.name : method.exportName, symbol, method.usesCdecl, AbiParameterBytes(method.parameters) });
	}

	void EmitWindowRuntime(std::ostringstream& prefix)
	{
		if (program_.windows.empty()) return;
		prefix << R"CPP(
namespace ecompiler_window_host {
struct Unit {
    unsigned int id=0; unsigned int form=0; HWND hwnd=nullptr; HWND parent=nullptr; int tabOwner=0; int tabPage=-1; const char* type=nullptr;
    std::wstring tag;
    // 选择列表框的勾选状态不等同于 Win32 LISTBOX 的焦点/选择状态。
    std::vector<unsigned char> checkStates;
    std::vector<unsigned char> checkEnabled;
    bool checkOnlyOne=false;
    bool hasTextColor=false;
    bool hasBackColor=false;
    COLORREF textColor=RGB(0,0,0);
    COLORREF backColor=RGB(255,255,255);
    HBRUSH backBrush=nullptr;
    int spinDirection=0;
    int writeX=0;
    int writeY=0;
    int drawUnit=0;
    COLORREF color=RGB(0,0,0);
    bool hasColor=false;
    bool allowTransparent=false;
    std::wstring path;
    std::wstring filePattern;
    std::wstring hyperlinkTarget;
    int hyperlinkType=0;
    int driveType=0;
    bool allowNormal=true;
    bool allowArchive=true;
    bool allowReadOnly=true;
    bool allowSystem=true;
    bool allowHidden=true;
    bool fileAllowMultiple=false;
    bool hasMinDate=false;
    bool hasMaxDate=false;
    SYSTEMTIME minDate{};
    SYSTEMTIME maxDate{};
    int scrollMin=0;
    int scrollMax=100;
    int scrollPage=10;
    int scrollLine=1;
    bool allowTrack=true;
    bool dateAllowEdit=true;
    int tabHeaderWay=0;
    bool tabMultiLine=false;
    bool tabFillBack=true;
    COLORREF tabBackColor=RGB(255,255,255);
    int progressDrawMode=0;
    int trackTickStyle=0;
    int trackTickFreq=0;
    bool trackAllowSel=false;
    bool removeDuplicates=false;
    COLORREF monthColors[6]{};
    unsigned char monthColorSet[6]{};
    UINT timerPeriod=0;
    int dialogType=0;
    std::wstring dialogFileName;
    std::wstring dialogFilter;
    std::wstring dialogInitialDir;
    std::wstring dialogDefExt;
     std::wstring dialogCaption;
     bool hideSelection=false;
     int inputMode=0;
     int convertMode=0;
     int spinMode=0;
     int spinMin=0;
     int spinMax=100;
     int borderStyle=1;
     int horizontalAlign=0;
     int verticalAlign=0;
     int shape=0;
     int shapeEffect=0;
     int lineStyle=1;
     int lineWidth=1;
     COLORREF lineColor=RGB(0,0,0);
     COLORREF fillColor=RGB(255,255,255);
     bool hasLineColor=false;
     bool hasFillColor=false;
     int penStyle=1;
     int drawRop2=R2_COPYPEN;
     int penWidth=0;
     COLORREF penColor=RGB(0,0,0);
     COLORREF brushColor=RGB(255,255,255);
     COLORREF textBackColor=RGB(255,255,255);
     int brushStyle=0;
     bool autoRedraw=false;
     int backPicMode=0;
     bool enterToNext=false;
     bool f1OpenHelp=false;
     std::wstring helpFileName;
     int helpContext=0;
     bool hitMove=false;
     bool topmost=false;
     int imageDrawMode=0;
     bool playImage=true;
     std::wstring imageFileName;
     bool imageCenter=false;
     bool imageTransparent=false;
     int imagePlayCount=0;
    bool visited=false;
    bool hyperlinkHot=false;
    bool hyperlinkTrack=true;
    COLORREF visitedColor=RGB(128,0,128);
    COLORREF hotColor=RGB(0,0,255);
    std::wstring passwordChar=L"*";
    HFONT font=nullptr;
    std::wstring fontName=L"SimSun";
    int fontSize=9;
    bool fontBold=false;
    bool fontItalic=false;
    bool fontStrikeOut=false;
    bool fontUnderline=false;
    int labelEffect=0;
    int gradientBorderWidth=0;
    COLORREF gradientBorderColor[3]{};
    unsigned char gradientBorderColorSet[3]{};
    int gradientBackMode=0;
    COLORREF gradientBackColor[3]{};
    unsigned char gradientBackColorSet[3]{};
    bool labelWordWrap=false;
    bool labelUnderline=false;
    std::vector<unsigned char> imageData;
    bool dialogCreatePrompt=false;
    bool dialogFileMustExist=true;
    bool dialogOverwritePrompt=true;
    bool dialogPathMustExist=true;
    bool dialogNoChangeDir=false;
    int dialogFilterIndex=0;
    COLORREF dialogFontColor=RGB(0,0,0);
    bool dialogFontBold=false;
    bool dialogFontItalic=false;
    bool dialogFontStrikeOut=false;
    bool dialogFontUnderline=false;
    std::wstring dialogFontName;
    int dialogFontSize=0;
    int dialogHelpCommand=0;
    int dialogHelpContext=0;
    std::unordered_map<std::string,Value> properties;
    std::vector<unsigned char> cursorData;
    HCURSOR cursor=nullptr;
};
struct Form {
    unsigned int id=0; HWND hwnd=nullptr; bool escapeCloses=false; bool canMove=true; bool firstActivated=false;
    bool enterToNext=false; bool f1OpenHelp=false; bool hitMove=false; bool topmost=false;
    bool showInTaskbar=true; bool keepTitleBarActive=false; int border=2; int position=0;
    bool controlButtons=true; bool maximizeButton=true; bool minimizeButton=true; int shape=0; int backPicMode=0; int playCount=2;
    DWORD idleSince=0;
    bool hasBackColor=false; COLORREF backColor=RGB(255,255,255);
    std::wstring helpFileName; int helpContext=0; std::wstring backPicFile; std::vector<unsigned char> backPicData; HICON icon=nullptr;
    NOTIFYICONDATAW tray{}; HICON trayIcon=nullptr; bool trayRegistered=false;
    std::unordered_map<std::string,Value> properties;
};
struct XmlAttribute { const char* name; const char* value; };
struct Spec { unsigned int id; unsigned int form; unsigned int parent; int left; int top; int width; int height; bool visible; bool disabled; bool tabStop; int tabOwner; int tabPage; int tabPageCount; int tabCurrentPage; const char* type; const char* text; const XmlAttribute* attributes; std::size_t attributeCount; const char* const* items; std::size_t itemCount; const int* itemValues; std::size_t itemValueCount; const unsigned char* itemChecked; std::size_t itemCheckedCount; const unsigned char* itemEnabled; std::size_t itemEnabledCount; };
static std::vector<Unit> units;
static std::vector<Form> forms;
static std::unordered_set<HWND> mouseInside;
static HWND lockedWindow=nullptr;
static HWND activePaintWindow=nullptr;
static HDC activePaintDC=nullptr;
static HINSTANCE instance=GetModuleHandleW(nullptr);
static HFONT defaultFont=nullptr;
static bool initializing=false;
static constexpr UINT trayMessage=WM_APP+0x5E;
static HCURSOR CursorFromBytes(const std::vector<unsigned char>& bytes) {
    if(bytes.empty())return nullptr;
    HICON icon=CreateIconFromResourceEx(const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes.data())),static_cast<DWORD>(bytes.size()),TRUE,0x00030000,0,0,LR_DEFAULTSIZE);
    return icon==nullptr?nullptr:static_cast<HCURSOR>(icon);
}
static void ReplaceUnitCursor(Unit& unit,const std::vector<unsigned char>& bytes) {
    HCURSOR cursor=CursorFromBytes(bytes);
    if(!bytes.empty()&&cursor==nullptr)return;
    if(unit.cursor!=nullptr)DestroyCursor(unit.cursor);
    unit.cursor=cursor;unit.cursorData=bytes;
}
static std::wstring Wide(const char* value) {
    if(value==nullptr||*value==0)return {};
    const int length=MultiByteToWideChar(CP_ACP,0,value,-1,nullptr,0);
    if(length<=1)return {};
    std::wstring result(static_cast<std::size_t>(length),L'\0');
    MultiByteToWideChar(CP_ACP,0,value,-1,result.data(),length);
    result.resize(static_cast<std::size_t>(length-1));
    return result;
}
static HFONT DefaultFont() {
    if(defaultFont!=nullptr)return defaultFont;
    // 易语言核心控件的空“字体”属性使用宋体 9 磅；固定这个默认值，
    // 避免宿主系统的现代 UI 字体把控件度量和 IDE 拉开差异。
    defaultFont=CreateFontW(-12,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"SimSun");
    if(defaultFont==nullptr)defaultFont=static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    return defaultFont;
}
static void ApplyDefaultFont(HWND window) {
    if(window!=nullptr)SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(DefaultFont()),TRUE);
}
static void TrackMouse(HWND window) {
    if(window==nullptr)return;
    TRACKMOUSEEVENT event{sizeof(TRACKMOUSEEVENT),TME_LEAVE,window,0};
    TrackMouseEvent(&event);
}
static bool BeginMouseTracking(HWND window) {
    if(window==nullptr)return false;
    const bool firstEntry=mouseInside.insert(window).second;
    TrackMouse(window);
    return firstEntry;
}
static void EndMouseTracking(HWND window) {
    if(window!=nullptr)mouseInside.erase(window);
}
static DWORD FormStyle(int border,bool controlButtons,bool maxButton,bool minButton) {
    DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_BORDER;
    switch(border) {
    case 0:style=WS_POPUP;break;
    case 1:case 3:style=WS_OVERLAPPED|WS_CAPTION|WS_THICKFRAME;break;
    case 2:case 4:style=WS_OVERLAPPED|WS_CAPTION|WS_BORDER;break;
    case 5:style=WS_OVERLAPPED|WS_THICKFRAME;break;
    case 6:style=WS_OVERLAPPED|WS_BORDER;break;
    default:style=WS_OVERLAPPEDWINDOW;break;
    }
    if(controlButtons)style|=WS_SYSMENU;
    if(maxButton)style|=WS_MAXIMIZEBOX;
    if(minButton)style|=WS_MINIMIZEBOX;
    return style;
}
static DWORD FormExtendedStyle(bool showInTaskbar) {
    return showInTaskbar?0:WS_EX_TOOLWINDOW;
}
static void PlaceForm(HWND window,int left,int top,int position) {
    if(window==nullptr)return;
    if(position==1) {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        RECT current{};GetWindowRect(window,&current);
        left=work.left+(work.right-work.left-(current.right-current.left))/2;
        top=work.top+(work.bottom-work.top-(current.bottom-current.top))/2;
    }
    SetWindowPos(window,nullptr,left,top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
}
static Unit* FindUnit(unsigned int id) { for(auto& item:units)if(item.id==id)return &item; return nullptr; }
static HWND FindForm(unsigned int id) { for(const auto& item:forms)if(item.id==id)return item.hwnd; return nullptr; }
enum NativeEventKind : unsigned int {
    native_unknown=0,
    native_created=1,
    native_closing=2,
    native_destroyed=3,
    native_size_changed=4,
    native_moved=5,
    native_activated=6,
    native_deactivated=7,
    native_focus_gained=8,
    native_focus_lost=9,
    native_clicked=10,
    native_double_clicked=11,
    native_drop_down=12,
    native_selection_changed=13,
    native_position_changed=14,
    native_changed=15,
    native_key_down=16,
    native_key_up=17,
    native_mouse_down=18,
    native_mouse_up=19,
    native_mouse_move=20,
    native_paint=21,
    native_timer=22,
    native_list_closed=23,
    native_selection_changing=24,
    native_char_input=25,
    native_right_mouse_down=26,
    native_right_mouse_up=27,
    native_first_activated=28,
    native_shown=29,
    native_hidden=30,
    native_idle=31,
    native_tray=32,
    native_mouse_enter=33,
    native_mouse_leave=34,
    native_check_changed=35,
    native_feedback=36,
};
static Value InvokeEvent(unsigned int methodId,std::vector<Value> values) {
    switch(methodId) {
)CPP";
		std::unordered_set<std::size_t> eventMethods;
		for (const auto& form : program_.windows) {
			for (const auto& event : form.events) {
				if (!eventMethods.insert(event.methodId).second) continue;
				prefix << "    case " << event.methodId << "u:{std::vector<Arg> callArgs;";
				if (event.methodId < program_.methods.size()) {
					const auto& method = program_.methods[event.methodId];
					for (std::size_t index = 0; index < method.parameters.size(); ++index) {
						prefix << "if(values.size()>" << index << ")callArgs.push_back(" <<
							(method.parameters[index].byReference ? "Arg::Ref(values[" : "Arg::Temp(values[") << index << "]));";
					}
				}
				prefix << "return method_" << event.methodId << "(std::move(callArgs),nullptr);}\n";
			}
			for (const auto& control : form.controls) {
				if (!HasNativeWin32Class(control.typeName)) continue;
				for (const auto& event : control.events) {
					if (!eventMethods.insert(event.methodId).second) continue;
					prefix << "    case " << event.methodId << "u:{std::vector<Arg> callArgs;";
					if (event.methodId < program_.methods.size()) {
						const auto& method = program_.methods[event.methodId];
						for (std::size_t index = 0; index < method.parameters.size(); ++index) {
							prefix << "if(values.size()>" << index << ")callArgs.push_back(" <<
								(method.parameters[index].byReference ? "Arg::Ref(values[" : "Arg::Temp(values[") << index << "]));";
						}
					}
					prefix << "return method_" << event.methodId << "(std::move(callArgs),nullptr);}\n";
				}
			}
		}
		prefix << R"CPP(
    default:return Empty();
    }
}
static bool Dispatch(unsigned int unit,unsigned int trigger,int nativeCode,std::vector<Value> values,Value* result=nullptr) {
    bool handled=false;
)CPP";
		for (const auto& form : program_.windows) {
			for (const auto& event : form.events) {
				const unsigned int triggerCode = static_cast<unsigned int>(event.trigger);
				prefix << "    if(unit==" << form.id << "u&&((trigger==" << triggerCode << "u&&" << triggerCode << "u!=0u)||(trigger==0u&&nativeCode==" << event.index << "))){if(result!=nullptr)*result=InvokeEvent(" << event.methodId << "u,values);else (void)InvokeEvent(" << event.methodId << "u,values);handled=true;}\n";
			}
			for (const auto& control : form.controls) {
				if (!HasNativeWin32Class(control.typeName)) continue;
				for (const auto& event : control.events) {
					const unsigned int triggerCode = static_cast<unsigned int>(event.trigger);
					prefix << "    if(unit==" << control.id << "u&&((trigger==" << triggerCode << "u&&" << triggerCode << "u!=0u)||(trigger==0u&&nativeCode==" << event.index << "))){if(result!=nullptr)*result=InvokeEvent(" << event.methodId << "u,values);else (void)InvokeEvent(" << event.methodId << "u,values);handled=true;}\n";
				}
			}
		}
        prefix << R"CPP(
    return handled;
}
static unsigned int UnitIdFromWindow(HWND window) {
    return window==nullptr?0u:static_cast<unsigned int>(GetDlgCtrlID(window));
}
static Unit* UnitFromWindow(HWND window);
static unsigned int CommandEventKind(HWND source,unsigned int notification) {
    const Unit* unit=UnitFromWindow(source);
    const bool pathSelection=unit!=nullptr&&unit->type!=nullptr&&
        (std::strcmp(unit->type,"file")==0||std::strcmp(unit->type,"directory")==0||std::strcmp(unit->type,"drive")==0);
    wchar_t className[32]{};
    GetClassNameW(source,className,static_cast<int>(std::size(className)));
    if(lstrcmpiW(className,L"LISTBOX")==0) {
        switch(notification) {
        case LBN_SELCHANGE:return pathSelection?native_changed:native_selection_changed;
        case LBN_DBLCLK:return native_double_clicked;
        default:return native_unknown;
        }
    }
    if(lstrcmpiW(className,L"COMBOBOX")==0) {
        switch(notification) {
        case CBN_DROPDOWN:return native_drop_down;
        case CBN_CLOSEUP:return native_list_closed;
        case CBN_SELCHANGE:case CBN_SELENDOK:return pathSelection?native_changed:native_selection_changed;
        case CBN_DBLCLK:return native_double_clicked;
        case CBN_EDITCHANGE:return native_changed;
        default:return native_unknown;
        }
    }
    if(lstrcmpiW(className,L"EDIT")==0) {
        switch(notification) {
        case EN_CHANGE:return native_changed;
        case EN_SETFOCUS:return native_focus_gained;
        case EN_KILLFOCUS:return native_focus_lost;
        default:return native_unknown;
        }
    }
    if(lstrcmpiW(className,L"BUTTON")==0 && notification==BN_CLICKED)return native_clicked;
    if(lstrcmpiW(className,L"STATIC")==0) {
        if(notification==STN_CLICKED)return native_clicked;
        if(notification==STN_DBLCLK)return native_double_clicked;
    }
    return native_unknown;
}
static bool DispatchNative(HWND source,unsigned int trigger,int nativeCode,std::vector<Value> values={}) {
    if(initializing||source==nullptr)return false;
    const unsigned int unit=UnitIdFromWindow(source);
    return unit!=0&&Dispatch(unit,trigger,nativeCode,std::move(values));
}
static bool DispatchNativeResult(HWND source,unsigned int trigger,int nativeCode,std::vector<Value> values,Value* result) {
    if(initializing||source==nullptr)return false;
    const unsigned int unit=UnitIdFromWindow(source);
    return unit!=0&&Dispatch(unit,trigger,nativeCode,std::move(values),result);
}
static Unit* UnitFromWindow(HWND window) {
    for(auto& unit:units)if(unit.hwnd==window)return &unit;
    return nullptr;
}
static bool IsCheckList(HWND window) {
    const Unit* unit=UnitFromWindow(window);
    return unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"checklist")==0;
}
static void EnsureCheckState(Unit& unit,std::size_t count) {
    if(unit.checkStates.size()<count)unit.checkStates.resize(count,0);
    else if(unit.checkStates.size()>count)unit.checkStates.resize(count);
    if(unit.checkEnabled.size()<count)unit.checkEnabled.resize(count,1);
    else if(unit.checkEnabled.size()>count)unit.checkEnabled.resize(count);
}
static bool SetCheckState(HWND window,int index,bool state,bool notify=true) {
    Unit* unit=UnitFromWindow(window);
    if(unit==nullptr||unit->type==nullptr||std::strcmp(unit->type,"checklist")!=0||index<0)return false;
    const int count=static_cast<int>(SendMessageW(window,LB_GETCOUNT,0,0));
    if(index>=count)return false;
    EnsureCheckState(*unit,static_cast<std::size_t>(count));
    if(!unit->checkEnabled[static_cast<std::size_t>(index)])return false;
    if(unit->checkOnlyOne&&state)for(std::size_t item=0;item<unit->checkStates.size();++item)unit->checkStates[item]=item==static_cast<std::size_t>(index)?1:0;
    else unit->checkStates[static_cast<std::size_t>(index)]=state?1:0;
    InvalidateRect(window,nullptr,TRUE);
    if(notify&&!initializing)DispatchNative(window,native_check_changed,0);
    return true;
}
static int CheckItemAtPoint(HWND window,LPARAM lParam) {
    POINT point{static_cast<short>(LOWORD(lParam)),static_cast<short>(HIWORD(lParam))};
    const LRESULT result=SendMessageW(window,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));
    return HIWORD(result)!=0? -1 : static_cast<int>(LOWORD(result));
}
static int CheckItemFromKey(HWND window) {
    const LRESULT caret=SendMessageW(window,LB_GETCARETINDEX,0,0);
    return caret<0||caret>static_cast<LRESULT>((std::numeric_limits<int>::max)())?-1:static_cast<int>(caret);
}
static HBRUSH UnitBackBrush(Unit& unit) {
    if(unit.backBrush==nullptr)unit.backBrush=CreateSolidBrush(unit.backColor);
    return unit.backBrush;
}
static bool IsDirectChild(HWND parent,HWND child);
static bool ClassEquals(HWND window,const wchar_t* expected);
static LRESULT RouteControlColor(HWND parent,UINT message,WPARAM wParam,LPARAM lParam) {
    HWND source=reinterpret_cast<HWND>(lParam);
    HWND target=source;
    if(!IsDirectChild(parent,source)) {
        HWND owner=GetParent(source);
        if(owner==nullptr||!IsDirectChild(parent,owner)||!ClassEquals(owner,L"COMBOBOX"))return (std::numeric_limits<LRESULT>::min)();
        target=owner;
    }
    Unit* unit=UnitFromWindow(target);
    if(unit==nullptr||(!unit->hasTextColor&&!unit->hasBackColor))return (std::numeric_limits<LRESULT>::min)();
    HDC dc=reinterpret_cast<HDC>(wParam);
    if(unit->hasTextColor)SetTextColor(dc,unit->textColor);
    if(unit->hasBackColor){SetBkColor(dc,unit->backColor);return reinterpret_cast<LRESULT>(UnitBackBrush(*unit));}
    SetBkMode(dc,TRANSPARENT);
    return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
}
static bool DrawCheckListItem(const DRAWITEMSTRUCT& item) {
    if(item.CtlType!=ODT_LISTBOX||item.hwndItem==nullptr||!IsCheckList(item.hwndItem))return false;
    Unit* unit=UnitFromWindow(item.hwndItem);
    if(unit==nullptr)return false;
    EnsureCheckState(*unit,static_cast<std::size_t>(SendMessageW(item.hwndItem,LB_GETCOUNT,0,0)));
    const bool selected=(item.itemState&ODS_SELECTED)!=0;
    const bool enabled= item.itemID<unit->checkEnabled.size() && unit->checkEnabled[item.itemID]!=0;
    const bool checked= item.itemID<unit->checkStates.size() && unit->checkStates[item.itemID]!=0;
    FillRect(item.hDC,&item.rcItem,GetSysColorBrush(selected?COLOR_HIGHLIGHT:COLOR_WINDOW));
    SetBkMode(item.hDC,TRANSPARENT);
    SetTextColor(item.hDC,enabled?(selected?GetSysColor(COLOR_HIGHLIGHTTEXT):GetSysColor(COLOR_WINDOWTEXT)):GetSysColor(COLOR_GRAYTEXT));
    RECT checkRect=item.rcItem; checkRect.left+=2; checkRect.top+=(item.rcItem.bottom-item.rcItem.top-14)/2; checkRect.right=checkRect.left+14; checkRect.bottom=checkRect.top+14;
    UINT state=DFCS_BUTTONCHECK|(checked?DFCS_CHECKED:0)|(enabled?0:DFCS_INACTIVE);
    DrawFrameControl(item.hDC,&checkRect,DFC_BUTTON,state);
    wchar_t text[4096]{};
    if(item.itemID!=static_cast<UINT>(-1))SendMessageW(item.hwndItem,LB_GETTEXT,item.itemID,reinterpret_cast<LPARAM>(text));
    RECT textRect=item.rcItem; textRect.left=checkRect.right+4;
    DrawTextW(item.hDC,text,-1,&textRect,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
    if(item.itemState&ODS_FOCUS)DrawFocusRect(item.hDC,&item.rcItem);
    return true;
}
static bool IsDirectChild(HWND parent,HWND child) {
    return parent!=nullptr&&child!=nullptr&&GetParent(child)==parent;
}
static void UpdateTabVisibility(unsigned int tabId);
static bool PaintUnitSurface(HWND window,HDC dc);
static LRESULT RouteChildNotification(HWND parent,UINT message,WPARAM wParam,LPARAM lParam) {
    constexpr LRESULT not_routed=(std::numeric_limits<LRESULT>::min)();
    if(message==WM_MEASUREITEM) {
        const auto* measure=reinterpret_cast<const MEASUREITEMSTRUCT*>(lParam);
        if(measure==nullptr||measure->CtlType!=ODT_LISTBOX)return not_routed;
        HWND source=GetDlgItem(parent,measure->CtlID);
        if(source==nullptr||!IsCheckList(source))return not_routed;
        auto* mutableMeasure=const_cast<MEASUREITEMSTRUCT*>(measure);
        mutableMeasure->itemHeight=20;
        return 0;
    }
    if(message==WM_DRAWITEM) {
        const auto* draw=reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if(draw!=nullptr&&DrawCheckListItem(*draw))return 0;
        return not_routed;
    }
    if(message==WM_CTLCOLORLISTBOX||message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC||
       message==WM_CTLCOLORBTN||message==WM_CTLCOLORSCROLLBAR) {
        const LRESULT color=RouteControlColor(parent,message,wParam,lParam);
        if(color!=not_routed)return color;
        return not_routed;
    }
    if(message==WM_COMMAND) {
        HWND source=reinterpret_cast<HWND>(lParam);
        HWND target=source;
        if(!IsDirectChild(parent,source)) {
            HWND owner=GetParent(source);
            if(owner==nullptr||!IsDirectChild(parent,owner)||!ClassEquals(owner,L"COMBOBOX"))return not_routed;
            target=owner;
        }
        const unsigned int notification=HIWORD(wParam);
        if(notification==BN_CLICKED) {
            if(Unit* unit=UnitFromWindow(target);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"color")==0) {
                COLORREF customColors[16]{};
                // lCustData is LPARAM (an integer-sized field), not a pointer.
                // Use zero explicitly so generated x86/x64 code is accepted by MSVC.
                CHOOSECOLORW chooser{sizeof(CHOOSECOLORW),parent,nullptr,unit->hasColor?unit->color:RGB(0,0,0),customColors,CC_FULLOPEN|CC_RGBINIT,0,nullptr,nullptr};
                if(ChooseColorW(&chooser)!=FALSE) { unit->color=chooser.rgbResult;unit->hasColor=true;InvalidateRect(target,nullptr,TRUE);DispatchNative(target,native_changed,0); }
                return 0;
            }
        }
        // A combo box sends notifications from its child EDIT control for
        // editable text changes.  The public events belong to the combo box,
        // so classify using the logical target rather than the nested source.
        const unsigned int eventKind=CommandEventKind(target,notification);
        DispatchNative(target,eventKind,static_cast<int>(notification));
        // BUTTON controls used as check/radio boxes expose two independent
        // EasyLanguage events: the click and the checked-state transition.
        // Win32 reports both through BN_CLICKED, so synthesize the latter
        // after the click without changing ordinary push-button behavior.
        if(notification==BN_CLICKED) {
            if(const Unit* unit=UnitFromWindow(target);unit!=nullptr&&unit->type!=nullptr&&
               (std::strcmp(unit->type,"checkbox")==0||std::strcmp(unit->type,"radio")==0))
                DispatchNative(target,native_check_changed,static_cast<int>(notification),
                    {Boolean(SendMessageW(target,BM_GETCHECK,0,0)==BST_CHECKED)});
        }
        return 0;
    }
    if(message==WM_HSCROLL||message==WM_VSCROLL) {
        HWND source=reinterpret_cast<HWND>(lParam);
        if(!IsDirectChild(parent,source))return not_routed;
        const Unit* unit=UnitFromWindow(source);
        const int code=static_cast<int>(LOWORD(wParam));
        if(unit==nullptr||unit->allowTrack||code!=SB_THUMBTRACK)DispatchNative(source,native_position_changed,code);
        return 0;
    }
    if(message==WM_NOTIFY) {
        const auto* header=reinterpret_cast<const NMHDR*>(lParam);
        if(header==nullptr||!IsDirectChild(parent,header->hwndFrom))return not_routed;
        if(header->code==UDN_DELTAPOS) {
            const auto* delta=reinterpret_cast<const NMUPDOWN*>(lParam);
            // NMUPDOWN uses a negative delta for the up arrow and a positive
            // delta for the down arrow.  The EasyLanguage event exposes the
            // corresponding button value as +1 / -1.
            const int button=delta==nullptr?0:(delta->iDelta<0?1:-1);
            // 编辑框调节器的通知来自其伴随 UPDOWN 窗口，事件归属仍是
            // 编辑框；独立“调节器”则没有 buddy，继续派发给自身。
            HWND target=header->hwndFrom;
            if(ClassEquals(header->hwndFrom,UPDOWN_CLASSW)) {
                if(HWND buddy=reinterpret_cast<HWND>(SendMessageW(header->hwndFrom,UDM_GETBUDDY,0,0));buddy!=nullptr)
                    target=buddy;
            }
            // 编辑框的“自动调节器”只修改数值；只有“手动调节器”
            // 才暴露“调节钮被按下”事件参数。
            if(const Unit* owner=UnitFromWindow(target);owner!=nullptr&&owner->type!=nullptr&&
               std::strcmp(owner->type,"edit")==0&&owner->spinMode!=2)return 0;
            DispatchNative(target,native_position_changed,static_cast<int>(header->code),{Integer(button)});
            return 0;
        }
        const unsigned int trigger=header->code==TCN_SELCHANGING?native_selection_changing:
            (header->code==TCN_SELCHANGE?native_selection_changed:
            (header->code==DTN_DATETIMECHANGE?native_changed:
            (header->code==MCN_SELCHANGE?native_changed:
            (header->code==NM_DBLCLK?native_double_clicked:
            (header->code==NM_CLICK?native_clicked:native_unknown)))));
        if(trigger==native_selection_changed)UpdateTabVisibility(UnitIdFromWindow(header->hwndFrom));
        if(trigger==native_selection_changing) {
            Value result;
            const bool invoked=DispatchNativeResult(header->hwndFrom,trigger,static_cast<int>(header->code),{},&result);
            return invoked&&result.type==T_BOOL&&!ToBool(result)?1:0;
        }
        DispatchNative(header->hwndFrom,trigger,static_cast<int>(header->code));
        return 0;
    }
    return not_routed;
}
static void RouteUnitInput(HWND source,UINT message,WPARAM wParam,LPARAM lParam) {
    switch(message) {
    case WM_LBUTTONDBLCLK:{wchar_t className[32]{};GetClassNameW(source,className,static_cast<int>(std::size(className)));if(lstrcmpiW(className,L"LISTBOX")!=0&&lstrcmpiW(className,L"COMBOBOX")!=0&&lstrcmpiW(className,L"SysTabControl32")!=0)DispatchNative(source,native_double_clicked,-1,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;}
    case WM_LBUTTONDOWN:DispatchNative(source,native_mouse_down,0,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_LBUTTONUP:if(Unit* unit=UnitFromWindow(source);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"hyperlink")==0){unit->visited=true;InvalidateRect(source,nullptr,TRUE);}DispatchNative(source,native_mouse_up,0,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_RBUTTONDBLCLK:DispatchNative(source,native_double_clicked,-1,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_RBUTTONDOWN:DispatchNative(source,native_right_mouse_down,0,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_RBUTTONUP:DispatchNative(source,native_right_mouse_up,0,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_MOUSEMOVE:if(BeginMouseTracking(source)){if(Unit* unit=UnitFromWindow(source);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"hyperlink")==0&&unit->hyperlinkTrack){unit->hyperlinkHot=true;InvalidateRect(source,nullptr,TRUE);}DispatchNative(source,native_mouse_enter,0);}DispatchNative(source,native_mouse_move,0,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_MOUSELEAVE:EndMouseTracking(source);if(Unit* unit=UnitFromWindow(source);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"hyperlink")==0){unit->hyperlinkHot=false;InvalidateRect(source,nullptr,TRUE);}DispatchNative(source,native_mouse_leave,0);break;
    case WM_MOVE:DispatchNative(source,native_moved,0,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam)))});break;
    case WM_KEYDOWN:case WM_SYSKEYDOWN:DispatchNative(source,native_key_down,static_cast<int>(wParam),{Integer(static_cast<unsigned int>(wParam)),Integer(static_cast<unsigned int>(lParam))});break;
    case WM_KEYUP:case WM_SYSKEYUP:DispatchNative(source,native_key_up,static_cast<int>(wParam),{Integer(static_cast<unsigned int>(wParam)),Integer(static_cast<unsigned int>(lParam))});break;
    case WM_CHAR:case WM_SYSCHAR:DispatchNative(source,native_char_input,static_cast<int>(wParam),{Integer(static_cast<unsigned int>(wParam))});break;
    case WM_SETFOCUS:DispatchNative(source,native_focus_gained,0);break;
    case WM_KILLFOCUS:DispatchNative(source,native_focus_lost,0);break;
    case WM_SHOWWINDOW:DispatchNative(source,wParam?native_shown:native_hidden,0);break;
    case WM_SIZE:DispatchNative(source,native_size_changed,0,{Integer(static_cast<unsigned int>(LOWORD(lParam))),Integer(static_cast<unsigned int>(HIWORD(lParam)))});break;
    case WM_TIMER:DispatchNative(source,native_timer,static_cast<int>(wParam));break;
    default:break;
    }
}
static Form* FindFormRecord(HWND window) {
    for(auto& form:forms)if(form.hwnd==window)return &form;
    return nullptr;
}
static Form* OwnerForm(HWND window) {
    if(Form* form=FindFormRecord(window))return form;
    for(auto& form:forms)if(window!=nullptr&&IsChild(form.hwnd,window))return &form;
    return nullptr;
}
static void RemoveTrayIcon(Form& form) {
    if(form.trayRegistered){Shell_NotifyIconW(NIM_DELETE,&form.tray);form.trayRegistered=false;}
    if(form.trayIcon!=nullptr){DestroyIcon(form.trayIcon);form.trayIcon=nullptr;}
    form.tray={};
}
static bool SetTrayIcon(Form& form,const Value* value,const std::wstring& tip) {
    RemoveTrayIcon(form);
    if(value==nullptr||value->missing||value->text.empty())return true;
    const std::wstring file=RuntimeWide(value->text);
    HICON icon=static_cast<HICON>(LoadImageW(nullptr,file.c_str(),IMAGE_ICON,0,0,LR_LOADFROMFILE|LR_DEFAULTSIZE));
    if(icon==nullptr)return false;
    form.tray.cbSize=sizeof(NOTIFYICONDATAW);form.tray.hWnd=form.hwnd;form.tray.uID=form.id;form.tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;form.tray.uCallbackMessage=trayMessage;form.tray.hIcon=icon;
    wcsncpy_s(form.tray.szTip,std::size(form.tray.szTip),tip.c_str(),_TRUNCATE);
    if(!Shell_NotifyIconW(NIM_ADD,&form.tray)){DestroyIcon(icon);form.tray={};return false;}
    form.trayIcon=icon;form.trayRegistered=true;return true;
}
static void PopupMenuAt(HWND owner,const Value* menu,const Value* xValue,const Value* yValue) {
    if(owner==nullptr||menu==nullptr||menu->missing)return;
    HMENU handle=reinterpret_cast<HMENU>(static_cast<ULONG_PTR>(ToInteger(*menu)));if(handle==nullptr)return;
    POINT point{};GetCursorPos(&point);if(xValue!=nullptr&&!xValue->missing)point.x=static_cast<LONG>(ToInteger(*xValue));if(yValue!=nullptr&&!yValue->missing)point.y=static_cast<LONG>(ToInteger(*yValue));
    SetForegroundWindow(owner);TrackPopupMenu(handle,TPM_RIGHTBUTTON,point.x,point.y,0,owner,nullptr);PostMessageW(owner,WM_NULL,0,0);
}
static unsigned int FormIdFromWindow(HWND window) {
    const Form* form=FindFormRecord(window);
    return form==nullptr?0u:form->id;
}
static void DispatchFormNative(HWND window,unsigned int trigger,int nativeCode=0,std::vector<Value> values={}) {
    if(initializing||window==nullptr)return;
    const unsigned int form=FormIdFromWindow(window);
    if(form!=0)Dispatch(form,trigger,nativeCode,std::move(values));
}
static bool DispatchFormIdle(HWND window) {
    if(initializing||window==nullptr)return false;
    const unsigned int form=FormIdFromWindow(window);
    if(form==0)return false;
    Value result;
    Form* record=FindFormRecord(window);
    if(record==nullptr)return false;
    if(record->idleSince==0)record->idleSince=GetTickCount();
    const bool invoked=Dispatch(form,native_idle,0,{Integer(static_cast<unsigned int>(GetTickCount()-record->idleSince))},&result);
    return invoked&&result.type==T_BOOL&&ToBool(result);
}
static bool DispatchFormClose(HWND window) {
    if(initializing||window==nullptr)return true;
    const unsigned int form=FormIdFromWindow(window);
    if(form==0)return true;
    Value result;
    const bool invoked=Dispatch(form,native_closing,0,{},&result);
    return !invoked||result.type!=T_BOOL||ToBool(result);
}
static void RouteFormInput(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    switch(message) {
    case WM_LBUTTONDBLCLK:DispatchFormNative(window,native_double_clicked,-1,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_LBUTTONDOWN:DispatchFormNative(window,native_mouse_down,0,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_LBUTTONUP:DispatchFormNative(window,native_mouse_up,0,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_RBUTTONDBLCLK:DispatchFormNative(window,native_double_clicked,-1,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_RBUTTONDOWN:DispatchFormNative(window,native_right_mouse_down,0,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_RBUTTONUP:DispatchFormNative(window,native_right_mouse_up,0,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_MOUSEMOVE:if(BeginMouseTracking(window))DispatchFormNative(window,native_mouse_enter,0);DispatchFormNative(window,native_mouse_move,0,{Integer(static_cast<short>(LOWORD(lParam))),Integer(static_cast<short>(HIWORD(lParam))),Integer(static_cast<unsigned int>(wParam))});break;
    case WM_MOUSELEAVE:EndMouseTracking(window);DispatchFormNative(window,native_mouse_leave);break;
    case WM_KEYDOWN:case WM_SYSKEYDOWN:DispatchFormNative(window,native_key_down,static_cast<int>(wParam),{Integer(static_cast<unsigned int>(wParam)),Integer(static_cast<unsigned int>(lParam))});break;
    case WM_KEYUP:case WM_SYSKEYUP:DispatchFormNative(window,native_key_up,static_cast<int>(wParam),{Integer(static_cast<unsigned int>(wParam)),Integer(static_cast<unsigned int>(lParam))});break;
    case WM_CHAR:case WM_SYSCHAR:DispatchFormNative(window,native_char_input,static_cast<int>(wParam),{Integer(static_cast<unsigned int>(wParam))});break;
    case WM_SETFOCUS:DispatchFormNative(window,native_focus_gained);break;
    case WM_KILLFOCUS:DispatchFormNative(window,native_focus_lost);break;
    default:break;
    }
}
static LRESULT CALLBACK FormProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_NCDESTROY)EndMouseTracking(window);
    if(message==WM_COMMAND||message==WM_HSCROLL||message==WM_VSCROLL||message==WM_NOTIFY||
       message==WM_DRAWITEM||message==WM_MEASUREITEM||message==WM_CTLCOLORLISTBOX||
       message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLORBTN||
       message==WM_CTLCOLORSCROLLBAR) {
        const LRESULT routed=RouteChildNotification(window,message,wParam,lParam);
        if(routed!=(std::numeric_limits<LRESULT>::min)())return routed;
    }
    switch(message) {
    case WM_SIZE:DispatchFormNative(window,native_size_changed,0,{Integer(static_cast<unsigned int>(LOWORD(lParam))),Integer(static_cast<unsigned int>(HIWORD(lParam)))});break;
    case WM_MOVE:DispatchFormNative(window,native_moved,0,{Integer(static_cast<unsigned int>(LOWORD(lParam))),Integer(static_cast<unsigned int>(HIWORD(lParam)))});break;
    case WM_ACTIVATE: {
        const bool active=LOWORD(wParam)!=WA_INACTIVE;
        DispatchFormNative(window,active?native_activated:native_deactivated,static_cast<int>(LOWORD(wParam)));
        if(active&&!initializing) {
            if(auto* form=FindFormRecord(window);form!=nullptr&&!form->firstActivated) {
                form->firstActivated=true;
                DispatchFormNative(window,native_first_activated);
            }
        }
        break;
    }
    case WM_KEYDOWN:
        if(wParam==VK_ESCAPE) {
            const Form* form=FindFormRecord(window);
            if(form!=nullptr&&form->escapeCloses){SendMessageW(window,WM_CLOSE,0,0);return 0;}
        }
        if(wParam==VK_F1) {
            const Form* form=FindFormRecord(window);
            if(form!=nullptr&&form->f1OpenHelp) {
                const std::wstring file=form->helpFileName.empty()?L"help.hlp":form->helpFileName;
                ShellExecuteW(window,L"open",file.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
                return 0;
            }
        }
        RouteFormInput(window,message,wParam,lParam);
        break;
    case WM_SYSKEYDOWN:case WM_KEYUP:case WM_SYSKEYUP:case WM_CHAR:case WM_SYSCHAR:
    case WM_LBUTTONDBLCLK:case WM_LBUTTONDOWN:case WM_LBUTTONUP:case WM_RBUTTONDBLCLK:case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_MOUSEMOVE:case WM_MOUSELEAVE:
        RouteFormInput(window,message,wParam,lParam);break;
    case WM_SHOWWINDOW:DispatchFormNative(window,wParam?native_shown:native_hidden);break;
    case WM_ERASEBKGND: {
        Form* form=FindFormRecord(window);
        if(form!=nullptr&&form->hasBackColor) { RECT rect{};GetClientRect(window,&rect);HBRUSH brush=CreateSolidBrush(form->backColor);FillRect(reinterpret_cast<HDC>(wParam),&rect,brush);DeleteObject(brush);return 1; }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc=BeginPaint(window,&paint); if(Form* form=FindFormRecord(window)){if(!form->backPicData.empty())PaintBitmap(window,dc,LoadPictureMemory(form->backPicData),form->backPicMode);}
        activePaintWindow=window; activePaintDC=dc;
        DispatchFormNative(window,native_paint);
        activePaintDC=nullptr; activePaintWindow=nullptr; EndPaint(window,&paint); return 0;
    }
    case WM_TIMER:
        if(wParam==0xE1D1u) { if(!DispatchFormIdle(window))KillTimer(window,0xE1D1u); return 0; }
        DispatchFormNative(window,native_timer,static_cast<int>(wParam));break;
    case trayMessage: {
        const int operation=LOWORD(lParam)==WM_LBUTTONDBLCLK?2:(LOWORD(lParam)==WM_RBUTTONUP?3:1);
        DispatchFormNative(window,native_tray,operation,{Integer(operation)});return 0;
    }
    case WM_CLOSE:if(DispatchFormClose(window))DestroyWindow(window);return 0;
    case WM_DESTROY:if(Form* form=FindFormRecord(window))RemoveTrayIcon(*form);DispatchFormNative(window,native_destroyed);PostQuitMessage(0);return 0;
    case WM_NCHITTEST: {
        const Form* form=FindFormRecord(window);
        const LRESULT hit=DefWindowProcW(window,message,wParam,lParam);
        return form!=nullptr&&!form->canMove&&hit==HTCAPTION?HTCLIENT:hit;
    }
    default:break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
static LRESULT CALLBACK UnitSubclassProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_SETCURSOR) {
        if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->cursor!=nullptr) { SetCursor(unit->cursor);return TRUE; }
    }
    if(message==WM_NCDESTROY) {
        EndMouseTracking(window);
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->cursor!=nullptr) { DestroyCursor(unit->cursor);unit->cursor=nullptr; }
    }
    if(message==WM_CHAR) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"edit")==0) {
            wchar_t ch=static_cast<wchar_t>(wParam);
            if(unit->inputMode>=3&&ch>=32) {
                const bool numeric=ch>=L'0'&&ch<=L'9';
                const bool sign=(ch==L'-'||ch==L'+');
                const bool decimal=(ch==L'.'||ch==L',');
                const bool dateSep=(ch==L'/'||ch==L'-'||ch==L':'||ch==L' ');
                const bool allow=unit->inputMode==3?(numeric||sign):(unit->inputMode<=10?(numeric||sign||decimal):(numeric||dateSep));
                if(!allow)return 0;
            }
            if(unit->convertMode==1&&ch>=L'A'&&ch<=L'Z')ch=static_cast<wchar_t>(ch+L'a'-L'A');
            else if(unit->convertMode==2&&ch>=L'a'&&ch<=L'z')ch=static_cast<wchar_t>(ch-L'a'+L'A');
            if(ch!=static_cast<wchar_t>(wParam)) { RouteUnitInput(window,message,static_cast<WPARAM>(ch),lParam); return DefSubclassProc(window,message,static_cast<WPARAM>(ch),lParam); }
        }
    }
    if(message==WM_COMMAND||message==WM_HSCROLL||message==WM_VSCROLL||message==WM_NOTIFY||
       message==WM_DRAWITEM||message==WM_MEASUREITEM||message==WM_CTLCOLORLISTBOX||
       message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLORBTN||
       message==WM_CTLCOLORSCROLLBAR) {
        const LRESULT routed=RouteChildNotification(window,message,wParam,lParam);
        if(routed!=(std::numeric_limits<LRESULT>::min)())return routed;
    }
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc=BeginPaint(window,&paint); activePaintWindow=window; activePaintDC=dc;
        const RECT& dirty=paint.rcPaint;
        const bool painted=PaintUnitSurface(window,dc);
        const bool handled=DispatchNative(window,native_paint,0,{Integer(dirty.left),Integer(dirty.top),Integer(dirty.right),Integer(dirty.bottom)});
        activePaintDC=nullptr; activePaintWindow=nullptr; EndPaint(window,&paint);
        if(painted||handled)return 0;
    }
    else RouteUnitInput(window,message,wParam,lParam);
    const LRESULT result=DefSubclassProc(window,message,wParam,lParam);
    if(IsCheckList(window)) {
        if(message==WM_LBUTTONUP) {
            const int index=CheckItemAtPoint(window,lParam);
            if(index>=0) {
                Unit* unit=UnitFromWindow(window);
                EnsureCheckState(*unit,static_cast<std::size_t>(SendMessageW(window,LB_GETCOUNT,0,0)));
                if(unit->checkEnabled[static_cast<std::size_t>(index)])SetCheckState(window,index,unit->checkStates[static_cast<std::size_t>(index)]==0);
            }
        }
        else if(message==WM_KEYUP&&wParam==VK_SPACE) {
            const int index=CheckItemFromKey(window);
            if(index>=0) {
                Unit* unit=UnitFromWindow(window);
                EnsureCheckState(*unit,static_cast<std::size_t>(SendMessageW(window,LB_GETCOUNT,0,0)));
                if(unit->checkEnabled[static_cast<std::size_t>(index)])SetCheckState(window,index,unit->checkStates[static_cast<std::size_t>(index)]==0);
            }
        }
    }
    return result;
}
static LRESULT CALLBACK PageProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_ERASEBKGND) { HWND owner=GetParent(window);Unit* tab=UnitFromWindow(owner);if(tab!=nullptr&&tab->type!=nullptr&&std::strcmp(tab->type,"tab")==0&&tab->tabFillBack){RECT rect{};GetClientRect(window,&rect);HBRUSH brush=CreateSolidBrush(tab->tabBackColor);FillRect(reinterpret_cast<HDC>(wParam),&rect,brush);DeleteObject(brush);return 1;} }
    if(message==WM_COMMAND||message==WM_HSCROLL||message==WM_VSCROLL||message==WM_NOTIFY||
       message==WM_DRAWITEM||message==WM_MEASUREITEM||message==WM_CTLCOLORLISTBOX||
       message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLORBTN||
       message==WM_CTLCOLORSCROLLBAR) {
        const LRESULT routed=RouteChildNotification(window,message,wParam,lParam);
        if(routed!=(std::numeric_limits<LRESULT>::min)())return routed;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
static LRESULT CALLBACK ContainerProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_COMMAND||message==WM_HSCROLL||message==WM_VSCROLL||message==WM_NOTIFY||
       message==WM_DRAWITEM||message==WM_MEASUREITEM||message==WM_CTLCOLORLISTBOX||
       message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC||
       message==WM_CTLCOLORBTN||message==WM_CTLCOLORSCROLLBAR) {
        const LRESULT routed=RouteChildNotification(window,message,wParam,lParam);
        if(routed!=(std::numeric_limits<LRESULT>::min)())return routed;
    }
    RouteUnitInput(window,message,wParam,lParam);
    return DefWindowProcW(window,message,wParam,lParam);
}
static bool AttributeEquals(const char* value,const wchar_t* expected) {
    if(value==nullptr||expected==nullptr)return false;
    const int length=MultiByteToWideChar(CP_ACP,0,value,-1,nullptr,0);
    if(length<=0)return false;
    std::wstring actual(static_cast<std::size_t>(length),L'\0');
    if(MultiByteToWideChar(CP_ACP,0,value,-1,actual.data(),length)<=0)return false;
    actual.resize(static_cast<std::size_t>(length-1));
    return actual==expected;
}
static const char* XmlAttributeValue(const Spec& spec,const wchar_t* name) {
    for(std::size_t index=0;index<spec.attributeCount;++index)
        if(AttributeEquals(spec.attributes[index].name,name))return spec.attributes[index].value;
    return nullptr;
}
static int XmlAttributeInteger(const Spec& spec,const wchar_t* name,int fallback) {
    const char* value=XmlAttributeValue(spec,name);
    if(value==nullptr||*value==0)return fallback;
    char* end=nullptr;
    const long parsed=std::strtol(value,&end,10);
    return end==value||*end!=0?fallback:static_cast<int>(parsed);
}
static bool XmlAttributeBoolean(const Spec& spec,const wchar_t* name,bool fallback) {
    const char* value=XmlAttributeValue(spec,name);
    if(value==nullptr)return fallback;
    return AttributeEquals(value,L"真")||std::strcmp(value,"1")==0||AttributeEquals(value,L"true")
        ? true
        : (AttributeEquals(value,L"假")||std::strcmp(value,"0")==0||AttributeEquals(value,L"false") ? false : fallback);
}
static bool ClassEquals(HWND window,const wchar_t* expected);
static std::wstring EnsureDirectoryPath(std::wstring path) {
    if(path.empty()) { wchar_t buffer[MAX_PATH]{}; const DWORD length=GetCurrentDirectoryW(static_cast<DWORD>(std::size(buffer)),buffer); if(length>0&&length<std::size(buffer))path.assign(buffer,length); }
    while(!path.empty()&&(path.back()==L'\\'||path.back()==L'/'))path.pop_back();
    return path;
}
static void PopulateDriveList(HWND window,const Unit* unit=nullptr) {
    if(window==nullptr)return;
    SendMessageW(window,CB_RESETCONTENT,0,0);
    const DWORD mask=GetLogicalDrives();
    for(int index=0;index<26;++index)if((mask&(1u<<index))!=0) {
        wchar_t drive[4]{static_cast<wchar_t>(L'A'+index),L':',L'\\',L'\0'};
        const UINT driveKind=GetDriveTypeW(drive);
        const bool accepted=unit==nullptr||unit->driveType==0||
            (unit->driveType==1&&driveKind==DRIVE_REMOVABLE)||(unit->driveType==2&&driveKind==DRIVE_FIXED)||
            (unit->driveType==3&&driveKind==DRIVE_CDROM)||(unit->driveType==4&&driveKind==DRIVE_REMOTE)||
            (unit->driveType==5&&driveKind==DRIVE_RAMDISK);
        if(accepted)SendMessageW(window,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(drive));
    }
}
static bool FileAttributeAllowed(const WIN32_FIND_DATAW& data,const Spec& spec) {
    const DWORD attributes=data.dwFileAttributes;
    const bool special=(attributes&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM|FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_ARCHIVE))!=0;
    if(!special&&!XmlAttributeBoolean(spec,L"通常",true))return false;
    const auto allowed=[&spec](const wchar_t* name){const char* raw=XmlAttributeValue(spec,name);return raw==nullptr||XmlAttributeBoolean(spec,name,true);};
    if((attributes&FILE_ATTRIBUTE_HIDDEN)!=0&&!allowed(L"隐藏"))return false;
    if((attributes&FILE_ATTRIBUTE_SYSTEM)!=0&&!allowed(L"系统"))return false;
    if((attributes&FILE_ATTRIBUTE_READONLY)!=0&&!allowed(L"只读"))return false;
    if((attributes&FILE_ATTRIBUTE_ARCHIVE)!=0&&!allowed(L"存档"))return false;
    return true;
}
static bool FileAttributeAllowed(const WIN32_FIND_DATAW& data,const Unit& unit) {
    const DWORD attributes=data.dwFileAttributes;
    const bool special=(attributes&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM|FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_ARCHIVE))!=0;
    if(!special&&!unit.allowNormal)return false;
    if((attributes&FILE_ATTRIBUTE_HIDDEN)!=0&&!unit.allowHidden)return false;
    if((attributes&FILE_ATTRIBUTE_SYSTEM)!=0&&!unit.allowSystem)return false;
    if((attributes&FILE_ATTRIBUTE_READONLY)!=0&&!unit.allowReadOnly)return false;
    if((attributes&FILE_ATTRIBUTE_ARCHIVE)!=0&&!unit.allowArchive)return false;
    if(unit.hasMinDate||unit.hasMaxDate) {
        SYSTEMTIME modified{};
        if(!FileTimeToSystemTime(&data.ftLastWriteTime,&modified))return false;
        SYSTEMTIME minimum=unit.minDate,maximum=unit.maxDate;
        FILETIME modifiedFt{},minimumFt{},maximumFt{};
        if(!SystemTimeToFileTime(&modified,&modifiedFt))return false;
        if(unit.hasMinDate&&SystemTimeToFileTime(&minimum,&minimumFt)&&CompareFileTime(&modifiedFt,&minimumFt)<0)return false;
        if(unit.hasMaxDate&&SystemTimeToFileTime(&maximum,&maximumFt)&&CompareFileTime(&modifiedFt,&maximumFt)>0)return false;
    }
    return true;
}
static void PopulatePathList(HWND window,const Spec& spec,Unit& unit,const bool directories) {
    if(window==nullptr)return;
    unit.path=Wide(XmlAttributeValue(spec,L"目录") == nullptr ? "" : XmlAttributeValue(spec,L"目录"));
    unit.path=EnsureDirectoryPath(unit.path);
    unit.filePattern=Wide(XmlAttributeValue(spec,L"通配符") == nullptr ? "*.*" : XmlAttributeValue(spec,L"通配符"));
    if(unit.filePattern.empty())unit.filePattern=L"*.*";
    SendMessageW(window,LB_RESETCONTENT,0,0);
    std::vector<std::wstring> patterns;
    std::size_t begin=0;
    while(begin<=unit.filePattern.size()) { const std::size_t end=unit.filePattern.find(L';',begin); const std::size_t length=end==std::wstring::npos?unit.filePattern.size()-begin:end-begin; if(length>0)patterns.emplace_back(unit.filePattern,begin,length); if(end==std::wstring::npos)break; begin=end+1; }
    if(patterns.empty())patterns.push_back(L"*.*");
    std::unordered_set<std::wstring> seen;
    for(const auto& pattern:patterns) {
        const std::wstring search=unit.path.empty()?pattern:(unit.path+L"\\"+pattern);
        WIN32_FIND_DATAW data{}; HANDLE handle=FindFirstFileW(search.c_str(),&data); if(handle==INVALID_HANDLE_VALUE)continue;
        do {
            const std::wstring name=data.cFileName;
            if(name==L"."||name==L".."||!FileAttributeAllowed(data,spec))continue;
            const bool isDirectory=(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
            if(isDirectory!=directories)continue;
            if(seen.insert(name).second)SendMessageW(window,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
        } while(FindNextFileW(handle,&data));
        FindClose(handle);
    }
}
static void PopulatePathListRuntime(HWND window,Unit& unit,const bool directories) {
    if(window==nullptr)return;
    unit.path=EnsureDirectoryPath(unit.path);
    if(unit.filePattern.empty())unit.filePattern=L"*.*";
    SendMessageW(window,LB_RESETCONTENT,0,0);
    const std::wstring search=unit.path.empty()?unit.filePattern:(unit.path+L"\\"+unit.filePattern);
    WIN32_FIND_DATAW data{}; HANDLE handle=FindFirstFileW(search.c_str(),&data); if(handle==INVALID_HANDLE_VALUE)return;
    do {
        const std::wstring name=data.cFileName;
        if(name==L"."||name==L"..")continue;
        const bool isDirectory=(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        if(isDirectory==directories&&FileAttributeAllowed(data,unit))SendMessageW(window,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
    } while(FindNextFileW(handle,&data));
    FindClose(handle);
}
static bool OleDateSystemTime(const double ole,SYSTEMTIME& result) {
    if(ole==0.0||!std::isfinite(ole))return false;
    SYSTEMTIME epoch{};epoch.wYear=1899;epoch.wMonth=12;epoch.wDay=30;
    FILETIME epochFile{};
    if(!SystemTimeToFileTime(&epoch,&epochFile))return false;
    ULARGE_INTEGER epochTicks{};epochTicks.LowPart=epochFile.dwLowDateTime;epochTicks.HighPart=epochFile.dwHighDateTime;
    const long double delta=static_cast<long double>(ole)*864000000000.0L;
    const long double total=static_cast<long double>(epochTicks.QuadPart)+delta;
    if(total<0.0L||total>static_cast<long double>((std::numeric_limits<ULONGLONG>::max)()))return false;
    ULARGE_INTEGER ticks{};ticks.QuadPart=static_cast<ULONGLONG>(total<0.0L?total-0.5L:total+0.5L);
    FILETIME file{};file.dwLowDateTime=ticks.LowPart;file.dwHighDateTime=ticks.HighPart;
    return FileTimeToSystemTime(&file,&result)!=FALSE;
}
static bool XmlAttributeSystemTime(const Spec& spec,const wchar_t* name,SYSTEMTIME& result) {
    const char* text=XmlAttributeValue(spec,name);
    if(text==nullptr||*text==0)return false;
    char* end=nullptr;
    const double ole=std::strtod(text,&end);
    if(end==text||*end!=0)return false;
    return OleDateSystemTime(ole,result);
}
static double SystemTimeOleDate(const SYSTEMTIME& value) {
    SYSTEMTIME epoch{};epoch.wYear=1899;epoch.wMonth=12;epoch.wDay=30;
    FILETIME file{};FILETIME epochFile{};
    if(!SystemTimeToFileTime(&value,&file)||!SystemTimeToFileTime(&epoch,&epochFile))return 0.0;
    ULARGE_INTEGER ticks{};ticks.LowPart=file.dwLowDateTime;ticks.HighPart=file.dwHighDateTime;
    ULARGE_INTEGER epochTicks{};epochTicks.LowPart=epochFile.dwLowDateTime;epochTicks.HighPart=epochFile.dwHighDateTime;
    return static_cast<double>(static_cast<long double>(ticks.QuadPart)-static_cast<long double>(epochTicks.QuadPart))/864000000000.0;
}
static bool WindowDateValue(HWND window,SYSTEMTIME& result) {
    if(ClassEquals(window,L"SysDateTimePick32"))return SendMessageW(window,DTM_GETSYSTEMTIME,0,reinterpret_cast<LPARAM>(&result))==GDT_VALID;
    if(ClassEquals(window,L"SysMonthCal32")) {if(SendMessageW(window,MCM_GETCURSEL,0,reinterpret_cast<LPARAM>(&result))!=FALSE)return true;SYSTEMTIME range[2]{};if(SendMessageW(window,MCM_GETSELRANGE,0,reinterpret_cast<LPARAM>(range))!=FALSE){result=range[0];return true;}}
    return false;
}
static bool SetWindowDateValue(HWND window,const SYSTEMTIME& value) {
    if(ClassEquals(window,L"SysDateTimePick32"))return SendMessageW(window,DTM_SETSYSTEMTIME,GDT_VALID,reinterpret_cast<LPARAM>(&value))!=FALSE;
    if(ClassEquals(window,L"SysMonthCal32")) {if(SendMessageW(window,MCM_SETCURSEL,0,reinterpret_cast<LPARAM>(&value))!=FALSE)return true;SYSTEMTIME range[2]{value,value};return SendMessageW(window,MCM_SETSELRANGE,0,reinterpret_cast<LPARAM>(range))!=FALSE;}
    return false;
}
static std::wstring DialogFilterBuffer(const std::wstring& filter) {
    std::wstring result=filter;
    for(auto& value:result)if(value==L'|')value=L'\0';
    if(result.empty()) {
        result.append(L"所有文件");
        result.push_back(L'\0');
        result.append(L"*.*");
    }
    else if(result.back()!=L'\0')result.push_back(L'\0');
    result.push_back(L'\0');
    return result;
}
static bool OpenCommonDialog(HWND owner,Unit& unit) {
    if(unit.dialogType==0||unit.dialogType==1) {
        wchar_t fileName[32768]{};
        if(!unit.dialogFileName.empty())wcsncpy_s(fileName,std::size(fileName),unit.dialogFileName.c_str(),_TRUNCATE);
        std::wstring filter=DialogFilterBuffer(unit.dialogFilter);
        OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
        dialog.hwndOwner=owner; dialog.lpstrFile=fileName; dialog.nMaxFile=static_cast<DWORD>(std::size(fileName));
        dialog.lpstrFilter=filter.c_str(); dialog.nFilterIndex=static_cast<DWORD>((std::max)(1,unit.dialogFilterIndex+1));
        dialog.lpstrInitialDir=unit.dialogInitialDir.empty()?nullptr:unit.dialogInitialDir.c_str();
        dialog.lpstrDefExt=unit.dialogDefExt.empty()?nullptr:unit.dialogDefExt.c_str();
        dialog.lpstrTitle=unit.dialogCaption.empty()?nullptr:unit.dialogCaption.c_str();
        dialog.Flags=0;
        if(unit.dialogType==0&&unit.dialogFileMustExist)dialog.Flags|=OFN_FILEMUSTEXIST;
        if(unit.dialogType==0&&unit.dialogCreatePrompt)dialog.Flags|=OFN_CREATEPROMPT;
        if(unit.dialogType==1&&unit.dialogOverwritePrompt)dialog.Flags|=OFN_OVERWRITEPROMPT;
        if(unit.dialogPathMustExist)dialog.Flags|=OFN_PATHMUSTEXIST;
        if(unit.dialogNoChangeDir)dialog.Flags|=OFN_NOCHANGEDIR;
        const BOOL opened=unit.dialogType==0?GetOpenFileNameW(&dialog):GetSaveFileNameW(&dialog);
        if(opened==FALSE)return false;
        unit.dialogFileName=fileName;unit.dialogFilterIndex=static_cast<int>(dialog.nFilterIndex)-1;
        return true;
    }
    if(unit.dialogType==2) {
        LOGFONTW logFont{}; logFont.lfHeight=-12; logFont.lfCharSet=DEFAULT_CHARSET;logFont.lfWeight=unit.dialogFontBold?FW_BOLD:FW_NORMAL;logFont.lfItalic=unit.dialogFontItalic;logFont.lfStrikeOut=unit.dialogFontStrikeOut;logFont.lfUnderline=unit.dialogFontUnderline;
        if(!unit.dialogFontName.empty())wcsncpy_s(logFont.lfFaceName,std::size(logFont.lfFaceName),unit.dialogFontName.c_str(),_TRUNCATE);
        CHOOSEFONTW dialog{sizeof(CHOOSEFONTW)}; dialog.hwndOwner=owner; dialog.lpLogFont=&logFont;dialog.rgbColors=unit.dialogFontColor;
        dialog.Flags=CF_SCREENFONTS|CF_EFFECTS|CF_INITTOLOGFONTSTRUCT;
        if(ChooseFontW(&dialog)==FALSE)return false;
        unit.dialogFontColor=dialog.rgbColors;unit.dialogFontBold=logFont.lfWeight>=FW_BOLD;unit.dialogFontItalic=logFont.lfItalic!=FALSE;unit.dialogFontStrikeOut=logFont.lfStrikeOut!=FALSE;unit.dialogFontUnderline=logFont.lfUnderline!=FALSE;unit.dialogFontName=logFont.lfFaceName;HDC dc=GetDC(owner);const int dpi=dc==nullptr?96:GetDeviceCaps(dc,LOGPIXELSY);if(dc!=nullptr)ReleaseDC(owner,dc);unit.dialogFontSize=MulDiv(-logFont.lfHeight,72,dpi);return true;
    }
    if(unit.dialogType==3) {
        const std::wstring file=unit.dialogFileName.empty()?L"help.hlp":unit.dialogFileName;
        const UINT command=unit.dialogHelpCommand==1?HELP_CONTEXT:unit.dialogHelpCommand==2?HELP_CONTEXTPOPUP:unit.dialogHelpCommand==3?HELP_QUIT:unit.dialogHelpCommand==4?HELP_HELPONHELP:unit.dialogHelpCommand==5?HELP_QUIT:HELP_FINDER;
        return WinHelpW(owner,file.c_str(),command,static_cast<ULONG_PTR>(unit.dialogHelpContext))!=FALSE;
    }
    return false;
}
static HDC CurrentPaintDC(HWND window) {
    return window!=nullptr&&window==activePaintWindow?activePaintDC:nullptr;
}
static const char* RawAttributeValue(const XmlAttribute* attributes,std::size_t count,const wchar_t* name) {
    if(attributes==nullptr||name==nullptr)return nullptr;
    for(std::size_t index=0;index<count;++index)
        if(AttributeEquals(attributes[index].name,name))return attributes[index].value;
    return nullptr;
}
static int RawAttributeInteger(const XmlAttribute* attributes,std::size_t count,const wchar_t* name,int fallback) {
    const char* value=RawAttributeValue(attributes,count,name);if(value==nullptr||*value==0)return fallback;
    char* end=nullptr;const long parsed=std::strtol(value,&end,10);return end==value||*end!=0?fallback:static_cast<int>(parsed);
}
static bool RawAttributeBoolean(const XmlAttribute* attributes,std::size_t count,const wchar_t* name,bool fallback) {
    const char* value=RawAttributeValue(attributes,count,name);if(value==nullptr)return fallback;
    return std::strcmp(value,"1")==0||AttributeEquals(value,L"真")||AttributeEquals(value,L"true")?true:
        (std::strcmp(value,"0")==0||AttributeEquals(value,L"假")||AttributeEquals(value,L"false")?false:fallback);
}
static int Base64Value(unsigned char value) {
    if(value>='A'&&value<='Z')return value-'A';if(value>='a'&&value<='z')return value-'a'+26;
    if(value>='0'&&value<='9')return value-'0'+52;if(value=='+')return 62;if(value=='/')return 63;return -1;
}
static std::vector<unsigned char> DecodeBase64(const char* text) {
    std::vector<unsigned char> result;if(text==nullptr||*text==0)return result;const std::size_t length=std::strlen(text);if((length&3)!=0)return result;
    result.reserve(length/4*3);
    for(std::size_t index=0;index<length;index+=4){const unsigned char c0=static_cast<unsigned char>(text[index]),c1=static_cast<unsigned char>(text[index+1]),c2=static_cast<unsigned char>(text[index+2]),c3=static_cast<unsigned char>(text[index+3]);const int a=Base64Value(c0),b=Base64Value(c1),c=c2=='='?0:Base64Value(c2),d=c3=='='?0:Base64Value(c3);if(a<0||b<0||c<0||d<0)return {};const unsigned int value=(static_cast<unsigned int>(a)<<18)|(static_cast<unsigned int>(b)<<12)|(static_cast<unsigned int>(c)<<6)|static_cast<unsigned int>(d);result.push_back(static_cast<unsigned char>(value>>16));if(c2!='=')result.push_back(static_cast<unsigned char>(value>>8));if(c3!='=')result.push_back(static_cast<unsigned char>(value));}
    return result;
}
static int CanvasPenStyle(const int style) {
    switch(style) {
    case 0:return PS_NULL;
    case 2:return PS_DASH;
    case 3:return PS_DOT;
    case 4:return PS_DASHDOT;
    case 5:return PS_DASHDOTDOT;
    case 6:return PS_INSIDEFRAME;
    default:return PS_SOLID;
    }
}
static int CanvasBrushStyle(const int style) {
    switch(style) {
    case 2:return HS_BDIAGONAL;
    case 3:return HS_CROSS;
    case 4:return HS_DIAGCROSS;
    case 5:return HS_FDIAGONAL;
    case 6:return HS_HORIZONTAL;
    case 7:return HS_VERTICAL;
    default:return -1;
    }
}
static void PaintShape(HWND window, HDC dc, const Unit& unit) {
    RECT rect{}; GetClientRect(window, &rect);
    HPEN pen=CreatePen(unit.lineStyle==0?PS_NULL:CanvasPenStyle(unit.lineStyle), (std::max)(1,unit.lineWidth), unit.hasLineColor?unit.lineColor:RGB(0,0,0));
    HBRUSH brush=unit.hasFillColor?CreateSolidBrush(unit.fillColor):static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen=pen?SelectObject(dc,pen):nullptr; HGDIOBJ oldBrush=brush?SelectObject(dc,brush):nullptr;
    switch(unit.shape) {
    case 1: { const LONG side=(std::min)(rect.right-rect.left,rect.bottom-rect.top); ::Rectangle(dc,rect.left,rect.top,rect.left+side,rect.top+side); break; }
    case 2: ::Ellipse(dc,rect.left,rect.top,rect.right,rect.bottom); break;
    case 3: { const LONG side=(std::min)(rect.right-rect.left,rect.bottom-rect.top); ::Ellipse(dc,rect.left,rect.top,rect.left+side,rect.top+side); break; }
    case 4: ::RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,12,12); break;
    case 5: { const LONG side=(std::min)(rect.right-rect.left,rect.bottom-rect.top); ::RoundRect(dc,rect.left,rect.top,rect.left+side,rect.top+side,12,12); break; }
    case 6: MoveToEx(dc,rect.left,(rect.top+rect.bottom)/2,nullptr); LineTo(dc,rect.right,(rect.top+rect.bottom)/2); break;
    case 7: MoveToEx(dc,(rect.left+rect.right)/2,rect.top,nullptr); LineTo(dc,(rect.left+rect.right)/2,rect.bottom); break;
    default: ::Rectangle(dc,rect.left,rect.top,rect.right,rect.bottom); break;
    }
    if(oldPen)SelectObject(dc,oldPen); if(oldBrush)SelectObject(dc,oldBrush); if(pen)DeleteObject(pen); if(unit.hasFillColor&&brush)DeleteObject(brush);
}
static COLORREF BlendColor(COLORREF first,COLORREF second,int step,int total) {
    if(total<=0)return first;
    const int r=GetRValue(first)+(GetRValue(second)-GetRValue(first))*step/total;
    const int g=GetGValue(first)+(GetGValue(second)-GetGValue(first))*step/total;
    const int b=GetBValue(first)+(GetBValue(second)-GetBValue(first))*step/total;
    return RGB((std::max)(0,(std::min)(255,r)),(std::max)(0,(std::min)(255,g)),(std::max)(0,(std::min)(255,b)));
}
static HBITMAP LoadPictureMemory(const std::vector<unsigned char>& bytes);
static void PaintBitmap(HWND window,HDC dc,HBITMAP bitmap,int mode);
static void PaintLabel(HWND window,HDC dc,const Unit& unit) {
    RECT rect{};GetClientRect(window,&rect);
    if(unit.gradientBackMode!=0&&unit.gradientBackColorSet[0]) {
        const bool horizontal=unit.gradientBackMode==2||unit.gradientBackMode==6;
        const int length=horizontal?(std::max)(1,rect.right):(std::max)(1,rect.bottom);
        for(int i=0;i<length;++i) {
            const int pos=(i*100)/(std::max)(1,length-1);
            const COLORREF color=BlendColor(unit.gradientBackColor[0],unit.gradientBackColorSet[1]?unit.gradientBackColor[1]:unit.gradientBackColor[0],pos,100);
            HBRUSH brush=CreateSolidBrush(color);RECT band=rect;
            if(horizontal){band.left=i;band.right=i+1;}else{band.top=i;band.bottom=i+1;}
            FillRect(dc,&band,brush);DeleteObject(brush);
        }
    } else {
        HBRUSH brush=unit.hasBackColor?CreateSolidBrush(unit.backColor):static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        FillRect(dc,&rect,brush);if(unit.hasBackColor)DeleteObject(brush);
    }
    if(!unit.imageData.empty()) { HBITMAP bitmap=LoadPictureMemory(unit.imageData); PaintBitmap(window,dc,bitmap,unit.backPicMode); }
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(window)+1),L'\0');
    GetWindowTextW(window,text.data(),static_cast<int>(text.size()));text.resize(std::wcslen(text.c_str()));
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,unit.hasTextColor?unit.textColor:GetSysColor(COLOR_WINDOWTEXT));
    UINT format=DT_NOPREFIX|(unit.labelWordWrap?DT_WORDBREAK:DT_SINGLELINE);
    if(unit.horizontalAlign==1)format|=DT_CENTER;else if(unit.horizontalAlign==2)format|=DT_RIGHT;else format|=DT_LEFT;
    if(!unit.labelWordWrap){if(unit.verticalAlign==1)format|=DT_VCENTER;else if(unit.verticalAlign==2)format|=DT_BOTTOM;}
    DrawTextW(dc,text.c_str(),-1,&rect,format);
    if(unit.gradientBorderWidth>0&&unit.gradientBorderColorSet[0]) {
        const int width=(std::max)(1,unit.gradientBorderWidth);
        const COLORREF color=unit.gradientBorderColor[0];
        HPEN pen=CreatePen(PS_SOLID,width,color);
        if(pen!=nullptr) {
            HGDIOBJ old=SelectObject(dc,pen);
            RECT border=rect;
            const int half=width/2;
            InflateRect(&border,-half,-half);
            MoveToEx(dc,border.left,border.top,nullptr);LineTo(dc,border.right-1,border.top);
            LineTo(dc,border.right-1,border.bottom-1);LineTo(dc,border.left,border.bottom-1);LineTo(dc,border.left,border.top);
            SelectObject(dc,old);DeleteObject(pen);
        }
    }
    if(unit.labelEffect==1||unit.labelEffect==2) { RECT border=rect;HPEN pen=CreatePen(PS_SOLID,1,unit.labelEffect==1?RGB(255,255,255):RGB(0,0,0));HGDIOBJ old=SelectObject(dc,pen);MoveToEx(dc,border.left,border.bottom-1,nullptr);LineTo(dc,border.right,border.bottom-1);LineTo(dc,border.right-1,border.top);SelectObject(dc,old);DeleteObject(pen); }
}
static void PaintHyperlink(HWND window,HDC dc,const Unit& unit) {
    Unit styled=unit; styled.hasTextColor=true; styled.textColor=unit.hyperlinkHot?unit.hotColor:(unit.visited?unit.visitedColor:RGB(0,0,238)); styled.labelUnderline=true; PaintLabel(window,dc,styled);
    RECT rect{};GetClientRect(window,&rect);HPEN pen=CreatePen(PS_SOLID,1,styled.textColor);if(pen!=nullptr){HGDIOBJ old=SelectObject(dc,pen);const int y=rect.bottom-2;MoveToEx(dc,rect.left,y,nullptr);LineTo(dc,rect.right,y);SelectObject(dc,old);DeleteObject(pen);}
}
static HBITMAP ClonePictureHandle(OLE_HANDLE handle) {
    if(handle==0)return nullptr;
    return static_cast<HBITMAP>(CopyImage(reinterpret_cast<HBITMAP>(static_cast<std::uintptr_t>(handle)),IMAGE_BITMAP,0,0,LR_CREATEDIBSECTION));
}
static HBITMAP LoadPictureMemory(const std::vector<unsigned char>& bytes) {
    if(bytes.empty())return nullptr;
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes.size());if(memory==nullptr)return nullptr;
    void* target=GlobalLock(memory);if(target==nullptr){GlobalFree(memory);return nullptr;}std::memcpy(target,bytes.data(),bytes.size());GlobalUnlock(memory);
    IStream* stream=nullptr;if(FAILED(CreateStreamOnHGlobal(memory,TRUE,&stream))) {GlobalFree(memory);return nullptr;}
    IPicture* picture=nullptr;HBITMAP result=nullptr;
    if(SUCCEEDED(OleLoadPicture(stream,static_cast<LONG>(bytes.size()),FALSE,IID_IPicture,reinterpret_cast<void**>(&picture)))&&picture!=nullptr){OLE_HANDLE handle=0;if(SUCCEEDED(picture->get_Handle(&handle)))result=ClonePictureHandle(handle);picture->Release();}
    stream->Release();return result;
}
static HBITMAP LoadPicturePath(const std::wstring& path) {
    if(path.empty())return nullptr;
    HBITMAP bitmap=static_cast<HBITMAP>(LoadImageW(nullptr,path.c_str(),IMAGE_BITMAP,0,0,LR_LOADFROMFILE|LR_CREATEDIBSECTION));if(bitmap!=nullptr)return bitmap;
    IPicture* picture=nullptr;HBITMAP result=nullptr;
    if(SUCCEEDED(OleLoadPicturePath(const_cast<LPOLESTR>(path.c_str()),nullptr,0,0,IID_IPicture,reinterpret_cast<void**>(&picture)))&&picture!=nullptr){OLE_HANDLE handle=0;if(SUCCEEDED(picture->get_Handle(&handle)))result=ClonePictureHandle(handle);picture->Release();}
    return result;
}
static void PaintBitmap(HWND window,HDC dc,HBITMAP bitmap,int mode) {
    if(bitmap==nullptr)return;BITMAP info{};GetObjectW(bitmap,sizeof(info),&info);HDC source=CreateCompatibleDC(dc);if(source==nullptr){DeleteObject(bitmap);return;}HGDIOBJ old=SelectObject(source,bitmap);RECT rect{};GetClientRect(window,&rect);int x=0,y=0,w=info.bmWidth,h=info.bmHeight;if(mode==1){w=rect.right;h=rect.bottom;}else if(mode==2){x=(rect.right-w)/2;y=(rect.bottom-h)/2;}StretchBlt(dc,x,y,w,h,source,0,0,info.bmWidth,info.bmHeight,SRCCOPY);SelectObject(source,old);DeleteDC(source);DeleteObject(bitmap);
}
static void DrawBitmapAt(HDC dc,HBITMAP bitmap,int x,int y,int width,int height) {
    if(dc==nullptr||bitmap==nullptr)return;
    BITMAP info{};if(GetObjectW(bitmap,sizeof(info),&info)==0){DeleteObject(bitmap);return;}
    HDC source=CreateCompatibleDC(dc);if(source==nullptr){DeleteObject(bitmap);return;}
    HGDIOBJ old=SelectObject(source,bitmap);
    const int drawWidth=width>0?width:info.bmWidth,drawHeight=height>0?height:info.bmHeight;
    StretchBlt(dc,x,y,drawWidth,drawHeight,source,0,0,info.bmWidth,info.bmHeight,SRCCOPY);
    SelectObject(source,old);DeleteDC(source);DeleteObject(bitmap);
}
static std::vector<unsigned char> BitmapToBytes(HBITMAP bitmap) {
    std::vector<unsigned char> result; if(bitmap==nullptr)return result;
    BITMAP info{};if(GetObjectW(bitmap,sizeof(info),&info)==0)return result;
    HDC dc=GetDC(nullptr);if(dc==nullptr)return result;
    BITMAPINFOHEADER header{};header.biSize=sizeof(header);header.biWidth=info.bmWidth;header.biHeight=-std::abs(info.bmHeight);header.biPlanes=1;header.biBitCount=32;header.biCompression=BI_RGB;
    const std::size_t stride=static_cast<std::size_t>(info.bmWidth)*4u;const std::size_t imageSize=stride*static_cast<std::size_t>(std::abs(info.bmHeight));
    std::vector<unsigned char> pixels(imageSize);if(GetDIBits(dc,bitmap,0,static_cast<UINT>(std::abs(info.bmHeight)),pixels.data(),reinterpret_cast<BITMAPINFO*>(&header),DIB_RGB_COLORS)==0){ReleaseDC(nullptr,dc);return result;}ReleaseDC(nullptr,dc);
    BITMAPFILEHEADER file{};file.bfType=0x4D42;file.bfOffBits=sizeof(file)+sizeof(header);file.bfSize=file.bfOffBits+static_cast<DWORD>(pixels.size());result.resize(file.bfSize);std::memcpy(result.data(),&file,sizeof(file));std::memcpy(result.data()+sizeof(file),&header,sizeof(header));std::memcpy(result.data()+file.bfOffBits,pixels.data(),pixels.size());return result;
}
static void PaintImageFile(HWND window, HDC dc, const Unit& unit) {
    HBITMAP bitmap=!unit.imageData.empty()?LoadPictureMemory(unit.imageData):LoadPicturePath(unit.imageFileName);
    PaintBitmap(window,dc,bitmap,unit.imageDrawMode);
}
struct CanvasPaintGuard {
    HDC dc=nullptr; HGDIOBJ oldPen=nullptr; HGDIOBJ oldBrush=nullptr; HPEN pen=nullptr; HBRUSH brush=nullptr;
    CanvasPaintGuard(HDC target,Unit* unit):dc(target) {
        if(dc==nullptr||unit==nullptr)return;
        ::SetROP2(dc,unit->drawRop2>=1&&unit->drawRop2<=16?unit->drawRop2:R2_COPYPEN);
        const int width=(std::max)(1,unit->penWidth);
        pen=CreatePen(CanvasPenStyle(unit->penStyle),width,unit->penColor);
        if(pen!=nullptr)oldPen=SelectObject(dc,pen);
        const int hatch=CanvasBrushStyle(unit->brushStyle);
        brush=hatch<0?CreateSolidBrush(unit->brushColor):CreateHatchBrush(hatch,unit->brushColor);
        if(brush!=nullptr)oldBrush=SelectObject(dc,brush);
        SetTextColor(dc,unit->textColor);SetBkColor(dc,unit->textBackColor);SetBkMode(dc,TRANSPARENT);
    }
    ~CanvasPaintGuard() {
        if(dc==nullptr)return;
        if(oldPen!=nullptr)SelectObject(dc,oldPen);if(oldBrush!=nullptr)SelectObject(dc,oldBrush);
        if(pen!=nullptr)DeleteObject(pen);if(brush!=nullptr)DeleteObject(brush);
    }
};
static bool PaintUnitSurface(HWND window,HDC dc) {
    Unit* unit=UnitFromWindow(window);
    if(unit==nullptr||unit->type==nullptr)return false;
    if(std::strcmp(unit->type,"shape")==0) {
        PaintShape(window,dc,*unit);
        return true;
    }
    if(std::strcmp(unit->type,"image")==0) {
        RECT rect{};GetClientRect(window,&rect);
        HBRUSH background=unit->hasBackColor?CreateSolidBrush(unit->backColor):static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        FillRect(dc,&rect,background);if(unit->hasBackColor)DeleteObject(background);
        PaintImageFile(window,dc,*unit);
        return true;
    }
    if(std::strcmp(unit->type,"label")==0) {
        PaintLabel(window,dc,*unit);
        return true;
    }
    if(std::strcmp(unit->type,"hyperlink")==0) {
        PaintHyperlink(window,dc,*unit);
        return true;
    }
    if(std::strcmp(unit->type,"canvas")==0) {
        RECT rect{};GetClientRect(window,&rect);
        HBRUSH background=unit->hasBackColor?CreateSolidBrush(unit->backColor):static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        FillRect(dc,&rect,background);if(unit->hasBackColor)DeleteObject(background);
        if(!unit->imageData.empty()) { HBITMAP bitmap=LoadPictureMemory(unit->imageData); PaintBitmap(window,dc,bitmap,unit->backPicMode); }
        return true;
    }
    return false;
}
static bool SetDateRange(HWND window,const SYSTEMTIME* minimum,const SYSTEMTIME* maximum) {
    if(!ClassEquals(window,L"SysDateTimePick32"))return false;
    SYSTEMTIME range[2]{};DWORD flags=0;
    if(minimum!=nullptr){range[0]=*minimum;flags|=GDTR_MIN;}
    if(maximum!=nullptr){range[1]=*maximum;flags|=GDTR_MAX;}
    return SendMessageW(window,DTM_SETRANGE,flags,reinterpret_cast<LPARAM>(range))!=FALSE;
}
static bool GetDateRange(HWND window,SYSTEMTIME* minimum,SYSTEMTIME* maximum) {
    if(!ClassEquals(window,L"SysDateTimePick32"))return false;
    SYSTEMTIME range[2]{};const LRESULT flags=SendMessageW(window,DTM_GETRANGE,0,reinterpret_cast<LPARAM>(range));
    bool found=false;
    if(minimum!=nullptr&&((flags&GDTR_MIN)!=0)){*minimum=range[0];found=true;}
    if(maximum!=nullptr&&((flags&GDTR_MAX)!=0)){*maximum=range[1];found=true;}
    return found;
}
static bool GetMonthRange(HWND window,SYSTEMTIME* minimum,SYSTEMTIME* maximum) {
    if(!ClassEquals(window,L"SysMonthCal32"))return false;
    SYSTEMTIME range[2]{};const LRESULT flags=SendMessageW(window,MCM_GETRANGE,0,reinterpret_cast<LPARAM>(range));
    bool found=false;
    if(minimum!=nullptr&&((flags&GDTR_MIN)!=0)){*minimum=range[0];found=true;}
    if(maximum!=nullptr&&((flags&GDTR_MAX)!=0)){*maximum=range[1];found=true;}
    return found;
}
static DWORD Style(const Spec& spec) {
    const char* type=spec.type;
    if(std::strcmp(type,"button")==0) {
        DWORD style=WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON|BS_NOTIFY;
        if(XmlAttributeInteger(spec,L"类型",0)==1)style=(style&~BS_TYPEMASK)|BS_DEFPUSHBUTTON;
        const int horizontal=XmlAttributeInteger(spec,L"横向对齐方式",1);
        if(horizontal==0)style=(style&~BS_CENTER)|BS_LEFT;
        else if(horizontal==2)style=(style&~BS_CENTER)|BS_RIGHT;
        const int vertical=XmlAttributeInteger(spec,L"纵向对齐方式",1);
        if(vertical==0)style=(style&~BS_TOP)|BS_TOP;
        else if(vertical==2)style=(style&~BS_TOP)|BS_BOTTOM;
        return style;
    }
    if(std::strcmp(type,"checkbox")==0) {
        DWORD style=WS_CHILD|WS_TABSTOP|BS_AUTOCHECKBOX|BS_NOTIFY;
        if(XmlAttributeBoolean(spec,L"按钮形式",false))style|=BS_PUSHLIKE;
        if(XmlAttributeBoolean(spec,L"平面",false))style|=BS_FLAT;
        return style;
    }
    if(std::strcmp(type,"radio")==0)return WS_CHILD|WS_TABSTOP|BS_AUTORADIOBUTTON|BS_NOTIFY;
    if(std::strcmp(type,"group")==0)return WS_CHILD|BS_GROUPBOX;
    if(std::strcmp(type,"edit")==0) {
        DWORD style=WS_CHILD|WS_BORDER|WS_TABSTOP|ES_LEFT;
        if(XmlAttributeBoolean(spec,L"是否允许多行",false)) {
            style|=ES_MULTILINE|ES_AUTOVSCROLL;
            const int scrollBar=XmlAttributeInteger(spec,L"滚动条",2);
            if(scrollBar==1||scrollBar==3)style|=WS_HSCROLL|ES_AUTOHSCROLL;
            if(scrollBar==2||scrollBar==3)style|=WS_VSCROLL;
        }
        const int inputMode=XmlAttributeInteger(spec,L"输入方式",0);
        if(inputMode==1)style|=ES_READONLY;
        if(inputMode==2)style|=ES_PASSWORD;
        const int align=XmlAttributeInteger(spec,L"对齐方式",0);
        if(align==1)style=(style&~ES_LEFT)|ES_CENTER;
        else if(align==2)style=(style&~ES_LEFT)|ES_RIGHT;
        return style;
    }
    if(std::strcmp(type,"list")==0||std::strcmp(type,"checklist")==0) {
        DWORD style=WS_CHILD|WS_BORDER|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL;
        if(std::strcmp(type,"checklist")==0)style|=LBS_OWNERDRAWFIXED|LBS_HASSTRINGS;
        if(XmlAttributeBoolean(spec,L"允许选择多项",false))style|=LBS_EXTENDEDSEL;
        if(XmlAttributeBoolean(spec,L"多列",false))style|=LBS_MULTICOLUMN;
        if(XmlAttributeBoolean(spec,L"自动排序",false))style|=LBS_SORT;
        return style;
    }
    if(std::strcmp(type,"combo")==0) {
        const int comboType=XmlAttributeInteger(spec,L"类型",2);
        DWORD comboStyle=comboType==0?CBS_SIMPLE:(comboType==1?CBS_DROPDOWN:CBS_DROPDOWNLIST);
        if(XmlAttributeBoolean(spec,L"自动排序",false))comboStyle|=CBS_SORT;
        return WS_CHILD|WS_TABSTOP|comboStyle|WS_VSCROLL;
    }
    if(std::strcmp(type,"drive")==0)return WS_CHILD|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL;
    if(std::strcmp(type,"directory")==0)return WS_CHILD|WS_BORDER|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL;
    if(std::strcmp(type,"file")==0) {
        DWORD style=WS_CHILD|WS_BORDER|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL;
        if(XmlAttributeBoolean(spec,L"允许选择多项",false))style|=LBS_EXTENDEDSEL;
        return style;
    }
    if(std::strcmp(type,"color")==0)return WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON|BS_NOTIFY;
    if(std::strcmp(type,"hyperlink")==0)return WS_CHILD|SS_NOTIFY|SS_LEFT;
    if(std::strcmp(type,"spin")==0) {
        DWORD style=WS_CHILD|UDS_ARROWKEYS;
        if(XmlAttributeBoolean(spec,L"热点跟踪",false))style|=UDS_HOTTRACK;
        if(XmlAttributeInteger(spec,L"方向",0)==0)style|=UDS_HORZ;
        return style;
    }
    if(std::strcmp(type,"tab")==0)return WS_CHILD|WS_CLIPSIBLINGS|WS_TABSTOP;
    if(std::strcmp(type,"progress")==0) {
        return WS_CHILD|(XmlAttributeInteger(spec,L"方向",0)!=0?PBS_VERTICAL:0);
    }
    if(std::strcmp(type,"trackbar")==0) {
        return WS_CHILD|WS_TABSTOP|(XmlAttributeInteger(spec,L"方向",0)!=0?TBS_VERT:0);
    }
    if(std::strcmp(type,"hscroll")==0)return WS_CHILD|SBS_HORZ;
    if(std::strcmp(type,"vscroll")==0)return WS_CHILD|SBS_VERT;
    if(std::strcmp(type,"date")==0) {
        DWORD style=WS_CHILD|WS_TABSTOP;
        if(XmlAttributeInteger(spec,L"附件类型",0)==1)style|=DTS_UPDOWN;
        return style;
    }
    if(std::strcmp(type,"month")==0) {
        DWORD style=WS_CHILD;
        if(XmlAttributeBoolean(spec,L"允许选择多天",false))style|=MCS_MULTISELECT;
        if(XmlAttributeBoolean(spec,L"不显示今天",false))style|=MCS_NOTODAY;
        if(XmlAttributeBoolean(spec,L"不圈注今天",false))style|=MCS_NOTODAYCIRCLE;
        if(XmlAttributeBoolean(spec,L"显示星期序号",false))style|=MCS_WEEKNUMBERS;
        return style;
    }
    if(std::strcmp(type,"label")==0||std::strcmp(type,"canvas")==0) {
        DWORD style=WS_CHILD|SS_NOTIFY;
        const int horizontal=XmlAttributeInteger(spec,L"横向对齐方式",0);
        style|=horizontal==1?SS_CENTER:(horizontal==2?SS_RIGHT:SS_LEFT);
        if(std::strcmp(type,"label")==0&&!XmlAttributeBoolean(spec,L"是否自动折行",false))style|=SS_LEFTNOWORDWRAP;
        const int vertical=XmlAttributeInteger(spec,L"纵向对齐方式",0);
        if(vertical==1)style|=SS_CENTERIMAGE;
        return style;
    }
    if(std::strcmp(type,"shape")==0||std::strcmp(type,"image")==0)return WS_CHILD|SS_NOTIFY;
    if(std::strcmp(type,"animate")==0) {
        DWORD style=WS_CHILD|ACS_AUTOPLAY;
        if(XmlAttributeBoolean(spec,L"居中播放",false))style|=ACS_CENTER;
        if(XmlAttributeBoolean(spec,L"透明背景",false))style|=ACS_TRANSPARENT;
        return style;
    }
    if(std::strcmp(type,"container")==0)return WS_CHILD|WS_CLIPSIBLINGS|WS_CLIPCHILDREN;
    if(std::strcmp(type,"unsupported")==0)return 0;
    // Never manufacture a style for an unknown token.  The host-side model
    // only emits the explicitly mapped native controls above.
    return 0;
}
static const wchar_t* ClassName(const char* type) {
    if(strcmp(type,"button")==0||strcmp(type,"checkbox")==0||strcmp(type,"radio")==0||strcmp(type,"group")==0)return L"BUTTON";
    if(strcmp(type,"edit")==0)return L"EDIT";
    if(strcmp(type,"list")==0||strcmp(type,"checklist")==0)return L"LISTBOX";
    if(strcmp(type,"combo")==0)return L"COMBOBOX";
    if(strcmp(type,"drive")==0)return L"COMBOBOX";
    if(strcmp(type,"directory")==0||strcmp(type,"file")==0)return L"LISTBOX";
    if(strcmp(type,"color")==0)return L"BUTTON";
    if(strcmp(type,"hyperlink")==0)return L"STATIC";
    if(strcmp(type,"spin")==0)return UPDOWN_CLASSW;
    if(strcmp(type,"tab")==0)return L"SysTabControl32";
    if(strcmp(type,"progress")==0)return L"msctls_progress32";
    if(strcmp(type,"trackbar")==0)return L"msctls_trackbar32";
    if(strcmp(type,"hscroll")==0||strcmp(type,"vscroll")==0)return L"SCROLLBAR";
    if(strcmp(type,"date")==0)return L"SysDateTimePick32";
    if(strcmp(type,"month")==0)return L"SysMonthCal32";
    if(strcmp(type,"animate")==0)return ANIMATE_CLASSW;
    if(strcmp(type,"image")==0)return L"STATIC";
    if(strcmp(type,"container")==0)return L"ecompiler_window_container";
    if(strcmp(type,"shape")==0||strcmp(type,"canvas")==0)return L"STATIC";
    return nullptr;
}
static HWND EditPart(HWND window);
static void UpdateTabVisibility(unsigned int tabId) {
    const Unit* tab=FindUnit(tabId);
    if(tab==nullptr)return;
    const int page=static_cast<int>(SendMessageW(tab->hwnd,TCM_GETCURSEL,0,0));
    for(auto& item:units)if(item.tabOwner==static_cast<int>(tabId))ShowWindow(item.hwnd,item.tabPage==page?SW_SHOW:SW_HIDE);
}
static void SetWindowZOrder(HWND window,int order) {
    if(window==nullptr)return;
    HWND insertAfter=HWND_TOP;
    UINT flags=SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE;
    switch(order) {
    case 2: insertAfter=HWND_BOTTOM; break;
    case 3: insertAfter=HWND_TOPMOST; break;
    case 4: insertAfter=HWND_NOTOPMOST; break;
    default: break;
    }
    SetWindowPos(window,insertAfter,0,0,0,0,flags);
}
static void ApplyUnitFont(HWND window,Unit& unit) {
    if(window==nullptr)return;
    LOGFONTW logFont{};logFont.lfCharSet=DEFAULT_CHARSET;logFont.lfWeight=unit.fontBold?FW_BOLD:FW_NORMAL;logFont.lfItalic=unit.fontItalic?TRUE:FALSE;logFont.lfStrikeOut=unit.fontStrikeOut?TRUE:FALSE;logFont.lfUnderline=unit.fontUnderline?TRUE:FALSE;
    std::wstring name=unit.fontName.empty()?L"SimSun":unit.fontName;wcsncpy_s(logFont.lfFaceName,std::size(logFont.lfFaceName),name.c_str(),_TRUNCATE);
    HDC dc=GetDC(window);const int dpi=dc==nullptr?96:GetDeviceCaps(dc,LOGPIXELSY);if(dc!=nullptr)ReleaseDC(window,dc);logFont.lfHeight=-MulDiv((std::max)(1,unit.fontSize),dpi,72);
    HFONT font=CreateFontIndirectW(&logFont);if(font==nullptr)return;if(unit.font!=nullptr)DeleteObject(unit.font);unit.font=font;SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
}
static LRESULT CALLBACK UnitSubclassProc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
static HWND EditSpinBuddy(HWND edit) {
    HWND parent=edit==nullptr?nullptr:GetParent(edit); if(parent==nullptr)return nullptr;
    for(HWND child=FindWindowExW(parent,nullptr,UPDOWN_CLASSW,nullptr);child!=nullptr;child=FindWindowExW(parent,child,UPDOWN_CLASSW,nullptr))
        if(reinterpret_cast<HWND>(SendMessageW(child,UDM_GETBUDDY,0,0))==edit)return child;
    return nullptr;
}
static void ConfigureEditSpin(HWND edit,Unit& unit,bool visible,bool disabled) {
    if(edit==nullptr)return;
    HWND spin=EditSpinBuddy(edit);
    if(unit.spinMode==0) { if(spin!=nullptr)DestroyWindow(spin); return; }
    if(spin==nullptr) {
        DWORD style=WS_CHILD|(visible?WS_VISIBLE:0)|UDS_ARROWKEYS|UDS_ALIGNRIGHT;
        if(unit.spinMode==1)style|=UDS_SETBUDDYINT;
        spin=CreateWindowExW(0,UPDOWN_CLASSW,L"",style,0,0,0,0,GetParent(edit),nullptr,instance,nullptr);
        if(spin==nullptr)return;
        SendMessageW(spin,UDM_SETBUDDY,0,reinterpret_cast<LPARAM>(edit));
        SetWindowSubclass(spin,UnitSubclassProc,static_cast<UINT_PTR>(unit.id),0);
    } else {
        LONG_PTR style=GetWindowLongPtrW(spin,GWL_STYLE); if(unit.spinMode==1)style|=UDS_SETBUDDYINT;else style&=~UDS_SETBUDDYINT;SetWindowLongPtrW(spin,GWL_STYLE,style);
    }
    SendMessageW(spin,UDM_SETRANGE32,unit.spinMin,unit.spinMax);
    EnableWindow(spin,disabled?FALSE:TRUE); ShowWindow(spin,visible?SW_SHOW:SW_HIDE);
    RECT rect{};GetWindowRect(edit,&rect);MapWindowPoints(nullptr,GetParent(edit),reinterpret_cast<POINT*>(&rect),2);
    const int spinWidth=GetSystemMetrics(SM_CXVSCROLL);const int width=rect.right-rect.left;
    if(width>spinWidth) { SetWindowPos(edit,nullptr,rect.left,rect.top,width-spinWidth,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);rect.right-=spinWidth; }
    SetWindowPos(spin,nullptr,rect.right,rect.top,spinWidth,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);
}
static void SetWindowBorder(HWND window,int border) {
    if(window==nullptr)return;LONG_PTR ex=GetWindowLongPtrW(window,GWL_EXSTYLE);ex&=~(WS_EX_CLIENTEDGE|WS_EX_STATICEDGE);LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);style&=~WS_BORDER;
    switch(border){case 1:case 3:ex|=WS_EX_CLIENTEDGE;break;case 2:case 4:ex|=WS_EX_STATICEDGE;break;case 5:style|=WS_BORDER;break;default:break;}
    SetWindowLongPtrW(window,GWL_EXSTYLE,ex);SetWindowLongPtrW(window,GWL_STYLE,style);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
}
static void ApplyAttributes(HWND window,const Spec& spec) {
    if(window==nullptr)return;
    if(Unit* unit=UnitFromWindow(window);unit!=nullptr) {
        unit->checkOnlyOne=XmlAttributeBoolean(spec,L"单选",unit->checkOnlyOne);
        unit->driveType=XmlAttributeInteger(spec,L"类型",unit->driveType);
        unit->allowNormal=XmlAttributeBoolean(spec,L"通常",unit->allowNormal);
        unit->allowArchive=XmlAttributeBoolean(spec,L"存档",unit->allowArchive);
        unit->allowReadOnly=XmlAttributeBoolean(spec,L"只读",unit->allowReadOnly);
        unit->allowSystem=XmlAttributeBoolean(spec,L"系统",unit->allowSystem);
        unit->allowHidden=XmlAttributeBoolean(spec,L"隐藏",unit->allowHidden);
        unit->fileAllowMultiple=XmlAttributeBoolean(spec,L"允许选择多项",unit->fileAllowMultiple);
        unit->allowTrack=XmlAttributeBoolean(spec,L"允许拖动跟踪",unit->allowTrack);
        unit->dateAllowEdit=XmlAttributeBoolean(spec,L"允许编辑",unit->dateAllowEdit);
        unit->tabHeaderWay=XmlAttributeInteger(spec,L"表头方向",unit->tabHeaderWay);
        unit->tabMultiLine=XmlAttributeBoolean(spec,L"允许多行表头",unit->tabMultiLine);
        unit->tabFillBack=XmlAttributeBoolean(spec,L"是否填充背景",unit->tabFillBack);
        unit->tabBackColor=static_cast<COLORREF>(XmlAttributeInteger(spec,L"背景颜色",static_cast<int>(unit->tabBackColor)));
        unit->progressDrawMode=XmlAttributeInteger(spec,L"显示方式",unit->progressDrawMode);
        unit->trackTickStyle=XmlAttributeInteger(spec,L"刻度类型",unit->trackTickStyle);
        unit->trackTickFreq=XmlAttributeInteger(spec,L"单位刻度值",unit->trackTickFreq);
        unit->trackAllowSel=XmlAttributeBoolean(spec,L"允许选择",unit->trackAllowSel);
        unit->removeDuplicates=XmlAttributeBoolean(spec,L"除去重复",unit->removeDuplicates);
        unit->drawUnit=XmlAttributeInteger(spec,L"绘画单位",unit->drawUnit);
        unit->hideSelection=XmlAttributeBoolean(spec,L"隐藏选择",unit->hideSelection);
        unit->inputMode=XmlAttributeInteger(spec,L"输入方式",unit->inputMode);
        unit->convertMode=XmlAttributeInteger(spec,L"转换方式",unit->convertMode);
        unit->spinMode=XmlAttributeInteger(spec,L"调节器方式",unit->spinMode);
        unit->spinMin=XmlAttributeInteger(spec,L"调节器底限值",unit->spinMin);
        unit->spinMax=XmlAttributeInteger(spec,L"调节器上限值",unit->spinMax);
        unit->borderStyle=XmlAttributeInteger(spec,L"边框",unit->borderStyle);
        unit->horizontalAlign=XmlAttributeInteger(spec,L"横向对齐方式",unit->horizontalAlign);
        unit->verticalAlign=XmlAttributeInteger(spec,L"纵向对齐方式",unit->verticalAlign);
        unit->shape=XmlAttributeInteger(spec,L"外形",unit->shape);
        unit->shapeEffect=XmlAttributeInteger(spec,L"线条效果",unit->shapeEffect);
        unit->lineStyle=XmlAttributeInteger(spec,L"线型",unit->lineStyle);
        unit->lineWidth=XmlAttributeInteger(spec,L"线宽",unit->lineWidth);
        unit->penStyle=XmlAttributeInteger(spec,L"画笔类型",unit->penStyle);
        unit->drawRop2=XmlAttributeInteger(spec,L"画出方式",unit->drawRop2);
        unit->penWidth=XmlAttributeInteger(spec,L"画笔粗细",unit->penWidth);
        unit->brushStyle=XmlAttributeInteger(spec,L"刷子类型",unit->brushStyle);
        unit->autoRedraw=XmlAttributeBoolean(spec,L"自动重画",unit->autoRedraw);
        unit->backPicMode=XmlAttributeInteger(spec,L"底图方式",unit->backPicMode);
        unit->imageDrawMode=XmlAttributeInteger(spec,L"显示方式",unit->imageDrawMode);
        unit->playImage=XmlAttributeBoolean(spec,L"播放动画",unit->playImage);
        unit->imageFileName=Wide(XmlAttributeValue(spec,L"文件名") == nullptr ? "" : XmlAttributeValue(spec,L"文件名"));
        unit->imageCenter=XmlAttributeBoolean(spec,L"居中播放",unit->imageCenter);
        unit->imageTransparent=XmlAttributeBoolean(spec,L"透明背景",unit->imageTransparent);
        unit->imagePlayCount=XmlAttributeInteger(spec,L"播放次数",unit->imagePlayCount);
        unit->labelEffect=XmlAttributeInteger(spec,L"效果",unit->labelEffect);
        unit->gradientBorderWidth=XmlAttributeInteger(spec,L"渐变边框宽度",unit->gradientBorderWidth);
        for(int index=0;index<3;++index) {
            std::wstring name=L"渐变边框颜色"+std::to_wstring(index+1);
            const char* color=XmlAttributeValue(spec,name.c_str());
            if(color!=nullptr&&std::strcmp(color,"#透明")!=0){unit->gradientBorderColor[index]=static_cast<COLORREF>(std::strtol(color,nullptr,10));unit->gradientBorderColorSet[index]=1;}
            name=L"渐变背景颜色"+std::to_wstring(index+1); color=XmlAttributeValue(spec,name.c_str());
            if(color!=nullptr&&std::strcmp(color,"#透明")!=0){unit->gradientBackColor[index]=static_cast<COLORREF>(std::strtol(color,nullptr,10));unit->gradientBackColorSet[index]=1;}
        }
        unit->gradientBackMode=XmlAttributeInteger(spec,L"渐变背景方式",unit->gradientBackMode);
        unit->labelWordWrap=XmlAttributeBoolean(spec,L"是否自动折行",unit->labelWordWrap);
        unit->visitedColor=static_cast<COLORREF>(XmlAttributeInteger(spec,L"访问后的颜色",static_cast<int>(unit->visitedColor)));
        unit->hotColor=static_cast<COLORREF>(XmlAttributeInteger(spec,L"热点颜色",static_cast<int>(unit->hotColor)));
        unit->enterToNext=XmlAttributeBoolean(spec,L"回车下移焦点",unit->enterToNext);
        unit->f1OpenHelp=XmlAttributeBoolean(spec,L"F1键打开帮助",unit->f1OpenHelp);
        unit->helpFileName=Wide(XmlAttributeValue(spec,L"帮助文件名") == nullptr ? "" : XmlAttributeValue(spec,L"帮助文件名"));
        unit->helpContext=XmlAttributeInteger(spec,L"帮助标志值",unit->helpContext);
        SYSTEMTIME minDate{},maxDate{};
        unit->hasMinDate=XmlAttributeSystemTime(spec,L"最小日期",minDate); if(unit->hasMinDate)unit->minDate=minDate;
        unit->hasMaxDate=XmlAttributeSystemTime(spec,L"最大日期",maxDate); if(unit->hasMaxDate)unit->maxDate=maxDate;
        if(std::strcmp(spec.type,"dialog")==0) {
            unit->dialogType=XmlAttributeInteger(spec,L"类型",0);
            unit->dialogFileName=Wide(XmlAttributeValue(spec,L"文件名") == nullptr ? "" : XmlAttributeValue(spec,L"文件名"));
            unit->dialogFilter=Wide(XmlAttributeValue(spec,L"过滤器") == nullptr ? "" : XmlAttributeValue(spec,L"过滤器"));
            unit->dialogInitialDir=Wide(XmlAttributeValue(spec,L"初始目录") == nullptr ? "" : XmlAttributeValue(spec,L"初始目录"));
            unit->dialogDefExt=Wide(XmlAttributeValue(spec,L"默认文件后缀") == nullptr ? "" : XmlAttributeValue(spec,L"默认文件后缀"));
            unit->dialogCaption=Wide(XmlAttributeValue(spec,L"标题") == nullptr ? "" : XmlAttributeValue(spec,L"标题"));
            unit->dialogFilterIndex=XmlAttributeInteger(spec,L"初始过滤器",0);
            unit->dialogCreatePrompt=XmlAttributeBoolean(spec,L"创建时提示",false);
            unit->dialogFileMustExist=XmlAttributeBoolean(spec,L"文件必须存在",true);
            unit->dialogOverwritePrompt=XmlAttributeBoolean(spec,L"文件覆盖提示",true);
            unit->dialogPathMustExist=XmlAttributeBoolean(spec,L"目录必须存在",true);
            unit->dialogNoChangeDir=XmlAttributeBoolean(spec,L"不改变目录",false);
            unit->dialogHelpCommand=XmlAttributeInteger(spec,L"帮助命令",0);
            unit->dialogHelpContext=XmlAttributeInteger(spec,L"帮助标志值",0);
            unit->dialogFontColor=static_cast<COLORREF>(XmlAttributeInteger(spec,L"字体颜色",static_cast<int>(unit->dialogFontColor)));
            unit->dialogFontBold=XmlAttributeBoolean(spec,L"加粗",false);unit->dialogFontItalic=XmlAttributeBoolean(spec,L"倾斜",false);unit->dialogFontStrikeOut=XmlAttributeBoolean(spec,L"删除线",false);unit->dialogFontUnderline=XmlAttributeBoolean(spec,L"下划线",false);
            const char* dialogFont=XmlAttributeValue(spec,L"字体名称");if(dialogFont!=nullptr)unit->dialogFontName=Wide(dialogFont);unit->dialogFontSize=XmlAttributeInteger(spec,L"字体大小",0);
        }
        const char* textColor=XmlAttributeValue(spec,L"文本颜色");
        if(textColor!=nullptr) { unit->textColor=static_cast<COLORREF>(std::strtol(textColor,nullptr,10)); unit->hasTextColor=true; }
        const char* backColor=XmlAttributeValue(spec,L"背景颜色");
        if(backColor==nullptr) backColor=XmlAttributeValue(spec,L"画板背景色");
        if(backColor!=nullptr) {
            const long value=std::strtol(backColor,nullptr,10);
            if(value!=-16777216L) { unit->backColor=static_cast<COLORREF>(value); unit->hasBackColor=true; }
        }
        const char* lineColor=XmlAttributeValue(spec,L"线条颜色");
        if(lineColor!=nullptr) { unit->lineColor=static_cast<COLORREF>(std::strtol(lineColor,nullptr,10)); unit->hasLineColor=true; }
        const char* fillColor=XmlAttributeValue(spec,L"填充颜色");
        if(fillColor!=nullptr&&std::strcmp(fillColor,"#透明")!=0) { unit->fillColor=static_cast<COLORREF>(std::strtol(fillColor,nullptr,10)); unit->hasFillColor=true; }
        const char* penColor=XmlAttributeValue(spec,L"画笔颜色");
        if(penColor!=nullptr) unit->penColor=static_cast<COLORREF>(std::strtol(penColor,nullptr,10));
        const char* brushColor=XmlAttributeValue(spec,L"刷子颜色");
        if(brushColor!=nullptr) unit->brushColor=static_cast<COLORREF>(std::strtol(brushColor,nullptr,10));
        const char* textBackColor=XmlAttributeValue(spec,L"文本背景颜色");
        if(textBackColor!=nullptr&&std::strcmp(textBackColor,"#透明")!=0) unit->textBackColor=static_cast<COLORREF>(std::strtol(textBackColor,nullptr,10));
        const char* fontName=XmlAttributeValue(spec,L"字体名称");if(fontName!=nullptr&&*fontName!=0)unit->fontName=Wide(fontName);
        unit->fontSize=XmlAttributeInteger(spec,L"字体大小",unit->fontSize);
        unit->fontBold=XmlAttributeBoolean(spec,L"加粗",unit->fontBold);
        unit->fontItalic=XmlAttributeBoolean(spec,L"倾斜",unit->fontItalic);
        unit->fontStrikeOut=XmlAttributeBoolean(spec,L"删除线",unit->fontStrikeOut);
        unit->fontUnderline=XmlAttributeBoolean(spec,L"下划线",unit->fontUnderline);
        if(fontName!=nullptr||XmlAttributeValue(spec,L"字体大小")!=nullptr||XmlAttributeValue(spec,L"加粗")!=nullptr||XmlAttributeValue(spec,L"倾斜")!=nullptr||XmlAttributeValue(spec,L"删除线")!=nullptr||XmlAttributeValue(spec,L"下划线")!=nullptr)ApplyUnitFont(window,*unit);
        if(std::strcmp(spec.type,"shape")==0) {
            unit->shape=XmlAttributeInteger(spec,L"外形",unit->shape);unit->shapeEffect=XmlAttributeInteger(spec,L"线条效果",unit->shapeEffect);unit->lineStyle=XmlAttributeInteger(spec,L"线型",unit->lineStyle);unit->lineWidth=(std::max)(1,XmlAttributeInteger(spec,L"线宽",unit->lineWidth));
        }
        const char* image=XmlAttributeValue(spec,L"图片");if(image!=nullptr)unit->imageData=DecodeBase64(image);
        const char* cursorData=XmlAttributeValue(spec,L"鼠标指针");
        if(cursorData!=nullptr)ReplaceUnitCursor(*unit,DecodeBase64(cursorData));
        if(std::strcmp(spec.type,"animate")==0&&unit->imageFileName.size()>0) {
            Animate_Open(window,unit->imageFileName.c_str());
            if(unit->playImage)Animate_Play(window,0,-1,unit->imagePlayCount<0?static_cast<UINT>(-1):static_cast<UINT>(unit->imagePlayCount));else Animate_Stop(window);
        }
    }
    if(XmlAttributeBoolean(spec,L"选中",false))SendMessageW(window,BM_SETCHECK,BST_CHECKED,0);
    const int limit=XmlAttributeInteger(spec,L"最大文本长度",XmlAttributeInteger(spec,L"最大允许长度",0));
    if(limit>0&&(std::strcmp(spec.type,"edit")==0||std::strcmp(spec.type,"combo")==0)) {
        HWND edit=EditPart(window);SendMessageW(edit!=nullptr?edit:window,EM_SETLIMITTEXT,static_cast<WPARAM>(limit),0);
    }
    if(std::strcmp(spec.type,"edit")==0) {
        const int inputMode=XmlAttributeInteger(spec,L"输入方式",0);
        if(inputMode==2) {
            const std::wstring password=Wide(XmlAttributeValue(spec,L"密码遮盖字符") == nullptr ? "*" : XmlAttributeValue(spec,L"密码遮盖字符"));
            SendMessageW(window,EM_SETPASSWORDCHAR,password.empty()?L'*':password.front(),0);
        }
        if(inputMode==1)SendMessageW(window,EM_SETREADONLY,TRUE,0);
        if(Unit* unit=UnitFromWindow(window)) {
            unit->hideSelection=XmlAttributeBoolean(spec,L"隐藏选择",unit->hideSelection);
            unit->convertMode=XmlAttributeInteger(spec,L"转换方式",unit->convertMode);
            unit->spinMode=XmlAttributeInteger(spec,L"调节器方式",unit->spinMode);
            unit->spinMin=XmlAttributeInteger(spec,L"调节器底限值",unit->spinMin);
            unit->spinMax=XmlAttributeInteger(spec,L"调节器上限值",unit->spinMax);
            const char* password=XmlAttributeValue(spec,L"密码遮盖字符");if(password!=nullptr)unit->passwordChar=Wide(password);
        }
        SetWindowBorder(window,XmlAttributeInteger(spec,L"边框",1));
    }
    // 易语言编辑框的自动/手动调节器是一个隐藏在编辑框右侧的原生
    // UPDOWN buddy，而不是编辑框自身的窗口样式。创建后通过 UDM_SETBUDDY
    // 绑定，通知路由再把 UDN_DELTAPOS 转发给编辑框事件。
    if(std::strcmp(spec.type,"edit")==0) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->spinMode!=0&&
           (GetWindowLongPtrW(window,GWL_STYLE)&ES_MULTILINE)==0) {
            DWORD spinStyle=WS_CHILD|(spec.visible?WS_VISIBLE:0)|UDS_ARROWKEYS|UDS_ALIGNRIGHT;
            if(unit->spinMode==1)spinStyle|=UDS_SETBUDDYINT;
            HWND spin=CreateWindowExW(0,UPDOWN_CLASSW,L"",spinStyle,
                0,0,0,0,GetParent(window),nullptr,instance,nullptr);
            if(spin!=nullptr) {
                SendMessageW(spin,UDM_SETBUDDY,0,reinterpret_cast<LPARAM>(window));
                SendMessageW(spin,UDM_SETRANGE32,unit->spinMin,unit->spinMax);
                SetWindowSubclass(spin,UnitSubclassProc,static_cast<UINT_PTR>(spec.id),0);
                RECT editRect{};GetWindowRect(window,&editRect);MapWindowPoints(nullptr,GetParent(window),reinterpret_cast<POINT*>(&editRect),2);
                const int spinWidth=GetSystemMetrics(SM_CXVSCROLL);
                const int editWidth=editRect.right-editRect.left;
                if(editWidth>spinWidth) {
                    SetWindowPos(window,nullptr,editRect.left,editRect.top,editWidth-spinWidth,
                        editRect.bottom-editRect.top,SWP_NOZORDER|SWP_NOACTIVATE);
                    editRect.right-=spinWidth;
                }
                SetWindowPos(spin,nullptr,editRect.right,editRect.top,spinWidth,
                    editRect.bottom-editRect.top,SWP_NOZORDER|SWP_NOACTIVATE);
                if(spec.disabled)EnableWindow(spin,FALSE);
            }
        }
    }
    if(std::strcmp(spec.type,"date")==0) {
        SYSTEMTIME value{};
        if(XmlAttributeSystemTime(spec,L"今天",value))SendMessageW(window,DTM_SETSYSTEMTIME,GDT_VALID,reinterpret_cast<LPARAM>(&value));
        SYSTEMTIME minimum{},maximum{};const bool hasMinimum=XmlAttributeSystemTime(spec,L"最小日期",minimum);const bool hasMaximum=XmlAttributeSystemTime(spec,L"最大日期",maximum);
        if(hasMinimum||hasMaximum)SetDateRange(window,hasMinimum?&minimum:nullptr,hasMaximum?&maximum:nullptr);
    }
    if(std::strcmp(spec.type,"month")==0) {
        SYSTEMTIME today{};
        if(XmlAttributeSystemTime(spec,L"今天",today))SendMessageW(window,MCM_SETTODAY,0,reinterpret_cast<LPARAM>(&today));
        SYSTEMTIME minimum{},maximum{};const bool hasMinimum=XmlAttributeSystemTime(spec,L"最小日期",minimum);const bool hasMaximum=XmlAttributeSystemTime(spec,L"最大日期",maximum);
        if(hasMinimum||hasMaximum){SYSTEMTIME range[2]{minimum,maximum};SendMessageW(window,MCM_SETRANGE,(hasMinimum?GDTR_MIN:0)|(hasMaximum?GDTR_MAX:0),reinterpret_cast<LPARAM>(range));}
        const int startOfWeek=XmlAttributeInteger(spec,L"开始星期首日",-1);if(startOfWeek>=0&&startOfWeek<=6)SendMessageW(window,MCM_SETFIRSTDAYOFWEEK,0,startOfWeek);
        const int maxSelection=XmlAttributeInteger(spec,L"最多选择天数",0);if(maxSelection>0)SendMessageW(window,MCM_SETMAXSELCOUNT,maxSelection,0);
        const int scrollRate=XmlAttributeInteger(spec,L"滚动月数",0);if(scrollRate>0)SendMessageW(window,MCM_SETMONTHDELTA,scrollRate,0);
        const wchar_t* colors[]={L"背景颜色",L"文本颜色",L"标题背景颜色",L"标题颜色",L"内背景颜色",L"非本月颜色"};
        if(Unit* unit=UnitFromWindow(window))for(int index=0;index<6;++index){const char* color=XmlAttributeValue(spec,colors[index]);if(color!=nullptr){unit->monthColors[index]=static_cast<COLORREF>(std::strtol(color,nullptr,10));unit->monthColorSet[index]=1;SendMessageW(window,MCM_SETCOLOR,index,unit->monthColors[index]);}}
    }
    if(std::strcmp(spec.type,"list")==0||std::strcmp(spec.type,"checklist")==0||std::strcmp(spec.type,"combo")==0) {
        const int extra=XmlAttributeInteger(spec,L"行间距",0);
        if(extra>0) {
            if(std::strcmp(spec.type,"list")==0||std::strcmp(spec.type,"checklist")==0) {
                const int height=static_cast<int>(SendMessageW(window,LB_GETITEMHEIGHT,0,0));
                if(height>0) {
                    const int count=static_cast<int>(SendMessageW(window,LB_GETCOUNT,0,0));
                    for(int index=0;index<count;++index)SendMessageW(window,LB_SETITEMHEIGHT,static_cast<WPARAM>(index),height+extra);
                }
            }
            else {
                const int height=static_cast<int>(SendMessageW(window,CB_GETITEMHEIGHT,0,0));
                if(height>0) {
                    SendMessageW(window,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),height+extra);
                    const int count=static_cast<int>(SendMessageW(window,CB_GETCOUNT,0,0));
                    for(int index=0;index<count;++index)SendMessageW(window,CB_SETITEMHEIGHT,static_cast<WPARAM>(index),height+extra);
                }
            }
        }
    }
    if(std::strcmp(spec.type,"drive")==0) {
        PopulateDriveList(window,UnitFromWindow(window));
        const char* value=XmlAttributeValue(spec,L"驱动器");
        if(value!=nullptr&&*value!=0) {
            std::wstring drive=Wide(value); if(drive.size()==1)drive+=L":"; if(drive.size()==2)drive+=L"\\";
            const LRESULT index=SendMessageW(window,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(drive.c_str()));
            if(index>=0)SendMessageW(window,CB_SETCURSEL,index,0);
        }
        if(SendMessageW(window,CB_GETCURSEL,0,0)<0&&SendMessageW(window,CB_GETCOUNT,0,0)>0)SendMessageW(window,CB_SETCURSEL,0,0);
    }
    if(std::strcmp(spec.type,"directory")==0||std::strcmp(spec.type,"file")==0) {
        if(Unit* unit=UnitFromWindow(window))PopulatePathList(window,spec,*unit,std::strcmp(spec.type,"directory")==0);
    }
    if(std::strcmp(spec.type,"color")==0) {
        if(Unit* unit=UnitFromWindow(window)) {
            const char* value=XmlAttributeValue(spec,L"颜色");
            if(value!=nullptr&&*value!=0&&std::strcmp(value,"#透明")!=0) { unit->color=static_cast<COLORREF>(std::strtol(value,nullptr,10));unit->hasColor=true; }
        }
    }
    if(std::strcmp(spec.type,"hyperlink")==0) {
        if(Unit* unit=UnitFromWindow(window)) {
            unit->hyperlinkType=XmlAttributeInteger(spec,L"类型",0);
            const wchar_t* property=unit->hyperlinkType==0?L"电子信箱地址":L"Internet地址";
            const char* value=XmlAttributeValue(spec,property);
            unit->hyperlinkTarget=Wide(value==nullptr?"":value);
        }
    }
    if(std::strcmp(spec.type,"spin")==0) {
        const int minimum=XmlAttributeInteger(spec,L"调节器底限值",0);
        const int maximum=XmlAttributeInteger(spec,L"调节器上限值",100);
        SendMessageW(window,UDM_SETRANGE32,minimum,maximum);
        const int position=XmlAttributeInteger(spec,L"位置",minimum);
        SendMessageW(window,UDM_SETPOS32,0,position);
    }
    if(std::strcmp(spec.type,"progress")==0) {
        const int minimum=XmlAttributeInteger(spec,L"最小位置",0);
        const int maximum=XmlAttributeInteger(spec,L"最大位置",100);
        const int position=XmlAttributeInteger(spec,L"位置",minimum);
        SendMessageW(window,PBM_SETRANGE32,minimum,maximum);
        SendMessageW(window,PBM_SETPOS,position,0);
        LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);
        if(XmlAttributeInteger(spec,L"显示方式",0)==1)style|=PBS_SMOOTH;else style&=~PBS_SMOOTH;
        SetWindowLongPtrW(window,GWL_STYLE,style);
    }
    if(std::strcmp(spec.type,"trackbar")==0) {
        const int minimum=XmlAttributeInteger(spec,L"最小位置",0);
        const int maximum=XmlAttributeInteger(spec,L"最大位置",100);
        const int position=XmlAttributeInteger(spec,L"位置",minimum);
        SendMessageW(window,TBM_SETRANGE,TRUE,MAKELONG(minimum,maximum));
        SendMessageW(window,TBM_SETPAGESIZE,0,XmlAttributeInteger(spec,L"页改变值",5));
        SendMessageW(window,TBM_SETLINESIZE,0,XmlAttributeInteger(spec,L"行改变值",1));
        const int tickFrequency=XmlAttributeInteger(spec,L"单位刻度值",0);
        if(tickFrequency>0)SendMessageW(window,TBM_SETTICFREQ,tickFrequency,0);
        if(XmlAttributeBoolean(spec,L"允许选择",false)) {
            const int selectionStart=XmlAttributeInteger(spec,L"首选择位置",minimum);
            const int selectionLength=XmlAttributeInteger(spec,L"选择长度",0);
            SendMessageW(window,TBM_SETSELSTART,TRUE,selectionStart);
            SendMessageW(window,TBM_SETSELEND,TRUE,selectionStart+selectionLength);
        }
        SendMessageW(window,TBM_SETPOS,TRUE,position);
        LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); style&=~(TBS_AUTOTICKS|TBS_NOTICKS|TBS_TOP|TBS_BOTTOM|TBS_BOTH);
        const int tickStyle=XmlAttributeInteger(spec,L"刻度类型",0);
        if(tickStyle==0)style|=TBS_NOTICKS; else if(tickStyle==1)style|=TBS_TOP; else if(tickStyle==2)style|=TBS_BOTTOM; else style|=TBS_BOTH;
        SetWindowLongPtrW(window,GWL_STYLE,style); SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
    }
    if(std::strcmp(spec.type,"hscroll")==0||std::strcmp(spec.type,"vscroll")==0) {
        if(Unit* unit=UnitFromWindow(window)){unit->scrollMin=XmlAttributeInteger(spec,L"最小位置",0);unit->scrollMax=XmlAttributeInteger(spec,L"最大位置",100);unit->scrollPage=XmlAttributeInteger(spec,L"页改变值",10);unit->scrollLine=XmlAttributeInteger(spec,L"行改变值",1);}
        const Unit* unit=UnitFromWindow(window);
        SCROLLINFO info{sizeof(SCROLLINFO),SIF_RANGE|SIF_PAGE|SIF_POS,
            unit==nullptr?0:unit->scrollMin,unit==nullptr?100:unit->scrollMax,
            static_cast<UINT>(unit==nullptr?10:unit->scrollPage),
            XmlAttributeInteger(spec,L"位置",0),0};
        SetScrollInfo(window,SB_CTL,&info,TRUE);
    }
    if(std::strcmp(spec.type,"date")==0) {
        if(Unit* unit=UnitFromWindow(window)) {
            unit->dateAllowEdit=XmlAttributeBoolean(spec,L"允许编辑",unit->dateAllowEdit);
            if(HWND child=FindWindowExW(window,nullptr,L"Edit",nullptr))SendMessageW(child,EM_SETREADONLY,unit->dateAllowEdit?FALSE:TRUE,0);
            SetWindowBorder(window,XmlAttributeInteger(spec,L"边框",unit->borderStyle));
        }
    }
}
static void ApplyStructuredData(HWND window,const Spec& spec) {
    if(window==nullptr)return;
    if(std::strcmp(spec.type,"list")==0||std::strcmp(spec.type,"checklist")==0) {
        std::vector<LRESULT> actualIndexes;
        actualIndexes.reserve(spec.itemCount);
        for(std::size_t index=0;index<spec.itemCount;++index) {
            if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->removeDuplicates&&SendMessageW(window,LB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(Wide(spec.items[index]).c_str()))>=0) { actualIndexes.push_back(-1); continue; }
            const LRESULT item=SendMessageW(window,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(Wide(spec.items[index]).c_str()));
            actualIndexes.push_back(item);
            if(index<spec.itemValueCount&&item>=0)SendMessageW(window,LB_SETITEMDATA,item,spec.itemValues[index]);
        }
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(spec.type,"checklist")==0) {
            EnsureCheckState(*unit,spec.itemCount);
            for(std::size_t index=0;index<spec.itemCount;++index) {
                const LRESULT item=index<actualIndexes.size()?actualIndexes[index]:-1;
                if(item<0)continue;
                const std::size_t slot=static_cast<std::size_t>(item);
                if(slot>=unit->checkStates.size())continue;
                if(index<spec.itemCheckedCount)unit->checkStates[slot]=spec.itemChecked[index]?1:0;
                if(index<spec.itemEnabledCount)unit->checkEnabled[slot]=spec.itemEnabled[index]?1:0;
            }
            InvalidateRect(window,nullptr,TRUE);
        }
        const int selected=XmlAttributeInteger(spec,L"现行选中项",-1);
        if(selected>=0)SendMessageW(window,LB_SETCURSEL,selected,0);
    }
    else if(std::strcmp(spec.type,"combo")==0) {
        for(std::size_t index=0;index<spec.itemCount;++index) {
            if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->removeDuplicates&&SendMessageW(window,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(Wide(spec.items[index]).c_str()))>=0)continue;
            const LRESULT item=SendMessageW(window,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(Wide(spec.items[index]).c_str()));
            if(index<spec.itemValueCount&&item>=0)SendMessageW(window,CB_SETITEMDATA,item,spec.itemValues[index]);
        }
        const int selected=XmlAttributeInteger(spec,L"现行选中项",-1);
        if(selected>=0)SendMessageW(window,CB_SETCURSEL,selected,0);
    }
}
static void StoreSpecProperties(std::unordered_map<std::string,Value>& store,const XmlAttribute* attributes,std::size_t count);
static void CreateUnit(const Spec& spec) {
    if(strcmp(spec.type,"unsupported")==0)return;
    HWND parent=nullptr;
    if(spec.parent==0)parent=FindForm(spec.form);
    else parent=FindUnit(spec.parent)==nullptr?nullptr:FindUnit(spec.parent)->hwnd;
    if(parent==nullptr)return;
    DWORD style=Style(spec);
    const wchar_t* className=ClassName(spec.type);
    if(style==0||className==nullptr)return;
    if(!spec.tabStop)style&=~WS_TABSTOP;
    if(spec.visible)style|=WS_VISIBLE;
    const DWORD exStyle=strcmp(spec.type,"container")==0?WS_EX_TRANSPARENT:0;
    HWND window=CreateWindowExW(exStyle,className,Wide(spec.text).c_str(),style,
        spec.left,spec.top,spec.width,spec.height,parent,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(spec.id)),instance,nullptr);
    if(window==nullptr)return;
    ApplyDefaultFont(window);
    SetWindowSubclass(window,UnitSubclassProc,static_cast<UINT_PTR>(spec.id),0);
    Unit initialUnit;
    initialUnit.id=spec.id; initialUnit.form=spec.form; initialUnit.hwnd=window; initialUnit.parent=parent;
    initialUnit.tabOwner=spec.tabOwner; initialUnit.tabPage=spec.tabPage; initialUnit.type=spec.type;
    initialUnit.tag=Wide(XmlAttributeValue(spec,L"标记") == nullptr ? "" : XmlAttributeValue(spec,L"标记"));
    initialUnit.checkOnlyOne=XmlAttributeBoolean(spec,L"单选",false);
    initialUnit.removeDuplicates=XmlAttributeBoolean(spec,L"除去重复",false);
    units.push_back(std::move(initialUnit));
    if(Unit* stored=UnitFromWindow(window))StoreSpecProperties(stored->properties,spec.attributes,spec.attributeCount);
    ApplyStructuredData(window,spec);
    ApplyAttributes(window,spec);
    if(std::strcmp(spec.type,"timer")==0) {
        if(Unit* unit=UnitFromWindow(window)) {
            const int period=XmlAttributeInteger(spec,L"时钟周期",0);
            unit->timerPeriod=period>0?static_cast<UINT>(period):0;
            if(unit->timerPeriod!=0)SetTimer(window,spec.id,unit->timerPeriod,nullptr);
        }
        ShowWindow(window,SW_HIDE);
    }
    SetWindowPos(window,nullptr,spec.left,spec.top,spec.width,spec.height,SWP_NOZORDER|SWP_NOACTIVATE);
    if(!spec.visible)ShowWindow(window,SW_HIDE);
    if(spec.disabled)EnableWindow(window,FALSE);
    if(strcmp(spec.type,"tab")==0&&XmlAttributeBoolean(spec,L"隐藏自身",false))ShowWindow(window,SW_HIDE);
}
static bool ClassEquals(HWND window,const wchar_t* expected);
static HWND WindowById(unsigned int id);
static void SetText(HWND window,const Value& value);
static Value TextFromWide(const std::wstring& wide) {
    if(wide.empty())return Text("");
    const int bytes=WideCharToMultiByte(CP_ACP,0,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr);
    std::string result(static_cast<std::size_t>((std::max)(bytes,0)),'\0');
    if(bytes>0)WideCharToMultiByte(CP_ACP,0,wide.data(),static_cast<int>(wide.size()),result.data(),bytes,nullptr,nullptr);
    return Text(std::move(result));
}
static std::wstring WindowTextW(HWND window) {
    int length=GetWindowTextLengthW(window);
    std::wstring value(static_cast<std::size_t>((std::max)(length,0)+1),L'\0');
    if(length>0)GetWindowTextW(window,value.data(),length+1);
    value.resize(static_cast<std::size_t>((std::max)(length,0)));
    return value;
}
static Value WindowTextValue(HWND window) {
    return TextFromWide(WindowTextW(window));
}
static HWND EditPart(HWND window) {
    if(ClassEquals(window,L"EDIT"))return window;
    if(!ClassEquals(window,L"COMBOBOX"))return nullptr;
    return FindWindowExW(window,nullptr,L"EDIT",nullptr);
}
static int CurrentSelection(HWND window);
static bool WindowGeometry(HWND window,int& left,int& top,int& width,int& height);
static Value WindowInvokeMember(unsigned int id,const char* operation,std::vector<Value> args) {
    HWND window=WindowById(id);
    if(window==nullptr||operation==nullptr)return Empty();
    const bool combo=ClassEquals(window,L"COMBOBOX");
    const bool list=ClassEquals(window,L"LISTBOX");
    const bool tab=ClassEquals(window,L"SysTabControl32");
    const bool checklist=IsCheckList(window);
    const auto argInt=[&](std::size_t index,int fallback=0){return index<args.size()?static_cast<int>(ToInteger(args[index])):fallback;};
    const auto argText=[&](std::size_t index){return index<args.size()?Wide(ToText(args[index]).c_str()):std::wstring();};
    const auto argProvided=[&](std::size_t index){return index<args.size()&&!args[index].missing;};
    const auto listCount=[&](){return static_cast<int>(list||checklist?SendMessageW(window,LB_GETCOUNT,0,0):(combo?SendMessageW(window,CB_GETCOUNT,0,0):-1));};
    const auto ensureListState=[&](){if(checklist)if(Unit* unit=UnitFromWindow(window))EnsureCheckState(*unit,static_cast<std::size_t>((std::max)(listCount(),0)));};
    const auto insertListState=[&](int index){if(!checklist)return;Unit* unit=UnitFromWindow(window);if(unit==nullptr)return;const int count=listCount();const std::size_t previousCount=static_cast<std::size_t>((std::max)(0,count-1));if(unit->checkStates.size()!=previousCount)unit->checkStates.resize(previousCount,0);if(unit->checkEnabled.size()!=previousCount)unit->checkEnabled.resize(previousCount,1);const std::size_t slot=static_cast<std::size_t>((std::max)(0,(std::min)(index,static_cast<int>(previousCount))));unit->checkStates.insert(unit->checkStates.begin()+static_cast<std::ptrdiff_t>(slot),0);unit->checkEnabled.insert(unit->checkEnabled.begin()+static_cast<std::ptrdiff_t>(slot),1);};
    const auto eraseListState=[&](int index){if(!checklist)return;Unit* unit=UnitFromWindow(window);if(unit==nullptr)return;const int count=listCount();const std::size_t previousCount=static_cast<std::size_t>((std::max)(0,count+1));if(unit->checkStates.size()!=previousCount)unit->checkStates.resize(previousCount,0);if(unit->checkEnabled.size()!=previousCount)unit->checkEnabled.resize(previousCount,1);if(index>=0&&static_cast<std::size_t>(index)<unit->checkStates.size()){unit->checkStates.erase(unit->checkStates.begin()+index);unit->checkEnabled.erase(unit->checkEnabled.begin()+index);}};
    if(operation==std::string("GetHWnd"))return Integer(static_cast<long long>(reinterpret_cast<std::uintptr_t>(window)));
    if(operation==std::string("Destroy")) {
        const bool form=ClassEquals(window,L"ecompiler_window_form");
        if(form&&!argInt(0,0))SendMessageW(window,WM_CLOSE,0,0); else DestroyWindow(window);
        return Empty();
    }
    if(operation==std::string("SetFocus")) {
        HWND target=window;
        if(ClassEquals(window,L"ecompiler_window_form")) {
            HWND child=GetNextDlgTabItem(window,nullptr,FALSE);
            if(child!=nullptr)target=child;
        }
        ::SetFocus(target);
        return Empty();
    }
    if(operation==std::string("IsFocus")) {
        const HWND focus=GetFocus();
        return Boolean(focus==window||(ClassEquals(window,L"ecompiler_window_form")&&focus!=nullptr&&IsChild(window,focus)));
    }
    if(operation==std::string("GetClientWidth")||operation==std::string("GetClientHeight")) {
        RECT rect{};GetClientRect(window,&rect);
        return Integer(operation==std::string("GetClientWidth")?rect.right-rect.left:rect.bottom-rect.top);
    }
    if(operation==std::string("LockWindowUpdate")) { LockWindowUpdate(window);lockedWindow=window;return Empty(); }
    if(operation==std::string("UnlockWindowUpdate")) { if(lockedWindow==window||window==nullptr){LockWindowUpdate(nullptr);lockedWindow=nullptr;}return Empty(); }
    if(operation==std::string("Invalidate")) { InvalidateRect(window,nullptr,TRUE);return Empty(); }
    if(operation==std::string("InvalidateRect")) {
        RECT rect{argInt(0),argInt(1),argInt(0)+argInt(2),argInt(1)+argInt(3)};
        InvalidateRect(window,&rect,TRUE);return Empty();
    }
    if(operation==std::string("Validate")) { ValidateRect(window,nullptr);return Empty(); }
    if(operation==std::string("UpdateWindow")) { ::UpdateWindow(window);return Empty(); }
    if(operation==std::string("Move")) {
        int left=0,top=0,width=0,height=0;
        if(WindowGeometry(window,left,top,width,height)) {
            if(argProvided(0))left=argInt(0);
            if(argProvided(1))top=argInt(1);
            if(argProvided(2)&&argInt(2,-1)!=-1)width=(std::max)(0,argInt(2));
            if(argProvided(3)&&argInt(3,-1)!=-1)height=(std::max)(0,argInt(3));
            if(GetParent(window)==nullptr) {
                RECT client{0,0,width,height};
                AdjustWindowRectEx(&client,static_cast<DWORD>(GetWindowLongW(window,GWL_STYLE)),FALSE,static_cast<DWORD>(GetWindowLongW(window,GWL_EXSTYLE)));
                SetWindowPos(window,nullptr,left,top,client.right-client.left,client.bottom-client.top,SWP_NOZORDER|SWP_NOACTIVATE);
            } else SetWindowPos(window,nullptr,left,top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
        }
        return Empty();
    }
    if(operation==std::string("ZOrder")) { SetWindowZOrder(window,argInt(0,1));return Empty(); }
    if(operation==std::string("SendMessage"))return Integer(static_cast<long long>(SendMessageW(window,static_cast<UINT>(argInt(0)),static_cast<WPARAM>(argInt(1)),static_cast<LPARAM>(argInt(2)))));
    if(operation==std::string("PostMessage")) { PostMessageW(window,static_cast<UINT>(argInt(0)),static_cast<WPARAM>(argInt(1)),static_cast<LPARAM>(argInt(2)));return Empty(); }
    if(operation==std::string("PopupMenu")) { PopupMenuAt(OwnerForm(window)!=nullptr?OwnerForm(window)->hwnd:window,args.size()>0?&args[0]:nullptr,args.size()>1?&args[1]:nullptr,args.size()>2?&args[2]:nullptr);return Empty(); }
    if(operation==std::string("Activate")) { if(ClassEquals(window,L"ecompiler_window_form"))SetForegroundWindow(window);else ::SetFocus(window);return Empty(); }
    if(operation==std::string("SetTrayIcon")) { Form* form=OwnerForm(window); if(form!=nullptr){const std::wstring tip=args.size()>1?argText(1):std::wstring();(void)SetTrayIcon(*form,args.empty()?nullptr:&args[0],tip);}return Empty(); }
    if(operation==std::string("PopupTrayMenu")) { Form* form=OwnerForm(window);if(form!=nullptr)PopupMenuAt(form->hwnd,args.empty()?nullptr:&args[0],nullptr,nullptr);return Empty(); }
    if(operation==std::string("SetParentWnd")) {
        HWND parent=nullptr;
        if(!args.empty()&&!args[0].missing) {
            const unsigned int targetId=static_cast<unsigned int>(ToInteger(args[0]));
            parent=WindowById(targetId);
            if(parent==nullptr)parent=reinterpret_cast<HWND>(static_cast<std::uintptr_t>(ToInteger(args[0])));
        }
        if(parent!=nullptr&&parent!=window)SetParent(window,parent);
        return Empty();
    }
    if(operation==std::string("GetSpecTagUnit")) {
        const long long wanted=argInt(0);
        const unsigned int formId=UnitFromWindow(window)!=nullptr?UnitFromWindow(window)->form:0u;
        for(const auto& item:units) {
            if(formId!=0u&&item.form!=formId)continue;
            wchar_t* end=nullptr;const long long value=std::wcstoll(item.tag.c_str(),&end,10);
            if(end!=item.tag.c_str()&&*end==L'\0'&&value==wanted)return Integer(static_cast<long long>(item.id),T_WINDOW_UNIT);
        }
        return Empty();
    }
	if(operation==std::string("SetShapePic"))return Boolean(false);
	if(operation==std::string("OpenDialog")) {
		Unit* unit=UnitFromWindow(window);
		if(unit==nullptr||unit->type==nullptr||std::strcmp(unit->type,"dialog")!=0)return Boolean(false);
		HWND owner=GetParent(window);
		if(owner==nullptr)owner=window;
		return Boolean(OpenCommonDialog(owner,*unit));
	}
	if(operation==std::string("GetHDC")) {
		return Integer(static_cast<long long>(reinterpret_cast<std::uintptr_t>(CurrentPaintDC(window))));
	}
	if(operation==std::string("DrawPic")) {
		HDC dc=CurrentPaintDC(window);
		if(dc!=nullptr&&!args.empty()) {
			HBITMAP bitmap=nullptr;
			if(!args[0].bytes.empty()) bitmap=LoadPictureMemory(args[0].bytes);
			else if(args[0].type==T_INT||args[0].type==T_BOOL||args[0].type==T_FLOAT)
				bitmap=ClonePictureHandle(static_cast<OLE_HANDLE>(static_cast<std::uintptr_t>(ToInteger(args[0]))));
			if(bitmap!=nullptr)DrawBitmapAt(dc,bitmap,argInt(1),argInt(2),argInt(3),argInt(4));
		}
		return Empty();
	}
	if(operation==std::string("GetCanvasPic")) {
		HDC dc=CurrentPaintDC(window);if(dc==nullptr)return Bytes({});
		RECT rect{};GetClientRect(window,&rect);const int width=(std::max)(0,rect.right),height=(std::max)(0,rect.bottom);if(width==0||height==0)return Bytes({});
		HDC memory=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,width,height);if(memory==nullptr||bitmap==nullptr){if(memory!=nullptr)DeleteDC(memory);if(bitmap!=nullptr)DeleteObject(bitmap);return Bytes({});}
		HGDIOBJ old=SelectObject(memory,bitmap);BitBlt(memory,0,0,width,height,dc,0,0,SRCCOPY);SelectObject(memory,old);DeleteDC(memory);std::vector<unsigned char> bytes=BitmapToBytes(bitmap);DeleteObject(bitmap);return Bytes(std::move(bytes));
	}
	if(operation==std::string("CopyCanvas")) {
		HDC dc=CurrentPaintDC(window);if(dc!=nullptr){RECT client{};GetClientRect(window,&client);const int x=argInt(0),y=argInt(1),width=argProvided(2)?argInt(2):client.right-x,height=argProvided(3)?argInt(3):client.bottom-y;if(width>0&&height>0){HDC memory=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,width,height);if(memory!=nullptr&&bitmap!=nullptr){HGDIOBJ old=SelectObject(memory,bitmap);BitBlt(memory,0,0,width,height,dc,x,y,SRCCOPY);SelectObject(memory,old);DeleteDC(memory);if(OpenClipboard(window)!=FALSE){EmptyClipboard();SetClipboardData(CF_BITMAP,bitmap);CloseClipboard();}else DeleteObject(bitmap);}else{if(memory!=nullptr)DeleteDC(memory);if(bitmap!=nullptr)DeleteObject(bitmap);}}}
		return Empty();
	}
	if(operation==std::string("GetPicWidth")||operation==std::string("GetPicHeight")) {
		// The core's image number is an opaque handle.  Ask the classic runtime
		// for its bitmap and query the dimensions when that ABI is available.
		const HBITMAP bitmap=reinterpret_cast<HBITMAP>(static_cast<std::uintptr_t>(BlackMoonFuncForeLibNotifySys(1003,static_cast<EPointer>(argInt(0)),0)));
		if(bitmap==nullptr)return Integer(0);
		BITMAP info{};GetObjectW(bitmap,sizeof(info),&info);DeleteObject(bitmap);
		return Integer(operation==std::string("GetPicWidth")?info.bmWidth:info.bmHeight);
	}
	if(operation==std::string("UnitCnv")) {
		Unit* unit=UnitFromWindow(window);const int value=argInt(0);const int kind=argInt(1);if(unit==nullptr||kind<1||kind>4)return Integer(value);if(unit->drawUnit==0)return Integer(value);HDC dc=GetDC(window);if(dc==nullptr)return Integer(value);const int dpiX=GetDeviceCaps(dc,LOGPIXELSX),dpiY=GetDeviceCaps(dc,LOGPIXELSY);ReleaseDC(window,dc);const double scale=(kind==1||kind==3)?(unit->drawUnit==1?dpiX/254.0:(unit->drawUnit==2?dpiX/2540.0:(unit->drawUnit==3?dpiX/100.0:(unit->drawUnit==4?dpiX/1000.0:dpiX/1440.0)))):(unit->drawUnit==1?dpiY/254.0:(unit->drawUnit==2?dpiY/2540.0:(unit->drawUnit==3?dpiY/100.0:(unit->drawUnit==4?dpiY/1000.0:dpiY/1440.0))));const bool pixelToUnit=kind==3||kind==4;return Integer(static_cast<long long>(pixelToUnit?value/scale:value*scale));
	}
	if(operation==std::string("GetPixel")) {
		HDC dc=CurrentPaintDC(window); if(dc==nullptr)return Integer(-1);
		return Integer(static_cast<long long>(GetPixel(dc,argInt(0),argInt(1))));
	}
	if(operation==std::string("CanvasClear")) {
		if(HDC dc=CurrentPaintDC(window);dc!=nullptr){RECT client{};GetClientRect(window,&client);const int left=argProvided(0)?argInt(0):0;const int top=argProvided(1)?argInt(1):0;const int width=argProvided(2)?argInt(2):client.right-left;const int height=argProvided(3)?argInt(3):client.bottom-top;RECT rect{left,top,left+(std::max)(0,width),top+(std::max)(0,height)};Unit* unit=UnitFromWindow(window);HBRUSH brush=unit!=nullptr?CreateSolidBrush(unit->hasBackColor?unit->backColor:RGB(255,255,255)):static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));FillRect(dc,&rect,brush);if(unit!=nullptr)DeleteObject(brush);if(unit!=nullptr){unit->writeX=left;unit->writeY=top;}}
		return Empty();
	}
	if(operation==std::string("SetPixel")) {
		HDC dc=CurrentPaintDC(window); if(dc!=nullptr)SetPixel(dc,argInt(0),argInt(1),static_cast<COLORREF>(argInt(2)));
		return Empty();
	}
	if(operation==std::string("GradientRect")) {
		HDC dc=CurrentPaintDC(window);if(dc!=nullptr){const int left=argInt(0),top=argInt(1),width=(std::max)(0,argInt(2)),height=(std::max)(0,argInt(3));const int direction=argInt(4,2);const COLORREF first=static_cast<COLORREF>(argInt(5)),second=static_cast<COLORREF>(argInt(6));const int steps=(std::max)(1,(direction==1||direction==5)?height:width);for(int step=0;step<steps;++step){const double ratio=steps<=1?0.0:static_cast<double>(step)/static_cast<double>(steps-1);const BYTE r=static_cast<BYTE>(GetRValue(first)+(GetRValue(second)-GetRValue(first))*ratio);const BYTE g=static_cast<BYTE>(GetGValue(first)+(GetGValue(second)-GetGValue(first))*ratio);const BYTE b=static_cast<BYTE>(GetBValue(first)+(GetBValue(second)-GetBValue(first))*ratio);HBRUSH brush=CreateSolidBrush(RGB(r,g,b));RECT rect{left,top,left+width,top+height};if(direction==1||direction==5){rect.top=top+(height*step)/steps;rect.bottom=top+(height*(step+1))/steps;}else{rect.left=left+(width*step)/steps;rect.right=left+(width*(step+1))/steps;}FillRect(dc,&rect,brush);DeleteObject(brush);}}
		return Empty();
	}
	if(operation==std::string("LineTo")) {
		HDC dc=CurrentPaintDC(window); if(dc!=nullptr){CanvasPaintGuard guard(dc,UnitFromWindow(window));MoveToEx(dc,argInt(0),argInt(1),nullptr);::LineTo(dc,argInt(2),argInt(3));}
		return Empty();
	}
	if(operation==std::string("Ellipse")) {
		HDC dc=CurrentPaintDC(window); if(dc!=nullptr){CanvasPaintGuard guard(dc,UnitFromWindow(window));::Ellipse(dc,argInt(0),argInt(1),argInt(2),argInt(3));}
		return Empty();
	}
	if(operation==std::string("ArcTo")||operation==std::string("Chord")||operation==std::string("Pie")) {
		HDC dc=CurrentPaintDC(window); if(dc!=nullptr){CanvasPaintGuard guard(dc,UnitFromWindow(window));const int left=argInt(0),top=argInt(1),right=argInt(2),bottom=argInt(3),sx=argInt(4),sy=argInt(5),ex=argInt(6),ey=argInt(7);if(operation==std::string("ArcTo"))Arc(dc,left,top,right,bottom,sx,sy,ex,ey);else if(operation==std::string("Chord"))Chord(dc,left,top,right,bottom,sx,sy,ex,ey);else Pie(dc,left,top,right,bottom,sx,sy,ex,ey);}
		return Empty();
	}
	if(operation==std::string("DrawRect")) {
		HDC dc=CurrentPaintDC(window); if(dc!=nullptr){CanvasPaintGuard guard(dc,UnitFromWindow(window));::Rectangle(dc,argInt(0),argInt(1),argInt(2),argInt(3));}
		return Empty();
	}
	if(operation==std::string("FillRect")) {
		HDC dc=CurrentPaintDC(window); if(dc!=nullptr){RECT rect{argInt(0),argInt(1),argInt(2),argInt(3)};Unit* unit=UnitFromWindow(window);HBRUSH brush=unit!=nullptr?CreateSolidBrush(unit->hasBackColor?unit->backColor:RGB(255,255,255)):static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));::FillRect(dc,&rect,brush);if(unit!=nullptr)DeleteObject(brush);}
		return Empty();
	}
	if(operation==std::string("RoundRect")) {
		HDC dc=CurrentPaintDC(window); if(dc!=nullptr){CanvasPaintGuard guard(dc,UnitFromWindow(window));::RoundRect(dc,argInt(0),argInt(1),argInt(2),argInt(3),argInt(4),argInt(5,argInt(4)));}
		return Empty();
	}
	if(operation==std::string("InvertRect")) {
		HDC dc=CurrentPaintDC(window); if(dc!=nullptr){RECT rect{argInt(0),argInt(1),argInt(2),argInt(3)};::InvertRect(dc,&rect);}
		return Empty();
	}
	if(operation==std::string("Polygon")) {
		HDC dc=CurrentPaintDC(window); if(dc!=nullptr&&!args.empty()&&args[0].declaredArray){std::vector<POINT> points;points.reserve(args[0].elements.size()/2);for(std::size_t i=0;i+1<args[0].elements.size();i+=2)points.push_back(POINT{static_cast<LONG>(ToInteger(args[0].elements[i])),static_cast<LONG>(ToInteger(args[0].elements[i+1]))});if(!points.empty())::Polygon(dc,points.data(),static_cast<int>(points.size()));}
		return Empty();
	}
	if(operation==std::string("SetWritePos")||operation==std::string("Write")||operation==std::string("PrintLine")||operation==std::string("ScrollPrint")||operation==std::string("Say")||operation==std::string("CanvasGetWidth")||operation==std::string("CanvasGetHeight")) {
		Unit* unit=UnitFromWindow(window); if(unit==nullptr)return operation==std::string("CanvasGetWidth")||operation==std::string("CanvasGetHeight")?Integer(0):Empty();
		HDC dc=CurrentPaintDC(window);
		if(operation==std::string("SetWritePos")){if(argProvided(0))unit->writeX=argInt(0);if(argProvided(1))unit->writeY=argInt(1);return Empty();}
		if(operation==std::string("CanvasGetWidth")||operation==std::string("CanvasGetHeight")){SIZE size{};const std::wstring text=argText(0);if(dc!=nullptr)GetTextExtentPoint32W(dc,text.c_str(),static_cast<int>(text.size()),&size);return Integer(operation==std::string("CanvasGetWidth")?size.cx:size.cy);}
		const std::wstring text=args.empty()?std::wstring():argText(operation==std::string("Say")?2:0);
		if(dc!=nullptr){int x=unit->writeX,y=unit->writeY;if(operation==std::string("Say")){if(argProvided(0))x=argInt(0);if(argProvided(1))y=argInt(1);}TextOutW(dc,x,y,text.c_str(),static_cast<int>(text.size()));SIZE size{};GetTextExtentPoint32W(dc,text.c_str(),static_cast<int>(text.size()),&size);if(operation==std::string("PrintLine")||operation==std::string("ScrollPrint")){unit->writeX=0;unit->writeY+=size.cy;}else if(operation==std::string("Write")){unit->writeX=x+size.cx;unit->writeY=y;}}
		return Empty();
	}
    if(operation==std::string("AddText")) {
        HWND edit=EditPart(window); if(edit==nullptr)edit=window;
        const std::wstring text=argText(0); SendMessageW(edit,EM_SETSEL,static_cast<WPARAM>(-1),static_cast<LPARAM>(-1));
        SendMessageW(edit,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(text.c_str())); return Empty();
    }
    if(operation==std::string("Goto")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&!unit->hyperlinkTarget.empty())
            (void)ShellExecuteW(window,L"open",unit->hyperlinkTarget.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        return Empty();
    }
    if(operation==std::string("SendLabelMsg")) {
        std::vector<Value> values;
        values.push_back(args.size()>0?Integer(ToInteger(args[0])):Integer(0));
        values.push_back(args.size()>1?Integer(ToInteger(args[1])):Integer(0));
        const bool waitForResult=args.size()<3||ToBool(args[2]);
        if(waitForResult) { Value result; if(DispatchNativeResult(window,native_feedback,0,std::move(values),&result))return Integer(ToInteger(result)); }
        else DispatchNative(window,native_feedback,0,std::move(values));
        return Integer(0);
    }
    if(operation==std::string("GetTabCount"))return Integer(TabCtrl_GetItemCount(window));
    if(operation==std::string("GetTabName")&&tab) {
        TCITEMW item{}; wchar_t buffer[4096]{}; item.mask=TCIF_TEXT; item.pszText=buffer; item.cchTextMax=static_cast<int>(std::size(buffer));
        const int index=argInt(0)-1; return index>=0&&TabCtrl_GetItem(window,index,&item)?TextFromWide(buffer):Text("");
    }
    if(operation==std::string("SetTabName")&&tab) { TCITEMW item{}; std::wstring text=argText(1); item.mask=TCIF_TEXT; item.pszText=text.data(); return Boolean(argInt(0)>0&&TabCtrl_SetItem(window,argInt(0)-1,&item)!=FALSE); }
    if(operation==std::string("GetTopIndex"))return Integer(combo?SendMessageW(window,CB_GETTOPINDEX,0,0):(list||checklist?SendMessageW(window,LB_GETTOPINDEX,0,0):-1));
    if(operation==std::string("SetTopIndex"))return Boolean((combo?SendMessageW(window,CB_SETTOPINDEX,argInt(0),0):(list||checklist?SendMessageW(window,LB_SETTOPINDEX,argInt(0),0):LB_ERR))!=CB_ERR);
    if(operation==std::string("GetCount"))return Integer(tab?TabCtrl_GetItemCount(window):listCount());
    if(operation==std::string("GetItemData"))return Integer(combo?SendMessageW(window,CB_GETITEMDATA,argInt(0),0):(list||checklist?SendMessageW(window,LB_GETITEMDATA,argInt(0),0):-1));
    if(operation==std::string("SetItemData"))return Boolean((combo?SendMessageW(window,CB_SETITEMDATA,argInt(0),argInt(1)):(list||checklist?SendMessageW(window,LB_SETITEMDATA,argInt(0),argInt(1)):LB_ERR))!=LB_ERR);
    if(operation==std::string("GetItemText")) {
        const int index=argInt(0); int length=combo?SendMessageW(window,CB_GETLBTEXTLEN,index,0):((list||checklist)?SendMessageW(window,LB_GETTEXTLEN,index,0):-1); if(index<0||length<0)return Text("");
        std::wstring text(static_cast<std::size_t>(length+1),L'\0');
        const LRESULT copied=combo?SendMessageW(window,CB_GETLBTEXT,index,reinterpret_cast<LPARAM>(text.data())):
            ((list||checklist)?SendMessageW(window,LB_GETTEXT,index,reinterpret_cast<LPARAM>(text.data())):LB_ERR);
        if(copied<0)return Text("");
        text.resize(std::wcslen(text.c_str())); const int bytes=WideCharToMultiByte(CP_ACP,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr); std::string result(static_cast<std::size_t>((std::max)(bytes,0)),'\0'); if(bytes>0)WideCharToMultiByte(CP_ACP,0,text.data(),static_cast<int>(text.size()),result.data(),bytes,nullptr,nullptr); return Text(std::move(result));
    }
    if(operation==std::string("SetItemText")) {
        const int index=argInt(0); std::wstring text=argText(1); if(index<0||index>=listCount())return Boolean(false);
        // Replacing an item is implemented as delete/insert because Win32 has
        // no set-text message for list boxes.  Preserve the selection for all
        // single-selection list-like controls, not only check lists.
        const int selected=CurrentSelection(window);
        Unit* unit=checklist?UnitFromWindow(window):nullptr;
        std::vector<unsigned char> savedStates; std::vector<unsigned char> savedEnabled;
        unsigned char replacedChecked=0; unsigned char replacedEnabled=1;
        if(unit!=nullptr){ensureListState();savedStates=unit->checkStates;savedEnabled=unit->checkEnabled;
            if(static_cast<std::size_t>(index)<savedStates.size())replacedChecked=savedStates[static_cast<std::size_t>(index)];
            if(static_cast<std::size_t>(index)<savedEnabled.size())replacedEnabled=savedEnabled[static_cast<std::size_t>(index)];}
        const auto itemTextAt=[&](int itemIndex) {
            const int length=combo?static_cast<int>(SendMessageW(window,CB_GETLBTEXTLEN,itemIndex,0)):static_cast<int>(SendMessageW(window,LB_GETTEXTLEN,itemIndex,0));
            if(itemIndex<0||length<0)return std::wstring();
            std::wstring result(static_cast<std::size_t>(length+1),L'\0');
            const LRESULT copied=combo?SendMessageW(window,CB_GETLBTEXT,itemIndex,reinterpret_cast<LPARAM>(result.data())):SendMessageW(window,LB_GETTEXT,itemIndex,reinterpret_cast<LPARAM>(result.data()));
            if(copied<0)return std::wstring();
            result.resize(std::wcslen(result.c_str()));
            return result;
        };
        const std::wstring oldText=itemTextAt(index);
        const LRESULT oldData=combo?SendMessageW(window,CB_GETITEMDATA,index,0):SendMessageW(window,LB_GETITEMDATA,index,0);
        const LRESULT removed=combo?SendMessageW(window,CB_DELETESTRING,index,0):SendMessageW(window,LB_DELETESTRING,index,0);
        if(removed==CB_ERR)return Boolean(false);
        std::vector<unsigned char> remainingStates=savedStates;
        std::vector<unsigned char> remainingEnabled=savedEnabled;
        if(unit!=nullptr){
            if(static_cast<std::size_t>(index)<remainingStates.size())remainingStates.erase(remainingStates.begin()+index);
            if(static_cast<std::size_t>(index)<remainingEnabled.size())remainingEnabled.erase(remainingEnabled.begin()+index);
            unit->checkStates=remainingStates;unit->checkEnabled=remainingEnabled;
        }
        const LRESULT inserted=combo?SendMessageW(window,CB_INSERTSTRING,index,reinterpret_cast<LPARAM>(text.c_str())):SendMessageW(window,LB_INSERTSTRING,index,reinterpret_cast<LPARAM>(text.c_str()));
        if(inserted<0){
            // Best-effort rollback keeps the original item and its metadata if
            // the replacement allocation fails.
            const LRESULT restored=combo?SendMessageW(window,CB_INSERTSTRING,index,reinterpret_cast<LPARAM>(oldText.c_str())):SendMessageW(window,LB_INSERTSTRING,index,reinterpret_cast<LPARAM>(oldText.c_str()));
            if(restored>=0&&oldData!=CB_ERR)(void)(combo?SendMessageW(window,CB_SETITEMDATA,restored,oldData):SendMessageW(window,LB_SETITEMDATA,restored,oldData));
            if(unit!=nullptr){unit->checkStates=savedStates;unit->checkEnabled=savedEnabled;InvalidateRect(window,nullptr,TRUE);}
            return Boolean(false);
        }
        if(oldData!=CB_ERR)(void)(combo?SendMessageW(window,CB_SETITEMDATA,inserted,oldData):SendMessageW(window,LB_SETITEMDATA,inserted,oldData));
        if(checklist){
            // `inserted` is the actual index.  This matters for LBS_SORT and
            // CBS_SORT, where the requested index is only a hint.
            const int count=listCount(); const int slot=static_cast<int>(inserted);
            const std::size_t insertSlot=static_cast<std::size_t>((std::max)(0,(std::min)(slot,count-1)));
            unit->checkStates=std::move(remainingStates);unit->checkEnabled=std::move(remainingEnabled);
            unit->checkStates.insert(unit->checkStates.begin()+static_cast<std::ptrdiff_t>(insertSlot),replacedChecked);
            unit->checkEnabled.insert(unit->checkEnabled.begin()+static_cast<std::ptrdiff_t>(insertSlot),replacedEnabled);
            int selectedAfterDelete=selected;
            if(selected==index)selectedAfterDelete=-1; else if(selected>index)--selectedAfterDelete;
            if(selectedAfterDelete>=0&&insertSlot<=static_cast<std::size_t>(selectedAfterDelete))++selectedAfterDelete;
            if(selected==index)selectedAfterDelete=slot;
            if(selectedAfterDelete>=0)SendMessageW(window,LB_SETCURSEL,selectedAfterDelete,0);
            InvalidateRect(window,nullptr,TRUE);
        }
        else if(selected>=0) {
            int selectedAfterDelete=selected;
            if(selected==index)selectedAfterDelete=-1; else if(selected>index)--selectedAfterDelete;
            if(selectedAfterDelete>=0&&inserted<=selectedAfterDelete)++selectedAfterDelete;
            if(selected==index)selectedAfterDelete=static_cast<int>(inserted);
            if(combo)SendMessageW(window,CB_SETCURSEL,selectedAfterDelete,0);
            else if(list&&((GetWindowLongPtrW(window,GWL_STYLE)&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))==0))SendMessageW(window,LB_SETCURSEL,selectedAfterDelete,0);
        }
        return Boolean(true);
    }
    if(operation==std::string("AddString")) { std::wstring text=argText(0); if(Unit* u=UnitFromWindow(window);u!=nullptr&&u->removeDuplicates&&((combo&&SendMessageW(window,CB_FINDSTRINGEXACT,-1,reinterpret_cast<LPARAM>(text.c_str()))>=0)||((list||checklist)&&SendMessageW(window,LB_FINDSTRINGEXACT,-1,reinterpret_cast<LPARAM>(text.c_str()))>=0)))return Integer(-1); const LRESULT item=combo?SendMessageW(window,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str())):((list||checklist)?SendMessageW(window,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str())):LB_ERR); if(item>=0&&args.size()>1)(void)(combo?SendMessageW(window,CB_SETITEMDATA,item,argInt(1)):SendMessageW(window,LB_SETITEMDATA,item,argInt(1))); if(item>=0&&checklist){insertListState(static_cast<int>(item));InvalidateRect(window,nullptr,TRUE);} return Integer(item); }
    if(operation==std::string("InsertString")) { const int index=argInt(0); std::wstring text=argText(1); if(Unit* u=UnitFromWindow(window);u!=nullptr&&u->removeDuplicates&&((combo&&SendMessageW(window,CB_FINDSTRINGEXACT,-1,reinterpret_cast<LPARAM>(text.c_str()))>=0)||((list||checklist)&&SendMessageW(window,LB_FINDSTRINGEXACT,-1,reinterpret_cast<LPARAM>(text.c_str()))>=0)))return Integer(-1); const LRESULT item=combo?SendMessageW(window,CB_INSERTSTRING,index,reinterpret_cast<LPARAM>(text.c_str())):((list||checklist)?SendMessageW(window,LB_INSERTSTRING,index,reinterpret_cast<LPARAM>(text.c_str())):LB_ERR); if(item>=0&&args.size()>2)(void)(combo?SendMessageW(window,CB_SETITEMDATA,item,argInt(2)):SendMessageW(window,LB_SETITEMDATA,item,argInt(2))); if(item>=0&&checklist){insertListState(static_cast<int>(item));InvalidateRect(window,nullptr,TRUE);} return Integer(item); }
    if(operation==std::string("DeleteString")){const int index=argInt(0);const LRESULT result=combo?SendMessageW(window,CB_DELETESTRING,index,0):((list||checklist)?SendMessageW(window,LB_DELETESTRING,index,0):LB_ERR);if(result!=CB_ERR&&checklist){eraseListState(index);InvalidateRect(window,nullptr,TRUE);}return Boolean(result!=CB_ERR);}
    if(operation==std::string("Clear")){(void)(combo?SendMessageW(window,CB_RESETCONTENT,0,0):((list||checklist)?SendMessageW(window,LB_RESETCONTENT,0,0):0));if(checklist)if(Unit* unit=UnitFromWindow(window)){unit->checkStates.clear();unit->checkEnabled.clear();}return Empty();}
    if(operation==std::string("SelItem")){const std::wstring wanted=argText(0);if(list&&!checklist&&(GetWindowLongPtrW(window,GWL_STYLE)&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))!=0)return Integer(-1);const int count=listCount();for(int i=0;i<count;++i){int length=combo?SendMessageW(window,CB_GETLBTEXTLEN,i,0):SendMessageW(window,LB_GETTEXTLEN,i,0);std::wstring text(static_cast<std::size_t>((std::max)(length,0)+1),L'\0');(void)(combo?SendMessageW(window,CB_GETLBTEXT,i,reinterpret_cast<LPARAM>(text.data())):SendMessageW(window,LB_GETTEXT,i,reinterpret_cast<LPARAM>(text.data())));if(text.rfind(wanted,0)==0){(void)(combo?SendMessageW(window,CB_SETCURSEL,i,0):SendMessageW(window,LB_SETCURSEL,i,0));return Integer(i);}}return Integer(-1);}
    if(operation==std::string("GetCaretIndex")) {
        if(!list&&!checklist)return Integer(-1);
        const LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);
        if((style&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))==0)return Integer(SendMessageW(window,LB_GETCURSEL,0,0));
        return Integer(SendMessageW(window,LB_GETCARETINDEX,0,0));
    }
    if(operation==std::string("SetCaretIndex")) {
        if(!list&&!checklist)return Boolean(false);
        const LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);
        if((style&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))==0)return Boolean(SendMessageW(window,LB_SETCURSEL,argInt(0),0)!=LB_ERR);
        return Boolean(SendMessageW(window,LB_SETCARETINDEX,argInt(0),TRUE)!=FALSE);
    }
    if(operation==std::string("GetSelCount"))return Integer(list&&!checklist?SendMessageW(window,LB_GETSELCOUNT,0,0):-1);
    if(operation==std::string("GetSelItems")) {
        if(!list||checklist)return MakeVar(T_INT,true,false);
        const int count=static_cast<int>(SendMessageW(window,LB_GETSELCOUNT,0,0));
        Value result=MakeVar(T_INT,true,false);
        if(count<=0)return result;
        result.dimensions={count}; result.elements.reserve(static_cast<std::size_t>(count));
        std::vector<int> indexes(static_cast<std::size_t>(count));
        if(SendMessageW(window,LB_GETSELITEMS,count,reinterpret_cast<LPARAM>(indexes.data()))==LB_ERR)return result;
        for(const int index:indexes)result.elements.push_back(Integer(index,T_INT));
        return result;
    }
    if(operation==std::string("IsSelected"))return Boolean(list&&!checklist&&SendMessageW(window,LB_GETSEL,argInt(0),0)>0);
    if(operation==std::string("SelectItem")) {
        const bool state=ToBool(args.size()>1?args[1]:Boolean(true));
        if(checklist)return Boolean(SendMessageW(window,LB_SETCURSEL,state?argInt(0):-1,0)!=LB_ERR);
        if(!list)return Boolean(false);
        const LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);
        if((style&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))!=0)return Boolean(SendMessageW(window,LB_SETSEL,state,argInt(0))!=LB_ERR);
        if(!state)return Boolean(SendMessageW(window,LB_SETCURSEL,-1,0)!=LB_ERR);
        return Boolean(SendMessageW(window,LB_SETCURSEL,argInt(0),0)!=LB_ERR);
    }
    if(operation==std::string("IsChecked")) {
        if(!checklist)return Boolean(false);
        Unit* unit=UnitFromWindow(window);const int index=argInt(0);if(unit==nullptr||index<0)return Boolean(false);ensureListState();return Boolean(index<static_cast<int>(unit->checkStates.size())&&unit->checkStates[static_cast<std::size_t>(index)]!=0);
    }
    if(operation==std::string("SetCheck")) {
        if(!checklist)return Boolean(false);
        return Boolean(SetCheckState(window,argInt(0),ToBool(args.size()>1?args[1]:Boolean(true))));
    }
    if(operation==std::string("IsEnabled")) {
        if(!checklist)return Boolean(false);
        Unit* unit=UnitFromWindow(window);const int index=argInt(0);if(unit==nullptr||index<0)return Boolean(false);ensureListState();return Boolean(index<static_cast<int>(unit->checkEnabled.size())&&unit->checkEnabled[static_cast<std::size_t>(index)]!=0);
    }
    if(operation==std::string("Enable")) {
        if(!checklist)return Boolean(false);
        Unit* unit=UnitFromWindow(window);const int index=argInt(0);if(unit==nullptr||index<0)return Boolean(false);const int count=listCount();if(index>=count)return Boolean(false);ensureListState();unit->checkEnabled[static_cast<std::size_t>(index)]=ToBool(args.size()>1?args[1]:Boolean(true))?1:0;InvalidateRect(window,nullptr,TRUE);return Boolean(true);
    }
    return Empty();
}
static void SetText(HWND window,const Value& value) { const std::wstring text=Wide(ToText(value).c_str()); SetWindowTextW(window,text.c_str()); }
static bool PropertyEquals(const char* property,const wchar_t* expected) {
    return AttributeEquals(property,expected);
}
static HWND WindowById(unsigned int id) {
    HWND window=FindForm(id);
    if(window==nullptr)if(const auto* unit=FindUnit(id))window=unit->hwnd;
    return window;
}
static Value SpecPropertyValue(const char* raw) {
    if(raw==nullptr)return Empty();
    if(std::strcmp(raw,"1")==0||AttributeEquals(raw,L"真")||AttributeEquals(raw,L"true"))return Boolean(true);
    if(std::strcmp(raw,"0")==0||AttributeEquals(raw,L"假")||AttributeEquals(raw,L"false"))return Boolean(false);
    char* end=nullptr; const long long integer=_strtoi64(raw,&end,10);
    if(end!=raw&&*end=='\0')return Integer(integer);
    end=nullptr; const double number=std::strtod(raw,&end);
    if(end!=raw&&*end=='\0')return Number(number);
    return Text(raw);
}
static void StoreSpecProperties(std::unordered_map<std::string,Value>& store,const XmlAttribute* attributes,std::size_t count) {
    if(attributes==nullptr)return;
    for(std::size_t index=0;index<count;++index)if(attributes[index].name!=nullptr)store[attributes[index].name]=SpecPropertyValue(attributes[index].value);
}
static std::unordered_map<std::string,Value>* PropertyStore(unsigned int id) {
    if(Form* form=FindFormRecord(WindowById(id)))return &form->properties;
    if(Unit* unit=FindUnit(id))return &unit->properties;
    return nullptr;
}
static bool ClassEquals(HWND window,const wchar_t* expected) {
    wchar_t actual[64]{};
    return window!=nullptr&&GetClassNameW(window,actual,static_cast<int>(std::size(actual)))>0&&lstrcmpiW(actual,expected)==0;
}
static int CurrentSelection(HWND window) {
    if(ClassEquals(window,L"LISTBOX")) {
        const LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);
        // Multi-select list boxes still have a logical current (caret) item.
        // LB_GETCURSEL is documented to return -1 for them, which used to
        // leak into 易语言的“现行选中项” property.  Report the caret item
        // instead, while retaining -1 when no item has focus.
        if((style&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))!=0) {
            const LRESULT caret=SendMessageW(window,LB_GETCARETINDEX,0,0);
            return caret>=0&&caret<=static_cast<LRESULT>((std::numeric_limits<int>::max)())
                ? static_cast<int>(caret) : -1;
        }
        return static_cast<int>(SendMessageW(window,LB_GETCURSEL,0,0));
    }
    if(ClassEquals(window,L"COMBOBOX"))return static_cast<int>(SendMessageW(window,CB_GETCURSEL,0,0));
    return -1;
}
static int CurrentPosition(HWND window) {
    if(ClassEquals(window,L"msctls_progress32"))return static_cast<int>(SendMessageW(window,PBM_GETPOS,0,0));
    if(ClassEquals(window,L"msctls_trackbar32"))return static_cast<int>(SendMessageW(window,TBM_GETPOS,0,0));
    if(ClassEquals(window,UPDOWN_CLASSW)) {
        BOOL ok=FALSE;
        const LRESULT value=SendMessageW(window,UDM_GETPOS32,0,reinterpret_cast<LPARAM>(&ok));
        return ok?static_cast<int>(value):0;
    }
    if(ClassEquals(window,L"SCROLLBAR")) {
        SCROLLINFO info{sizeof(SCROLLINFO),SIF_POS,0,0,0,0,0};
        GetScrollInfo(window,SB_CTL,&info);
        return info.nPos;
    }
    return 0;
}
static int CanvasDimension(HWND window,const Unit& unit,bool height) {
    RECT client{};
    if(window==nullptr||GetClientRect(window,&client)==FALSE)return 0;
    const int pixels=height?(std::max)(0,static_cast<int>(client.bottom)):(std::max)(0,static_cast<int>(client.right));
    if(unit.drawUnit==0)return pixels;
    HDC dc=GetDC(window);
    if(dc==nullptr)return pixels;
    const int dpi=GetDeviceCaps(dc,height?LOGPIXELSY:LOGPIXELSX);
    ReleaseDC(window,dc);
    if(dpi<=0)return pixels;
    const double unitsPerPixel=unit.drawUnit==1?254.0/dpi:unit.drawUnit==2?2540.0/dpi:unit.drawUnit==3?100.0/dpi:unit.drawUnit==4?1000.0/dpi:1440.0/dpi;
    return static_cast<int>(std::llround(pixels*unitsPerPixel));
}
static int TrackSelectionStart(HWND window) {
    if(!ClassEquals(window,L"msctls_trackbar32"))return 0;
    return static_cast<int>(SendMessageW(window,TBM_GETSELSTART,0,0));
}
static int TrackSelectionLength(HWND window) {
    if(!ClassEquals(window,L"msctls_trackbar32"))return 0;
    const int start=static_cast<int>(SendMessageW(window,TBM_GETSELSTART,0,0));
    const int end=static_cast<int>(SendMessageW(window,TBM_GETSELEND,0,0));
    return end-start;
}
static void SetPosition(HWND window,int position) {
    if(ClassEquals(window,L"msctls_progress32"))SendMessageW(window,PBM_SETPOS,position,0);
    else if(ClassEquals(window,L"msctls_trackbar32"))SendMessageW(window,TBM_SETPOS,TRUE,position);
    else if(ClassEquals(window,L"SCROLLBAR"))SetScrollPos(window,SB_CTL,position,TRUE);
}
static bool WindowGeometry(HWND window,int& left,int& top,int& width,int& height) {
    RECT rect{};
    if(window==nullptr||!GetWindowRect(window,&rect))return false;
    HWND parent=GetParent(window);
    if(parent==nullptr) {
        POINT origin{0,0};
        ClientToScreen(window,&origin);
        RECT client{};GetClientRect(window,&client);
        left=origin.x;top=origin.y;width=client.right;height=client.bottom;
    }
    else {
        MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&rect),2);
        left=rect.left;top=rect.top;width=rect.right-rect.left;height=rect.bottom-rect.top;
    }
    return true;
}
static void SetWindowGeometry(HWND window,const char* property,const Value& value) {
    int left=0,top=0,width=0,height=0;
    if(!WindowGeometry(window,left,top,width,height))return;
    const int number=static_cast<int>(ToInteger(value));
    if(PropertyEquals(property,L"\u5de6\u8fb9"))left=number;
    else if(PropertyEquals(property,L"\u9876\u8fb9"))top=number;
    else if(PropertyEquals(property,L"\u5bbd\u5ea6"))width=(std::max)(0,number);
    else if(PropertyEquals(property,L"\u9ad8\u5ea6"))height=(std::max)(0,number);
    else return;
    if(GetParent(window)==nullptr) {
        RECT client{0,0,width,height};
        const LONG style=GetWindowLongW(window,GWL_STYLE);
        const LONG exStyle=GetWindowLongW(window,GWL_EXSTYLE);
        AdjustWindowRectEx(&client,static_cast<DWORD>(style),FALSE,static_cast<DWORD>(exStyle));
        SetWindowPos(window,nullptr,left,top,client.right-client.left,client.bottom-client.top,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    else SetWindowPos(window,nullptr,left,top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
}
static Value GetProperty(unsigned int id,const char* property) {
    HWND window=WindowById(id);
    if(window==nullptr)return Empty();
    if(Form* form=FindFormRecord(window)) {
        if(PropertyEquals(property,L"\u53ef\u5426\u79fb\u52a8"))return Boolean(form->canMove);
        if(PropertyEquals(property,L"\u56de\u8f66\u4e0b\u79fb\u7126\u70b9"))return Boolean(form->enterToNext);
        if(PropertyEquals(property,L"Esc\u952e\u5173\u95ed"))return Boolean(form->escapeCloses);
        if(PropertyEquals(property,L"F1\u952e\u6253\u5f00\u5e2e\u52a9"))return Boolean(form->f1OpenHelp);
        if(PropertyEquals(property,L"\u5e2e\u52a9\u6587\u4ef6\u540d"))return TextFromWide(form->helpFileName);
        if(PropertyEquals(property,L"\u5e2e\u52a9\u6807\u5fd7\u503c"))return Integer(form->helpContext);
        if(PropertyEquals(property,L"\u968f\u610f\u79fb\u52a8"))return Boolean(form->hitMove);
        if(PropertyEquals(property,L"\u603b\u5728\u6700\u524d"))return Boolean(form->topmost);
        if(PropertyEquals(property,L"\u5728\u4efb\u52a1\u6761\u4e2d\u663e\u793a"))return Boolean(form->showInTaskbar);
        if(PropertyEquals(property,L"\u8fb9\u6846"))return Integer(form->border);
        if(PropertyEquals(property,L"\u4f4d\u7f6e"))return Integer(form->position);
        if(PropertyEquals(property,L"\u63a7\u5236\u6309\u94ae"))return Boolean(form->controlButtons);
        if(PropertyEquals(property,L"\u6700\u5927\u5316\u6309\u94ae"))return Boolean(form->maximizeButton);
        if(PropertyEquals(property,L"\u6700\u5c0f\u5316\u6309\u94ae"))return Boolean(form->minimizeButton);
        if(PropertyEquals(property,L"\u4fdd\u6301\u6807\u9898\u6761\u6fc0\u6d3b"))return Boolean(form->keepTitleBarActive);
        if(PropertyEquals(property,L"\u5e95\u8272"))return Integer(static_cast<long long>(form->hasBackColor?form->backColor:GetSysColor(COLOR_WINDOW)));
        if(PropertyEquals(property,L"\u5e95\u56fe"))return Bytes(form->backPicData);
        if(PropertyEquals(property,L"\u5916\u5f62"))return Integer(form->shape);
        if(PropertyEquals(property,L"\u5e95\u56fe\u65b9\u5f0f"))return Integer(form->backPicMode);
        if(PropertyEquals(property,L"\u64ad\u653e\u6b21\u6570"))return Integer(form->playCount);
    }
    if(PropertyEquals(property,L"\u6807\u9898")||PropertyEquals(property,L"\u5185\u5bb9")){
        HWND edit=PropertyEquals(property,L"\u5185\u5bb9")?EditPart(window):nullptr;
        return WindowTextValue(edit!=nullptr?edit:window);
    }
    if(PropertyEquals(property,L"\u53ef\u89c6"))return Boolean(IsWindowVisible(window)!=FALSE);
    if(PropertyEquals(property,L"\u7981\u6b62"))return Boolean(IsWindowEnabled(window)==FALSE);
    if(PropertyEquals(property,L"\u6807\u8bb0"))if(const Unit* unit=UnitFromWindow(window))return TextFromWide(unit->tag);
    if(PropertyEquals(property,L"\u9f20\u6807\u6307\u9488"))if(const Unit* unit=UnitFromWindow(window))return Bytes(unit->cursorData);
    if(const Unit* unit=UnitFromWindow(window)) {
        if(PropertyEquals(property,L"\u5b57\u4f53\u540d\u79f0"))return TextFromWide(unit->fontName);
        if(PropertyEquals(property,L"\u5b57\u4f53\u5927\u5c0f"))return Integer(unit->fontSize);
        if(PropertyEquals(property,L"\u52a0\u7c97"))return Boolean(unit->fontBold);
        if(PropertyEquals(property,L"\u503e\u659c"))return Boolean(unit->fontItalic);
        if(PropertyEquals(property,L"\u5220\u9664\u7ebf"))return Boolean(unit->fontStrikeOut);
        if(PropertyEquals(property,L"\u4e0b\u5212\u7ebf"))return Boolean(unit->fontUnderline);
        if(PropertyEquals(property,L"\u8fb9\u6846"))return Integer(unit->borderStyle);
        if(PropertyEquals(property,L"\u6a2a\u5411\u5bf9\u9f50\u65b9\u5f0f"))return Integer(unit->horizontalAlign);
        if(PropertyEquals(property,L"\u7eb5\u5411\u5bf9\u9f50\u65b9\u5f0f"))return Integer(unit->verticalAlign);
    }
    if(PropertyEquals(property,L"\u5bc6\u7801\u906e\u76d6\u5b57\u7b26"))if(const Unit* unit=UnitFromWindow(window))return TextFromWide(unit->passwordChar);
    if(PropertyEquals(property,L"\u53ef\u505c\u7559\u7126\u70b9"))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&WS_TABSTOP)!=0);
    if(PropertyEquals(property,L"\u9690\u85cf\u9009\u62e9"))if(HWND edit=EditPart(window))if(const Unit* unit=UnitFromWindow(window))return Boolean(unit->hideSelection);
    if(PropertyEquals(property,L"\u662f\u5426\u5141\u8bb8\u591a\u884c"))if(HWND edit=EditPart(window))return Boolean((GetWindowLongPtrW(edit,GWL_STYLE)&ES_MULTILINE)!=0);
    if(PropertyEquals(property,L"\u8f93\u5165\u65b9\u5f0f"))if(const Unit* unit=UnitFromWindow(window))return Integer(unit->inputMode);
    if(PropertyEquals(property,L"\u8f6c\u6362\u65b9\u5f0f"))if(const Unit* unit=UnitFromWindow(window))return Integer(unit->convertMode);
    if(PropertyEquals(property,L"\u5bf9\u9f50\u65b9\u5f0f"))if(const Unit* unit=UnitFromWindow(window))return Integer(unit->horizontalAlign);
    if(PropertyEquals(property,L"\u6eda\u52a8\u6761"))if(const Unit* unit=UnitFromWindow(window)){const LONG_PTR style=GetWindowLongPtrW(EditPart(window)!=nullptr?EditPart(window):window,GWL_STYLE);return Integer((style&WS_HSCROLL?1:0)|(style&WS_VSCROLL?2:0));}
    if(PropertyEquals(property,L"\u8c03\u8282\u5668\u65b9\u5f0f"))if(const Unit* unit=UnitFromWindow(window))return Integer(unit->spinMode);
    if(PropertyEquals(property,L"\u8c03\u8282\u5668\u5e95\u9650\u503c"))if(const Unit* unit=UnitFromWindow(window))return Integer(unit->spinMin);
    if(PropertyEquals(property,L"\u8c03\u8282\u5668\u4e0a\u9650\u503c"))if(const Unit* unit=UnitFromWindow(window))return Integer(unit->spinMax);
    if(PropertyEquals(property,L"\u5141\u8bb8\u9009\u62e9\u591a\u9879")&&(ClassEquals(window,L"LISTBOX")||ClassEquals(window,L"COMBOBOX")))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))!=0);
    if(PropertyEquals(property,L"\u81ea\u52a8\u6392\u5e8f")&&(ClassEquals(window,L"LISTBOX")||ClassEquals(window,L"COMBOBOX")))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&(ClassEquals(window,L"LISTBOX")?LBS_SORT:CBS_SORT))!=0);
    if(PropertyEquals(property,L"\u591a\u5217")&&ClassEquals(window,L"LISTBOX"))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&LBS_MULTICOLUMN)!=0);
    if(PropertyEquals(property,L"\u9009\u4e2d"))return Boolean(SendMessageW(window,BM_GETCHECK,0,0)==BST_CHECKED);
    if(PropertyEquals(property,L"\u73b0\u884c\u9009\u4e2d\u9879"))return Integer(CurrentSelection(window));
    if(PropertyEquals(property,L"\u4f4d\u7f6e")&&ClassEquals(window,UPDOWN_CLASSW))return Integer(CurrentPosition(window));
    if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr) {
        if(std::strcmp(unit->type,"timer")==0&&PropertyEquals(property,L"\u65f6\u949f\u5468\u671f"))return Integer(unit->timerPeriod);
        if(std::strcmp(unit->type,"drive")==0&&PropertyEquals(property,L"\u9a71\u52a8\u5668"))return WindowTextValue(window);
        if(std::strcmp(unit->type,"drive")==0&&PropertyEquals(property,L"\u7c7b\u578b"))return Integer(unit->driveType);
        if((std::strcmp(unit->type,"directory")==0||std::strcmp(unit->type,"file")==0)&&PropertyEquals(property,L"\u76ee\u5f55"))return TextFromWide(unit->path);
        if(std::strcmp(unit->type,"file")==0) {
            if(PropertyEquals(property,L"\u901a\u5e38"))return Boolean(unit->allowNormal);
            if(PropertyEquals(property,L"\u5b58\u6863"))return Boolean(unit->allowArchive);
            if(PropertyEquals(property,L"\u53ea\u8bfb"))return Boolean(unit->allowReadOnly);
            if(PropertyEquals(property,L"\u7cfb\u7edf"))return Boolean(unit->allowSystem);
            if(PropertyEquals(property,L"\u9690\u85cf"))return Boolean(unit->allowHidden);
            if(PropertyEquals(property,L"\u5141\u8bb8\u9009\u62e9\u591a\u9879"))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))!=0);
            if(PropertyEquals(property,L"\u6700\u5c0f\u65e5\u671f"))return unit->hasMinDate?Number(SystemTimeOleDate(unit->minDate)):Number(0.0);
            if(PropertyEquals(property,L"\u6700\u5927\u65e5\u671f"))return unit->hasMaxDate?Number(SystemTimeOleDate(unit->maxDate)):Number(0.0);
        }
        if(std::strcmp(unit->type,"file")==0&&PropertyEquals(property,L"\u88ab\u9009\u62e9\u6587\u4ef6")) {
            const int count=static_cast<int>(SendMessageW(window,LB_GETCOUNT,0,0)); const LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); std::wstring result;
            for(int index=0;index<count;++index) {
                if((style&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))!=0&&SendMessageW(window,LB_GETSEL,index,0)<=0)continue;
                if((style&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))==0&&index!=CurrentSelection(window))continue;
                const int length=static_cast<int>(SendMessageW(window,LB_GETTEXTLEN,index,0)); if(length<0)continue; std::wstring item(static_cast<std::size_t>(length+1),L'\0'); SendMessageW(window,LB_GETTEXT,index,reinterpret_cast<LPARAM>(item.data())); item.resize(std::wcslen(item.c_str())); if(!result.empty())result+=L';'; result+=item;
            }
            return TextFromWide(result);
        }
        if(std::strcmp(unit->type,"color")==0&&PropertyEquals(property,L"\u989c\u8272"))return Integer(static_cast<long long>(unit->hasColor?unit->color:0));
        if(std::strcmp(unit->type,"color")==0&&PropertyEquals(property,L"\u5141\u8bb8\u900f\u660e"))return Boolean(unit->allowTransparent);
        if(std::strcmp(unit->type,"hyperlink")==0) {
            if(PropertyEquals(property,L"\u7c7b\u578b"))return Integer(unit->hyperlinkType);
            if(PropertyEquals(property,L"\u7535\u5b50\u90ae\u7bb1\u5730\u5740")&&unit->hyperlinkType==0)return TextFromWide(unit->hyperlinkTarget);
            if(PropertyEquals(property,L"Internet\u5730\u5740")&&unit->hyperlinkType!=0)return TextFromWide(unit->hyperlinkTarget);
        }
    }
    if(PropertyEquals(property,L"\u73b0\u884c\u5b50\u5939")&&ClassEquals(window,L"SysTabControl32"))return Integer(SendMessageW(window,TCM_GETCURSEL,0,0));
    if(PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e")||PropertyEquals(property,L"\u6700\u5927\u4f4d\u7f6e")) {
        if(ClassEquals(window,L"msctls_progress32")) { PBRANGE range{};SendMessageW(window,PBM_GETRANGE,TRUE,reinterpret_cast<LPARAM>(&range));return Integer(PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e")?range.iLow:range.iHigh); }
        if(ClassEquals(window,L"msctls_trackbar32"))return Integer(PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e")?SendMessageW(window,TBM_GETRANGEMIN,0,0):SendMessageW(window,TBM_GETRANGEMAX,0,0));
        if(ClassEquals(window,L"SCROLLBAR")) { SCROLLINFO info{sizeof(SCROLLINFO),SIF_RANGE,0,0,0,0,0};GetScrollInfo(window,SB_CTL,&info);return Integer(PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e")?info.nMin:info.nMax); }
    }
    if(PropertyEquals(property,L"\u9875\u6539\u53d8\u503c")||PropertyEquals(property,L"\u884c\u6539\u53d8\u503c")) {
        if(ClassEquals(window,L"msctls_trackbar32"))return Integer(PropertyEquals(property,L"\u9875\u6539\u53d8\u503c")?SendMessageW(window,TBM_GETPAGESIZE,0,0):SendMessageW(window,TBM_GETLINESIZE,0,0));
        if(ClassEquals(window,L"SCROLLBAR"))if(const Unit* unit=UnitFromWindow(window))return Integer(PropertyEquals(property,L"\u9875\u6539\u53d8\u503c")?unit->scrollPage:unit->scrollLine);
    }
    if(PropertyEquals(property,L"\u5141\u8bb8\u62d6\u52a8\u8ddf\u8e2a"))if(const Unit* unit=UnitFromWindow(window))return Boolean(unit->allowTrack);
    if(PropertyEquals(property,L"\u5141\u8bb8\u7f16\u8f91"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"date")==0)return Boolean(unit->dateAllowEdit);
    if(PropertyEquals(property,L"\u8868\u5934\u65b9\u5411"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"tab")==0)return Integer(unit->tabHeaderWay);
    if(PropertyEquals(property,L"\u5141\u8bb8\u591a\u884c\u8868\u5934"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"tab")==0)return Boolean(unit->tabMultiLine);
    if(PropertyEquals(property,L"\u662f\u5426\u586b\u5145\u80cc\u666f"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"tab")==0)return Boolean(unit->tabFillBack);
    if(PropertyEquals(property,L"\u9690\u85cf\u81ea\u8eab"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"tab")==0)return Boolean(!IsWindowVisible(window));
    if(PropertyEquals(property,L"\u523b\u5ea6\u7c7b\u578b"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"trackbar")==0)return Integer(unit->trackTickStyle);
    if(PropertyEquals(property,L"\u5355\u4f4d\u523b\u5ea6\u503c"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"trackbar")==0)return Integer(unit->trackTickFreq);
    if(PropertyEquals(property,L"\u5141\u8bb8\u9009\u62e9"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"trackbar")==0)return Boolean(unit->trackAllowSel);
    if(PropertyEquals(property,L"\u663e\u793a\u65b9\u5f0f"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"progress")==0)return Integer(unit->progressDrawMode);
    if(PropertyEquals(property,L"\u65b9\u5411"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&(std::strcmp(unit->type,"spin")==0||std::strcmp(unit->type,"progress")==0||std::strcmp(unit->type,"trackbar")==0)){const LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);return Integer(std::strcmp(unit->type,"spin")==0?((style&UDS_HORZ)!=0?0:1):((style&(std::strcmp(unit->type,"progress")==0?PBS_VERTICAL:TBS_VERT))!=0?1:0));}
    if(PropertyEquals(property,L"\u5c45\u4e2d\u64ad\u653e"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"animate")==0)return Boolean(unit->imageCenter);
    if(PropertyEquals(property,L"\u900f\u660e\u80cc\u666f"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"animate")==0)return Boolean(unit->imageTransparent);
    if(PropertyEquals(property,L"\u64ad\u653e\u52a8\u753b"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"image")==0)return Boolean(unit->playImage);
    if(PropertyEquals(property,L"\u9664\u53bb\u91cd\u590d"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&(std::strcmp(unit->type,"list")==0||std::strcmp(unit->type,"checklist")==0||std::strcmp(unit->type,"combo")==0))return Boolean(unit->removeDuplicates);
    if(PropertyEquals(property,L"\u6309\u94ae\u5f62\u5f0f"))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&BS_PUSHLIKE)!=0);
    if(PropertyEquals(property,L"\u5e73\u9762"))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&BS_FLAT)!=0);
    if(PropertyEquals(property,L"\u6807\u9898\u5c45\u5de6"))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&BS_LEFT)!=0);
    if(PropertyEquals(property,L"\u6700\u5927\u5141\u8bb8\u957f\u5ea6")) { HWND edit=EditPart(window);if(edit!=nullptr)return Integer(SendMessageW(edit,EM_GETLIMITTEXT,0,0)); }
    if(PropertyEquals(property,L"\u4eca\u5929")){SYSTEMTIME date{};if(WindowDateValue(window,date))return Number(SystemTimeOleDate(date));return Number(0.0);}
    if(PropertyEquals(property,L"\u6700\u5c0f\u65e5\u671f")){SYSTEMTIME date{};if(GetDateRange(window,&date,nullptr)||GetMonthRange(window,&date,nullptr))return Number(SystemTimeOleDate(date));return Number(0.0);}
    if(PropertyEquals(property,L"\u6700\u5927\u65e5\u671f")){SYSTEMTIME date{};if(GetDateRange(window,nullptr,&date)||GetMonthRange(window,nullptr,&date))return Number(SystemTimeOleDate(date));return Number(0.0);}
    if(PropertyEquals(property,L"\u9996\u9009\u62e9\u65e5")||PropertyEquals(property,L"\u5c3e\u9009\u62e9\u65e5")){if(ClassEquals(window,L"SysMonthCal32")){SYSTEMTIME range[2]{};SendMessageW(window,MCM_GETSELRANGE,0,reinterpret_cast<LPARAM>(range));return Number(SystemTimeOleDate(range[PropertyEquals(property,L"\u5c3e\u9009\u62e9\u65e5")?1:0]));}return Number(0.0);}
    if(PropertyEquals(property,L"\u9996\u9009\u62e9\u4f4d\u7f6e"))return Integer(TrackSelectionStart(window));
    if(PropertyEquals(property,L"\u9009\u62e9\u957f\u5ea6"))return Integer(TrackSelectionLength(window));
    if(PropertyEquals(property,L"\u6700\u591a\u9009\u62e9\u5929\u6570")&&ClassEquals(window,L"SysMonthCal32"))return Integer(SendMessageW(window,MCM_GETMAXSELCOUNT,0,0));
    if(PropertyEquals(property,L"\u6eda\u52a8\u6708\u6570")&&ClassEquals(window,L"SysMonthCal32"))return Integer(SendMessageW(window,MCM_GETMONTHDELTA,0,0));
    if(PropertyEquals(property,L"\u5f00\u59cb\u661f\u671f\u9996\u65e5")&&ClassEquals(window,L"SysMonthCal32"))return Integer(LOWORD(SendMessageW(window,MCM_GETFIRSTDAYOFWEEK,0,0)));
    if(PropertyEquals(property,L"\u4e0d\u663e\u793a\u4eca\u5929")&&ClassEquals(window,L"SysMonthCal32"))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&MCS_NOTODAY)!=0);
    if(PropertyEquals(property,L"\u4e0d\u5708\u6ce8\u4eca\u5929")&&ClassEquals(window,L"SysMonthCal32"))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&MCS_NOTODAYCIRCLE)!=0);
    if(PropertyEquals(property,L"\u663e\u793a\u661f\u671f\u5e8f\u53f7")&&ClassEquals(window,L"SysMonthCal32"))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&MCS_WEEKNUMBERS)!=0);
    if(PropertyEquals(property,L"\u5141\u8bb8\u9009\u62e9\u591a\u5929")&&ClassEquals(window,L"SysMonthCal32"))return Boolean((GetWindowLongPtrW(window,GWL_STYLE)&MCS_MULTISELECT)!=0);
    if(ClassEquals(window,L"SysMonthCal32")) {
        int colorIndex=-1;
        if(PropertyEquals(property,L"\u6587\u672c\u989c\u8272"))colorIndex=MCSC_TEXT;
        else if(PropertyEquals(property,L"\u80cc\u666f\u989c\u8272")||PropertyEquals(property,L"\u5185\u80cc\u666f\u989c\u8272"))colorIndex=MCSC_MONTHBK;
        else if(PropertyEquals(property,L"\u6807\u9898\u989c\u8272"))colorIndex=MCSC_TITLETEXT;
        else if(PropertyEquals(property,L"\u6807\u9898\u80cc\u666f\u989c\u8272"))colorIndex=MCSC_TITLEBK;
        else if(PropertyEquals(property,L"\u975e\u672c\u6708\u989c\u8272"))colorIndex=MCSC_TRAILINGTEXT;
        if(colorIndex>=0)return Integer(static_cast<long long>(SendMessageW(window,MCM_GETCOLOR,colorIndex,0)));
    }
    if(PropertyEquals(property,L"\u6587\u672c\u989c\u8272"))if(const Unit* unit=UnitFromWindow(window))return Integer(static_cast<long long>(unit->hasTextColor?unit->textColor:GetSysColor(COLOR_WINDOWTEXT)));
    if((PropertyEquals(property,L"\u80cc\u666f\u989c\u8272")||PropertyEquals(property,L"\u753b\u677f\u80cc\u666f\u8272")))if(const Unit* unit=UnitFromWindow(window))return Integer(static_cast<long long>(unit->type!=nullptr&&std::strcmp(unit->type,"tab")==0?unit->tabBackColor:(unit->hasBackColor?unit->backColor:GetSysColor(COLOR_WINDOW))));
    if(PropertyEquals(property,L"\u663e\u793a\u65b9\u5f0f"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"progress")!=0)return Integer(unit->imageDrawMode);
    if(PropertyEquals(property,L"\u6548\u679c"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"label")==0)return Integer(unit->labelEffect);
    if(PropertyEquals(property,L"\u6e10\u53d8\u80cc\u666f\u65b9\u5f0f"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"label")==0)return Integer(unit->gradientBackMode);
    if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"label")==0) {
        if(PropertyEquals(property,L"\u6e10\u53d8\u8fb9\u6846\u5bbd\u5ea6"))return Integer(unit->gradientBorderWidth);
        for(int index=0;index<3;++index) {
            if(PropertyEquals(property,(L"\u6e10\u53d8\u8fb9\u6846\u989c\u8272"+std::to_wstring(index+1)).c_str()))return Integer(unit->gradientBorderColor[index]);
            if(PropertyEquals(property,(L"\u6e10\u53d8\u80cc\u666f\u989c\u8272"+std::to_wstring(index+1)).c_str()))return Integer(unit->gradientBackColor[index]);
        }
    }
    if(PropertyEquals(property,L"\u662f\u5426\u81ea\u52a8\u6298\u884c"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"label")==0)return Boolean(unit->labelWordWrap);
    if(PropertyEquals(property,L"\u6587\u4ef6\u540d"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"animate")==0)return TextFromWide(unit->imageFileName);
    if(PropertyEquals(property,L"\u64ad\u653e"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"animate")==0)return Boolean(unit->playImage);
    if(PropertyEquals(property,L"\u64ad\u653e\u6b21\u6570"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"animate")==0)return Integer(unit->imagePlayCount);
    if(PropertyEquals(property,L"\u5916\u5f62"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"shape")==0)return Integer(unit->shape);
    if(PropertyEquals(property,L"\u7ebf\u6761\u6548\u679c"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"shape")==0)return Integer(unit->shapeEffect);
    if(PropertyEquals(property,L"\u7ebf\u578b"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"shape")==0)return Integer(unit->lineStyle);
    if(PropertyEquals(property,L"\u7ebf\u5bbd"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"shape")==0)return Integer(unit->lineWidth);
    if(PropertyEquals(property,L"\u7ebf\u6761\u989c\u8272"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"shape")==0)return Integer(unit->lineColor);
    if(PropertyEquals(property,L"\u586b\u5145\u989c\u8272"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"shape")==0)return Integer(unit->fillColor);
    if(PropertyEquals(property,L"\u56fe\u7247")||PropertyEquals(property,L"\u5e95\u56fe"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&(std::strcmp(unit->type,"image")==0||std::strcmp(unit->type,"label")==0||std::strcmp(unit->type,"canvas")==0))return Bytes(unit->imageData);
    if(PropertyEquals(property,L"\u5355\u9009"))if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&IsCheckList(window))return Boolean(unit->checkOnlyOne);
    if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"hyperlink")==0) {
        if(PropertyEquals(property,L"\u8bbf\u95ee\u540e\u7684\u989c\u8272"))return Integer(unit->visitedColor);
        if(PropertyEquals(property,L"\u70ed\u70b9\u989c\u8272"))return Integer(unit->hotColor);
        if(PropertyEquals(property,L"\u70ed\u70b9\u8ddf\u8e2a"))return Boolean(unit->hyperlinkTrack);
    }
    if(const Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"canvas")==0) {
        if(PropertyEquals(property,L"\u81ea\u52a8\u91cd\u753b"))return Boolean(unit->autoRedraw);
        if(PropertyEquals(property,L"\u7ed8\u753b\u5355\u4f4d"))return Integer(unit->drawUnit);
        if(PropertyEquals(property,L"\u753b\u7b14\u7c7b\u578b"))return Integer(unit->penStyle);
        if(PropertyEquals(property,L"\u753b\u51fa\u65b9\u5f0f"))return Integer(unit->drawRop2);
        if(PropertyEquals(property,L"\u753b\u7b14\u7c97\u7ec6"))return Integer(unit->penWidth);
        if(PropertyEquals(property,L"\u5237\u5b50\u7c7b\u578b"))return Integer(unit->brushStyle);
        if(PropertyEquals(property,L"\u753b\u7b14\u989c\u8272"))return Integer(unit->penColor);
        if(PropertyEquals(property,L"\u5237\u5b50\u989c\u8272"))return Integer(unit->brushColor);
        if(PropertyEquals(property,L"\u6587\u672c\u80cc\u666f\u989c\u8272"))return Integer(unit->textBackColor);
        if(PropertyEquals(property,L"\u5e95\u56fe\u65b9\u5f0f"))return Integer(unit->backPicMode);
        if(PropertyEquals(property,L"\u753b\u677f\u5bbd\u5ea6"))return Integer(CanvasDimension(window,*unit,false));
        if(PropertyEquals(property,L"\u753b\u677f\u9ad8\u5ea6"))return Integer(CanvasDimension(window,*unit,true));
    }
    HWND edit=EditPart(window);
    if(edit!=nullptr&&PropertyEquals(property,L"\u8d77\u59cb\u9009\u62e9\u4f4d\u7f6e")) { DWORD start=0,end=0;SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));return Integer(start); }
    if(edit!=nullptr&&PropertyEquals(property,L"\u88ab\u9009\u62e9\u5b57\u7b26\u6570")) { DWORD start=0,end=0;SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));return Integer(static_cast<int>(end-start)); }
    if(edit!=nullptr&&PropertyEquals(property,L"\u88ab\u9009\u62e9\u6587\u672c")) {
        DWORD start=0,end=0;SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
        const std::wstring text=WindowTextW(edit); if(start>end||start>text.size())return Text("");
        end=(std::min<DWORD>)(end,static_cast<DWORD>(text.size())); return TextFromWide(text.substr(start,end-start));
    }
    if(PropertyEquals(property,L"\u4f4d\u7f6e"))return Integer(CurrentPosition(window));
    int left=0,top=0,width=0,height=0;
    if(PropertyEquals(property,L"\u5de6\u8fb9")||PropertyEquals(property,L"\u9876\u8fb9")||
        PropertyEquals(property,L"\u5bbd\u5ea6")||PropertyEquals(property,L"\u9ad8\u5ea6")) {
        if(!WindowGeometry(window,left,top,width,height))return Empty();
        if(PropertyEquals(property,L"\u5de6\u8fb9"))return Integer(left);
        if(PropertyEquals(property,L"\u9876\u8fb9"))return Integer(top);
        if(PropertyEquals(property,L"\u5bbd\u5ea6"))return Integer(width);
        return Integer(height);
    }
    if(const auto* store=PropertyStore(id)){const auto it=store->find(property==nullptr?std::string():std::string(property));if(it!=store->end())return it->second;}
    return Empty();
}
static void SetProperty(unsigned int id,const char* property,const Value& value) {
    HWND window=WindowById(id);
    if(window==nullptr)return;
    if(auto* store=PropertyStore(id);store!=nullptr&&property!=nullptr) (*store)[property]=value;
    if(Form* form=FindFormRecord(window)) {
        if(PropertyEquals(property,L"\u63a7\u5236\u6309\u94ae")||PropertyEquals(property,L"\u6700\u5927\u5316\u6309\u94ae")||PropertyEquals(property,L"\u6700\u5c0f\u5316\u6309\u94ae")||PropertyEquals(property,L"\u8fb9\u6846")) {
            if(PropertyEquals(property,L"\u63a7\u5236\u6309\u94ae"))form->controlButtons=ToBool(value);
            else if(PropertyEquals(property,L"\u6700\u5927\u5316\u6309\u94ae"))form->maximizeButton=ToBool(value);
            else if(PropertyEquals(property,L"\u6700\u5c0f\u5316\u6309\u94ae"))form->minimizeButton=ToBool(value);
            else form->border=static_cast<int>(ToInteger(value));
            const LONG_PTR style=FormStyle(form->border,form->controlButtons,form->maximizeButton,form->minimizeButton);
            SetWindowLongPtrW(window,GWL_STYLE,style);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);return;
        }
        if(PropertyEquals(property,L"\u4f4d\u7f6e")){form->position=static_cast<int>(ToInteger(value));if(form->position==2)ShowWindow(window,SW_MINIMIZE);else if(form->position==3)ShowWindow(window,SW_MAXIMIZE);else {ShowWindow(window,SW_RESTORE);PlaceForm(window,0,0,form->position);}return;}
        if(PropertyEquals(property,L"\u5728\u4efb\u52a1\u6761\u4e2d\u663e\u793a")){form->showInTaskbar=ToBool(value);LONG_PTR ex=GetWindowLongPtrW(window,GWL_EXSTYLE);if(form->showInTaskbar)ex&=~WS_EX_TOOLWINDOW;else ex|=WS_EX_TOOLWINDOW;SetWindowLongPtrW(window,GWL_EXSTYLE,ex);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);return;}
        if(PropertyEquals(property,L"\u53ef\u5426\u79fb\u52a8")){form->canMove=ToBool(value);return;}
        if(PropertyEquals(property,L"\u56de\u8f66\u4e0b\u79fb\u7126\u70b9")){form->enterToNext=ToBool(value);return;}
        if(PropertyEquals(property,L"Esc\u952e\u5173\u95ed")){form->escapeCloses=ToBool(value);return;}
        if(PropertyEquals(property,L"F1\u952e\u6253\u5f00\u5e2e\u52a9")){form->f1OpenHelp=ToBool(value);return;}
        if(PropertyEquals(property,L"\u5e2e\u52a9\u6587\u4ef6\u540d")){form->helpFileName=Wide(ToText(value).c_str());return;}
        if(PropertyEquals(property,L"\u5e2e\u52a9\u6807\u5fd7\u503c")){form->helpContext=static_cast<int>(ToInteger(value));return;}
        if(PropertyEquals(property,L"\u968f\u610f\u79fb\u52a8")){form->hitMove=ToBool(value);return;}
        if(PropertyEquals(property,L"\u603b\u5728\u6700\u524d")){form->topmost=ToBool(value);SetWindowPos(window,form->topmost?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);return;}
        if(PropertyEquals(property,L"\u4fdd\u6301\u6807\u9898\u6761\u6fc0\u6d3b")){form->keepTitleBarActive=ToBool(value);return;}
        if(PropertyEquals(property,L"\u5e95\u8272")){form->backColor=static_cast<COLORREF>(ToInteger(value));form->hasBackColor=true;InvalidateRect(window,nullptr,TRUE);return;}
        if(PropertyEquals(property,L"\u5e95\u56fe")){form->backPicData=value.bytes;InvalidateRect(window,nullptr,TRUE);return;}
        if(PropertyEquals(property,L"\u5e95\u56fe\u65b9\u5f0f")){form->backPicMode=static_cast<int>(ToInteger(value));InvalidateRect(window,nullptr,TRUE);return;}
        if(PropertyEquals(property,L"\u5916\u5f62")){form->shape=static_cast<int>(ToInteger(value));InvalidateRect(window,nullptr,TRUE);return;}
    }
    if(PropertyEquals(property,L"\u6807\u9898")||PropertyEquals(property,L"\u5185\u5bb9")) {
        HWND edit=PropertyEquals(property,L"\u5185\u5bb9")?EditPart(window):nullptr;
        SetText(edit!=nullptr?edit:window,value);
    }
    else if(PropertyEquals(property,L"\u5b57\u4f53\u540d\u79f0")||PropertyEquals(property,L"\u5b57\u4f53\u5927\u5c0f")||PropertyEquals(property,L"\u52a0\u7c97")||PropertyEquals(property,L"\u503e\u659c")||PropertyEquals(property,L"\u5220\u9664\u7ebf")||PropertyEquals(property,L"\u4e0b\u5212\u7ebf")) {
        if(Unit* unit=UnitFromWindow(window)) {
            if(PropertyEquals(property,L"\u5b57\u4f53\u540d\u79f0"))unit->fontName=Wide(ToText(value).c_str());
            else if(PropertyEquals(property,L"\u5b57\u4f53\u5927\u5c0f"))unit->fontSize=(std::max)(1,static_cast<int>(ToInteger(value)));
            else if(PropertyEquals(property,L"\u52a0\u7c97"))unit->fontBold=ToBool(value);
            else if(PropertyEquals(property,L"\u503e\u659c"))unit->fontItalic=ToBool(value);
            else if(PropertyEquals(property,L"\u5220\u9664\u7ebf"))unit->fontStrikeOut=ToBool(value);
            else unit->fontUnderline=ToBool(value);
            ApplyUnitFont(window,*unit);
        }
    }
    else if(PropertyEquals(property,L"\u8fb9\u6846"))if(Unit* unit=UnitFromWindow(window)){unit->borderStyle=static_cast<int>(ToInteger(value));SetWindowBorder(window,unit->borderStyle);}
    else if(PropertyEquals(property,L"\u6a2a\u5411\u5bf9\u9f50\u65b9\u5f0f")||PropertyEquals(property,L"\u7eb5\u5411\u5bf9\u9f50\u65b9\u5f0f"))if(Unit* unit=UnitFromWindow(window)){if(PropertyEquals(property,L"\u6a2a\u5411\u5bf9\u9f50\u65b9\u5f0f"))unit->horizontalAlign=static_cast<int>(ToInteger(value));else unit->verticalAlign=static_cast<int>(ToInteger(value));InvalidateRect(window,nullptr,TRUE);}
    else if(PropertyEquals(property,L"\u53ef\u89c6"))ShowWindow(window,ToInteger(value)?SW_SHOW:SW_HIDE);
    else if(PropertyEquals(property,L"\u9690\u85cf\u81ea\u8eab")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"tab")==0)
            ShowWindow(window,ToBool(value)?SW_HIDE:SW_SHOW);
    }
    else if(PropertyEquals(property,L"\u7981\u6b62"))EnableWindow(window,ToInteger(value)?FALSE:TRUE);
    else if(PropertyEquals(property,L"\u6807\u8bb0"))if(Unit* unit=UnitFromWindow(window))unit->tag=Wide(ToText(value).c_str());
    else if(PropertyEquals(property,L"\u9f20\u6807\u6307\u9488"))if(Unit* unit=UnitFromWindow(window)) { ReplaceUnitCursor(*unit,value.bytes); SetCursor(unit->cursor!=nullptr?unit->cursor:LoadCursorW(nullptr,IDC_ARROW)); }
    else if(PropertyEquals(property,L"\u9690\u85cf\u9009\u62e9"))if(Unit* unit=UnitFromWindow(window)){unit->hideSelection=ToBool(value);if(HWND edit=EditPart(window))SendMessageW(edit,0x043Fu,unit->hideSelection,FALSE);}
    else if(PropertyEquals(property,L"\u5bc6\u7801\u906e\u76d6\u5b57\u7b26"))if(Unit* unit=UnitFromWindow(window)){unit->passwordChar=Wide(ToText(value).c_str());if(HWND edit=EditPart(window))SendMessageW(edit,EM_SETPASSWORDCHAR,unit->passwordChar.empty()?L'*':unit->passwordChar.front(),0);}
    else if(PropertyEquals(property,L"\u8f93\u5165\u65b9\u5f0f"))if(Unit* unit=UnitFromWindow(window)){unit->inputMode=static_cast<int>(ToInteger(value));if(HWND edit=EditPart(window)){const int mode=unit->inputMode;SendMessageW(edit,EM_SETREADONLY,mode==1,0);if(mode==2)SendMessageW(edit,EM_SETPASSWORDCHAR,L'*',0);else SendMessageW(edit,EM_SETPASSWORDCHAR,0,0);}}
    else if(PropertyEquals(property,L"\u5bf9\u9f50\u65b9\u5f0f"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"edit")==0){unit->horizontalAlign=static_cast<int>(ToInteger(value));HWND edit=EditPart(window);LONG_PTR style=GetWindowLongPtrW(edit!=nullptr?edit:window,GWL_STYLE);style&=~(ES_LEFT|ES_CENTER|ES_RIGHT);style|=unit->horizontalAlign==1?ES_CENTER:(unit->horizontalAlign==2?ES_RIGHT:ES_LEFT);SetWindowLongPtrW(edit!=nullptr?edit:window,GWL_STYLE,style);InvalidateRect(edit!=nullptr?edit:window,nullptr,TRUE);}
    else if(PropertyEquals(property,L"\u8c03\u8282\u5668\u5e95\u9650\u503c")||PropertyEquals(property,L"\u8c03\u8282\u5668\u4e0a\u9650\u503c")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"spin")==0) {
            if(PropertyEquals(property,L"\u8c03\u8282\u5668\u5e95\u9650\u503c"))unit->spinMin=static_cast<int>(ToInteger(value));
            else unit->spinMax=static_cast<int>(ToInteger(value));
            if(unit->spinMin>unit->spinMax)std::swap(unit->spinMin,unit->spinMax);
            SendMessageW(window,UDM_SETRANGE32,unit->spinMin,unit->spinMax);
        }
    }
    else if(PropertyEquals(property,L"\u6eda\u52a8\u6761"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"edit")==0){const int mode=static_cast<int>(ToInteger(value));HWND edit=EditPart(window);LONG_PTR style=GetWindowLongPtrW(edit!=nullptr?edit:window,GWL_STYLE);if(mode==1||mode==3)style|=WS_HSCROLL;else style&=~WS_HSCROLL;if(mode==2||mode==3)style|=WS_VSCROLL;else style&=~WS_VSCROLL;SetWindowLongPtrW(edit!=nullptr?edit:window,GWL_STYLE,style);SetWindowPos(edit!=nullptr?edit:window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);}
    else if(PropertyEquals(property,L"\u8f6c\u6362\u65b9\u5f0f"))if(Unit* unit=UnitFromWindow(window))unit->convertMode=static_cast<int>(ToInteger(value));
    else if(PropertyEquals(property,L"\u662f\u5426\u5141\u8bb8\u591a\u884c")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"edit")==0) {
            HWND edit=EditPart(window); if(edit==nullptr)edit=window;
            LONG_PTR style=GetWindowLongPtrW(edit,GWL_STYLE);
            if(ToBool(value))style|=ES_MULTILINE|ES_AUTOVSCROLL;
            else style&=~(ES_MULTILINE|ES_AUTOVSCROLL|ES_AUTOHSCROLL);
            SetWindowLongPtrW(edit,GWL_STYLE,style);
            SetWindowPos(edit,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
            InvalidateRect(edit,nullptr,TRUE);
        }
    }
    else if(PropertyEquals(property,L"\u8c03\u8282\u5668\u65b9\u5f0f"))if(Unit* unit=UnitFromWindow(window)) { unit->spinMode=(std::clamp)(static_cast<int>(ToInteger(value)),0,2); if(std::strcmp(unit->type,"edit")==0)ConfigureEditSpin(window,*unit,IsWindowVisible(window)!=FALSE,IsWindowEnabled(window)==FALSE); }
    else if(PropertyEquals(property,L"\u6700\u5927\u5141\u8bb8\u957f\u5ea6")||PropertyEquals(property,L"\u6700\u5927\u6587\u672c\u957f\u5ea6")) { if(HWND edit=EditPart(window);edit!=nullptr)SendMessageW(edit,EM_SETLIMITTEXT,static_cast<WPARAM>((std::max)(0,static_cast<int>(ToInteger(value)))),0); }
    else if(PropertyEquals(property,L"\u53ef\u505c\u7559\u7126\u70b9")) { LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); if(ToBool(value))style|=WS_TABSTOP;else style&=~WS_TABSTOP; SetWindowLongPtrW(window,GWL_STYLE,style); }
    else if(PropertyEquals(property,L"\u81ea\u52a8\u6392\u5e8f")&&(ClassEquals(window,L"LISTBOX")||ClassEquals(window,L"COMBOBOX"))) { LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); const LONG_PTR bit=ClassEquals(window,L"LISTBOX")?LBS_SORT:CBS_SORT; if(ToBool(value))style|=bit;else style&=~bit; SetWindowLongPtrW(window,GWL_STYLE,style); }
    else if(PropertyEquals(property,L"\u591a\u5217")&&ClassEquals(window,L"LISTBOX")) { LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); if(ToBool(value))style|=LBS_MULTICOLUMN;else style&=~LBS_MULTICOLUMN; SetWindowLongPtrW(window,GWL_STYLE,style); }
    else if(PropertyEquals(property,L"\u5141\u8bb8\u9009\u62e9\u591a\u9879")&&ClassEquals(window,L"LISTBOX")) { LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); style&=~(LBS_MULTIPLESEL|LBS_EXTENDEDSEL); if(ToBool(value))style|=LBS_EXTENDEDSEL; SetWindowLongPtrW(window,GWL_STYLE,style); if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"file")==0){unit->fileAllowMultiple=ToBool(value);PopulatePathListRuntime(window,*unit,false);} }
    else if(PropertyEquals(property,L"\u9009\u4e2d"))SendMessageW(window,BM_SETCHECK,ToInteger(value)?BST_CHECKED:BST_UNCHECKED,0);
    else if(PropertyEquals(property,L"\u9a71\u52a8\u5668")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"drive")==0) {
            std::wstring drive=Wide(ToText(value).c_str()); if(drive.size()==1)drive+=L":"; if(drive.size()==2)drive+=L"\\";
            const LRESULT index=SendMessageW(window,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(drive.c_str())); if(index>=0)SendMessageW(window,CB_SETCURSEL,index,0);
        }
    }
    else if(PropertyEquals(property,L"\u76ee\u5f55")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&(std::strcmp(unit->type,"directory")==0||std::strcmp(unit->type,"file")==0)) { unit->path=Wide(ToText(value).c_str()); PopulatePathListRuntime(window,*unit,std::strcmp(unit->type,"directory")==0); }
    }
    else if(PropertyEquals(property,L"\u901a\u914d\u7b26")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"file")==0) { unit->filePattern=Wide(ToText(value).c_str()); PopulatePathListRuntime(window,*unit,false); }
    }
    else if(PropertyEquals(property,L"\u989c\u8272")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"color")==0) { unit->color=static_cast<COLORREF>(ToInteger(value));unit->hasColor=true;InvalidateRect(window,nullptr,TRUE);DispatchNative(window,native_changed,0); }
    }
    else if(PropertyEquals(property,L"\u65f6\u949f\u5468\u671f")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"timer")==0) {
            KillTimer(window,id); const int period=static_cast<int>(ToInteger(value)); unit->timerPeriod=period>0?static_cast<UINT>(period):0;
            if(unit->timerPeriod!=0)SetTimer(window,id,unit->timerPeriod,nullptr);
        }
    }
    else if(PropertyEquals(property,L"\u5141\u8bb8\u62d6\u52a8\u8ddf\u8e2a")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&(std::strcmp(unit->type,"hscroll")==0||std::strcmp(unit->type,"vscroll")==0))unit->allowTrack=ToBool(value);
    }
    else if(PropertyEquals(property,L"\u5141\u8bb8\u7f16\u8f91")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"date")==0) {
            unit->dateAllowEdit=ToBool(value);
            if(HWND child=FindWindowExW(window,nullptr,L"Edit",nullptr))SendMessageW(child,EM_SETREADONLY,unit->dateAllowEdit?FALSE:TRUE,0);
        }
    }
    else if(PropertyEquals(property,L"\u8868\u5934\u65b9\u5411")||PropertyEquals(property,L"\u5141\u8bb8\u591a\u884c\u8868\u5934")||PropertyEquals(property,L"\u662f\u5426\u586b\u5145\u80cc\u666f")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"tab")==0) {
            if(PropertyEquals(property,L"\u8868\u5934\u65b9\u5411"))unit->tabHeaderWay=static_cast<int>(ToInteger(value));
            else if(PropertyEquals(property,L"\u5141\u8bb8\u591a\u884c\u8868\u5934"))unit->tabMultiLine=ToBool(value);
            else unit->tabFillBack=ToBool(value);
            LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);style&=~(TCS_BOTTOM|TCS_VERTICAL|TCS_MULTILINE);
            if(unit->tabHeaderWay==1)style|=TCS_BOTTOM;else if(unit->tabHeaderWay==2)style|=TCS_VERTICAL;else if(unit->tabHeaderWay==3)style|=TCS_VERTICAL|TCS_RIGHT;
            if(unit->tabMultiLine)style|=TCS_MULTILINE;
            SetWindowLongPtrW(window,GWL_STYLE,style);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);InvalidateRect(window,nullptr,TRUE);
        }
    }
    else if(PropertyEquals(property,L"\u523b\u5ea6\u7c7b\u578b")||PropertyEquals(property,L"\u5355\u4f4d\u523b\u5ea6\u503c")||PropertyEquals(property,L"\u5141\u8bb8\u9009\u62e9")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"trackbar")==0) {
            if(PropertyEquals(property,L"\u523b\u5ea6\u7c7b\u578b"))unit->trackTickStyle=static_cast<int>(ToInteger(value));
            else if(PropertyEquals(property,L"\u5355\u4f4d\u523b\u5ea6\u503c"))unit->trackTickFreq=(std::max)(0,static_cast<int>(ToInteger(value)));
            else { unit->trackAllowSel=ToBool(value); if(!unit->trackAllowSel)SendMessageW(window,TBM_CLEARSEL,TRUE,0); }
            LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);style&=~(TBS_AUTOTICKS|TBS_NOTICKS|TBS_TOP|TBS_BOTTOM|TBS_BOTH);
            if(unit->trackTickStyle==0)style|=TBS_NOTICKS;else if(unit->trackTickStyle==1)style|=TBS_TOP;else if(unit->trackTickStyle==2)style|=TBS_BOTTOM;else style|=TBS_BOTH;
            SetWindowLongPtrW(window,GWL_STYLE,style);if(unit->trackTickFreq>0)SendMessageW(window,TBM_SETTICFREQ,unit->trackTickFreq,0);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);InvalidateRect(window,nullptr,TRUE);
        }
    }
    else if(PropertyEquals(property,L"\u663e\u793a\u65b9\u5f0f")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"progress")==0) {unit->progressDrawMode=static_cast<int>(ToInteger(value));LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);if(unit->progressDrawMode==1)style|=PBS_SMOOTH;else style&=~PBS_SMOOTH;SetWindowLongPtrW(window,GWL_STYLE,style);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);}
    }
    else if(PropertyEquals(property,L"\u65b9\u5411"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&(std::strcmp(unit->type,"spin")==0||std::strcmp(unit->type,"progress")==0||std::strcmp(unit->type,"trackbar")==0)){LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);if(std::strcmp(unit->type,"spin")==0){style&=~UDS_HORZ;if(ToInteger(value)==0)style|=UDS_HORZ;}else if(std::strcmp(unit->type,"progress")==0){if(ToInteger(value)!=0)style|=PBS_VERTICAL;else style&=~PBS_VERTICAL;}else{if(ToInteger(value)!=0)style|=TBS_VERT;else style&=~TBS_VERT;}SetWindowLongPtrW(window,GWL_STYLE,style);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);}
    else if(PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e")||PropertyEquals(property,L"\u6700\u5927\u4f4d\u7f6e")||PropertyEquals(property,L"\u4f4d\u7f6e")||PropertyEquals(property,L"\u9875\u6539\u53d8\u503c")||PropertyEquals(property,L"\u884c\u6539\u53d8\u503c")||PropertyEquals(property,L"\u9996\u9009\u62e9\u4f4d\u7f6e")||PropertyEquals(property,L"\u9009\u62e9\u957f\u5ea6")) {
        const int number=static_cast<int>(ToInteger(value));
        if(ClassEquals(window,UPDOWN_CLASSW)&&PropertyEquals(property,L"\u4f4d\u7f6e"))SendMessageW(window,UDM_SETPOS32,0,number);
        else if(ClassEquals(window,L"msctls_progress32")) { if(PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e")||PropertyEquals(property,L"\u6700\u5927\u4f4d\u7f6e")){PBRANGE range{};SendMessageW(window,PBM_GETRANGE,TRUE,reinterpret_cast<LPARAM>(&range));if(PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e"))range.iLow=number;else range.iHigh=number;SendMessageW(window,PBM_SETRANGE32,range.iLow,range.iHigh);}else if(PropertyEquals(property,L"\u4f4d\u7f6e"))SendMessageW(window,PBM_SETPOS,number,0); }
        else if(ClassEquals(window,L"msctls_trackbar32")) { if(PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e"))SendMessageW(window,TBM_SETRANGEMIN,TRUE,number);else if(PropertyEquals(property,L"\u6700\u5927\u4f4d\u7f6e"))SendMessageW(window,TBM_SETRANGEMAX,TRUE,number);else if(PropertyEquals(property,L"\u4f4d\u7f6e"))SendMessageW(window,TBM_SETPOS,TRUE,number);else if(PropertyEquals(property,L"\u9875\u6539\u53d8\u503c"))SendMessageW(window,TBM_SETPAGESIZE,0,number);else if(PropertyEquals(property,L"\u884c\u6539\u53d8\u503c"))SendMessageW(window,TBM_SETLINESIZE,0,number);else {const int start=PropertyEquals(property,L"\u9996\u9009\u62e9\u4f4d\u7f6e")?number:static_cast<int>(SendMessageW(window,TBM_GETSELSTART,0,0));const int length=PropertyEquals(property,L"\u9009\u62e9\u957f\u5ea6")?number:static_cast<int>(SendMessageW(window,TBM_GETSELEND,0,0))-start;SendMessageW(window,TBM_SETSELSTART,TRUE,start);SendMessageW(window,TBM_SETSELEND,TRUE,start+(std::max)(0,length));} }
        else if(ClassEquals(window,L"SCROLLBAR")) { Unit* unit=UnitFromWindow(window);if(unit!=nullptr){if(PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e"))unit->scrollMin=number;else if(PropertyEquals(property,L"\u6700\u5927\u4f4d\u7f6e"))unit->scrollMax=number;else if(PropertyEquals(property,L"\u9875\u6539\u53d8\u503c"))unit->scrollPage=number;else if(PropertyEquals(property,L"\u884c\u6539\u53d8\u503c"))unit->scrollLine=number;else if(PropertyEquals(property,L"\u4f4d\u7f6e"))SetScrollPos(window,SB_CTL,number,TRUE);SCROLLINFO info{sizeof(SCROLLINFO),SIF_RANGE|SIF_PAGE|SIF_POS,unit->scrollMin,unit->scrollMax,static_cast<UINT>(unit->scrollPage),number,0};SetScrollInfo(window,SB_CTL,&info,TRUE);}} }
    else if(PropertyEquals(property,L"\u9664\u53bb\u91cd\u590d"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&(std::strcmp(unit->type,"list")==0||std::strcmp(unit->type,"checklist")==0||std::strcmp(unit->type,"combo")==0))unit->removeDuplicates=ToBool(value);
    else if(PropertyEquals(property,L"\u5c45\u4e2d\u64ad\u653e")||PropertyEquals(property,L"\u900f\u660e\u80cc\u666f"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"animate")==0){if(PropertyEquals(property,L"\u5c45\u4e2d\u64ad\u653e"))unit->imageCenter=ToBool(value);else unit->imageTransparent=ToBool(value);LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);style&=~(ACS_CENTER|ACS_TRANSPARENT);if(unit->imageCenter)style|=ACS_CENTER;if(unit->imageTransparent)style|=ACS_TRANSPARENT;SetWindowLongPtrW(window,GWL_STYLE,style);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);}
    else if(PropertyEquals(property,L"\u64ad\u653e\u52a8\u753b"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"image")==0){unit->playImage=ToBool(value);}
    else if(PropertyEquals(property,L"\u6309\u94ae\u5f62\u5f0f")||PropertyEquals(property,L"\u5e73\u9762")||PropertyEquals(property,L"\u6807\u9898\u5c45\u5de6")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&(std::strcmp(unit->type,"checkbox")==0||std::strcmp(unit->type,"radio")==0)) {
            LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);
            const LONG_PTR bit=PropertyEquals(property,L"\u6309\u94ae\u5f62\u5f0f")?BS_PUSHLIKE:(PropertyEquals(property,L"\u5e73\u9762")?BS_FLAT:BS_LEFT);
            if(ToBool(value))style|=bit;else style&=~bit;
            SetWindowLongPtrW(window,GWL_STYLE,style);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);InvalidateRect(window,nullptr,TRUE);
        }
    }
    else if(PropertyEquals(property,L"\u5141\u8bb8\u900f\u660e")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"color")==0)unit->allowTransparent=ToBool(value);
    }
    else if(PropertyEquals(property,L"\u8bbf\u95ee\u540e\u7684\u989c\u8272")||PropertyEquals(property,L"\u70ed\u70b9\u989c\u8272")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"hyperlink")==0) { if(PropertyEquals(property,L"\u8bbf\u95ee\u540e\u7684\u989c\u8272"))unit->visitedColor=static_cast<COLORREF>(ToInteger(value)); else unit->hotColor=static_cast<COLORREF>(ToInteger(value)); InvalidateRect(window,nullptr,TRUE); }
    }
    else if(PropertyEquals(property,L"\u56fe\u7247")||PropertyEquals(property,L"\u5e95\u56fe")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&(std::strcmp(unit->type,"image")==0||std::strcmp(unit->type,"label")==0||std::strcmp(unit->type,"canvas")==0)) { unit->imageData=value.bytes; InvalidateRect(window,nullptr,TRUE); }
    }
    else if(PropertyEquals(property,L"\u81ea\u52a8\u91cd\u753b")||PropertyEquals(property,L"\u7ed8\u753b\u5355\u4f4d")||PropertyEquals(property,L"\u753b\u7b14\u7c7b\u578b")||PropertyEquals(property,L"\u753b\u51fa\u65b9\u5f0f")||PropertyEquals(property,L"\u753b\u7b14\u7c97\u7ec6")||PropertyEquals(property,L"\u5237\u5b50\u7c7b\u578b")||PropertyEquals(property,L"\u753b\u7b14\u989c\u8272")||PropertyEquals(property,L"\u5237\u5b50\u989c\u8272")||PropertyEquals(property,L"\u6587\u672c\u80cc\u666f\u989c\u8272")||PropertyEquals(property,L"\u5e95\u56fe\u65b9\u5f0f")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"canvas")==0) {
            if(PropertyEquals(property,L"\u81ea\u52a8\u91cd\u753b"))unit->autoRedraw=ToBool(value);
            else if(PropertyEquals(property,L"\u7ed8\u753b\u5355\u4f4d"))unit->drawUnit=static_cast<int>(ToInteger(value));
            else if(PropertyEquals(property,L"\u753b\u7b14\u7c7b\u578b"))unit->penStyle=static_cast<int>(ToInteger(value));
            else if(PropertyEquals(property,L"\u753b\u51fa\u65b9\u5f0f"))unit->drawRop2=static_cast<int>(ToInteger(value));
            else if(PropertyEquals(property,L"\u753b\u7b14\u7c97\u7ec6"))unit->penWidth=(std::max)(0,static_cast<int>(ToInteger(value)));
            else if(PropertyEquals(property,L"\u5237\u5b50\u7c7b\u578b"))unit->brushStyle=static_cast<int>(ToInteger(value));
            else if(PropertyEquals(property,L"\u753b\u7b14\u989c\u8272"))unit->penColor=static_cast<COLORREF>(ToInteger(value));
            else if(PropertyEquals(property,L"\u5237\u5b50\u989c\u8272"))unit->brushColor=static_cast<COLORREF>(ToInteger(value));
            else if(PropertyEquals(property,L"\u6587\u672c\u80cc\u666f\u989c\u8272"))unit->textBackColor=static_cast<COLORREF>(ToInteger(value));
            else unit->backPicMode=static_cast<int>(ToInteger(value));
            InvalidateRect(window,nullptr,TRUE);
        }
    }
    else if(PropertyEquals(property,L"\u6e10\u53d8\u8fb9\u6846\u5bbd\u5ea6")||PropertyEquals(property,L"\u6e10\u53d8\u8fb9\u6846\u989c\u82721")||PropertyEquals(property,L"\u6e10\u53d8\u8fb9\u6846\u989c\u82722")||PropertyEquals(property,L"\u6e10\u53d8\u8fb9\u6846\u989c\u82723")||PropertyEquals(property,L"\u6e10\u53d8\u80cc\u666f\u989c\u82721")||PropertyEquals(property,L"\u6e10\u53d8\u80cc\u666f\u989c\u82722")||PropertyEquals(property,L"\u6e10\u53d8\u80cc\u666f\u989c\u82723")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"label")==0) {
            if(PropertyEquals(property,L"\u6e10\u53d8\u8fb9\u6846\u5bbd\u5ea6"))unit->gradientBorderWidth=(std::max)(0,static_cast<int>(ToInteger(value)));
            else for(int index=0;index<3;++index) {
                const std::wstring borderName=L"渐变边框颜色"+std::to_wstring(index+1), backName=L"渐变背景颜色"+std::to_wstring(index+1);
                if(PropertyEquals(property,borderName.c_str())) { unit->gradientBorderColor[index]=static_cast<COLORREF>(ToInteger(value));unit->gradientBorderColorSet[index]=1; }
                if(PropertyEquals(property,backName.c_str())) { unit->gradientBackColor[index]=static_cast<COLORREF>(ToInteger(value));unit->gradientBackColorSet[index]=1; }
            }
            InvalidateRect(window,nullptr,TRUE);
        }
    }
    else if(PropertyEquals(property,L"\u7c7b\u578b")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"hyperlink")==0)unit->hyperlinkType=static_cast<int>(ToInteger(value));
        else if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"drive")==0) { unit->driveType=static_cast<int>(ToInteger(value)); PopulateDriveList(window,unit); }
        else if(ClassEquals(window,L"COMBOBOX")) { const int type=static_cast<int>(ToInteger(value)); LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); style&=~CBS_SIMPLE; style&=~CBS_DROPDOWN; style&=~CBS_DROPDOWNLIST; style|=(type==0?CBS_SIMPLE:(type==1?CBS_DROPDOWN:CBS_DROPDOWNLIST)); SetWindowLongPtrW(window,GWL_STYLE,style); }
    }
    else if(PropertyEquals(property,L"\u9644\u4ef6\u7c7b\u578b")&&ClassEquals(window,L"SysDateTimePick32")) {
        LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); if(ToInteger(value)==1)style|=DTS_UPDOWN;else style&=~DTS_UPDOWN;SetWindowLongPtrW(window,GWL_STYLE,style);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
    }
    else if(PropertyEquals(property,L"\u70ed\u70b9\u8ddf\u8e2a")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"hyperlink")==0){unit->hyperlinkTrack=ToBool(value);if(!unit->hyperlinkTrack)unit->hyperlinkHot=false;InvalidateRect(window,nullptr,TRUE);}
    }
    else if(PropertyEquals(property,L"\u7535\u5b50\u90ae\u7bb1\u5730\u5740")||PropertyEquals(property,L"Internet\u5730\u5740")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"hyperlink")==0)unit->hyperlinkTarget=Wide(ToText(value).c_str());
    }
    else if(PropertyEquals(property,L"\u663e\u793a\u65b9\u5f0f"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"progress")!=0){unit->imageDrawMode=static_cast<int>(ToInteger(value));InvalidateRect(window,nullptr,TRUE);}
    else if(PropertyEquals(property,L"\u5e95\u56fe\u65b9\u5f0f"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&& (std::strcmp(unit->type,"label")==0||std::strcmp(unit->type,"canvas")==0)){unit->backPicMode=static_cast<int>(ToInteger(value));InvalidateRect(window,nullptr,TRUE);}
    else if(PropertyEquals(property,L"\u6548\u679c"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"label")==0){unit->labelEffect=static_cast<int>(ToInteger(value));InvalidateRect(window,nullptr,TRUE);}
    else if(PropertyEquals(property,L"\u6e10\u53d8\u80cc\u666f\u65b9\u5f0f"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"label")==0){unit->gradientBackMode=static_cast<int>(ToInteger(value));InvalidateRect(window,nullptr,TRUE);}
    else if(PropertyEquals(property,L"\u662f\u5426\u81ea\u52a8\u6298\u884c"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"label")==0){unit->labelWordWrap=ToBool(value);InvalidateRect(window,nullptr,TRUE);}
    else if(PropertyEquals(property,L"\u6587\u4ef6\u540d"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"animate")==0){unit->imageFileName=Wide(ToText(value).c_str());Animate_Open(window,unit->imageFileName.c_str());if(unit->playImage)Animate_Play(window,0,-1,unit->imagePlayCount<0?static_cast<UINT>(-1):static_cast<UINT>(unit->imagePlayCount));}
    else if(PropertyEquals(property,L"\u64ad\u653e"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"animate")==0){unit->playImage=ToBool(value);if(unit->playImage)Animate_Play(window,0,-1,unit->imagePlayCount<0?static_cast<UINT>(-1):static_cast<UINT>(unit->imagePlayCount));else Animate_Stop(window);}
    else if(PropertyEquals(property,L"\u64ad\u653e\u6b21\u6570"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"animate")==0){unit->imagePlayCount=static_cast<int>(ToInteger(value));}
    else if(PropertyEquals(property,L"\u5916\u5f62")||PropertyEquals(property,L"\u7ebf\u6761\u6548\u679c")||PropertyEquals(property,L"\u7ebf\u578b")||PropertyEquals(property,L"\u7ebf\u5bbd")||PropertyEquals(property,L"\u7ebf\u6761\u989c\u8272")||PropertyEquals(property,L"\u586b\u5145\u989c\u8272"))if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&std::strcmp(unit->type,"shape")==0){if(PropertyEquals(property,L"\u5916\u5f62"))unit->shape=static_cast<int>(ToInteger(value));else if(PropertyEquals(property,L"\u7ebf\u6761\u6548\u679c"))unit->shapeEffect=static_cast<int>(ToInteger(value));else if(PropertyEquals(property,L"\u7ebf\u578b"))unit->lineStyle=static_cast<int>(ToInteger(value));else if(PropertyEquals(property,L"\u7ebf\u5bbd"))unit->lineWidth=(std::max)(1,static_cast<int>(ToInteger(value)));else if(PropertyEquals(property,L"\u7ebf\u6761\u989c\u8272")){unit->lineColor=static_cast<COLORREF>(ToInteger(value));unit->hasLineColor=true;}else{unit->fillColor=static_cast<COLORREF>(ToInteger(value));unit->hasFillColor=true;}InvalidateRect(window,nullptr,TRUE);}
    else if(PropertyEquals(property,L"\u6587\u672c\u989c\u8272")) {
        if(ClassEquals(window,L"SysMonthCal32"))SendMessageW(window,MCM_SETCOLOR,MCSC_TEXT,static_cast<COLORREF>(ToInteger(value)));
        else if(Unit* unit=UnitFromWindow(window)){unit->textColor=static_cast<COLORREF>(ToInteger(value));unit->hasTextColor=true;InvalidateRect(window,nullptr,TRUE);}
    }
    else if(PropertyEquals(property,L"\u80cc\u666f\u989c\u8272")||PropertyEquals(property,L"\u753b\u677f\u80cc\u666f\u8272")) {
        if(ClassEquals(window,L"SysMonthCal32"))SendMessageW(window,MCM_SETCOLOR,MCSC_MONTHBK,static_cast<COLORREF>(ToInteger(value)));
        else if(Unit* unit=UnitFromWindow(window)){if(unit->type!=nullptr&&std::strcmp(unit->type,"tab")==0){unit->tabBackColor=static_cast<COLORREF>(ToInteger(value));unit->tabFillBack=true;}else{if(unit->backBrush!=nullptr){DeleteObject(unit->backBrush);unit->backBrush=nullptr;}unit->backColor=static_cast<COLORREF>(ToInteger(value));unit->hasBackColor=true;}InvalidateRect(window,nullptr,TRUE);}
    }
    else if(PropertyEquals(property,L"\u5185\u80cc\u666f\u989c\u8272")||PropertyEquals(property,L"\u6807\u9898\u989c\u8272")||PropertyEquals(property,L"\u6807\u9898\u80cc\u666f\u989c\u8272")||PropertyEquals(property,L"\u975e\u672c\u6708\u989c\u8272")) {
        if(ClassEquals(window,L"SysMonthCal32")) {
            const int colorIndex=PropertyEquals(property,L"\u6807\u9898\u989c\u8272")?MCSC_TITLETEXT:(PropertyEquals(property,L"\u6807\u9898\u80cc\u666f\u989c\u8272")?MCSC_TITLEBK:(PropertyEquals(property,L"\u975e\u672c\u6708\u989c\u8272")?MCSC_TRAILINGTEXT:MCSC_MONTHBK));
            SendMessageW(window,MCM_SETCOLOR,colorIndex,static_cast<COLORREF>(ToInteger(value)));
        }
    }
    else if(PropertyEquals(property,L"\u901a\u5e38")||PropertyEquals(property,L"\u5b58\u6863")||PropertyEquals(property,L"\u53ea\u8bfb")||PropertyEquals(property,L"\u7cfb\u7edf")||PropertyEquals(property,L"\u9690\u85cf")) {
        if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"file")==0) {
            const bool enabled=ToBool(value); if(PropertyEquals(property,L"\u901a\u5e38"))unit->allowNormal=enabled; else if(PropertyEquals(property,L"\u5b58\u6863"))unit->allowArchive=enabled; else if(PropertyEquals(property,L"\u53ea\u8bfb"))unit->allowReadOnly=enabled; else if(PropertyEquals(property,L"\u7cfb\u7edf"))unit->allowSystem=enabled; else unit->allowHidden=enabled;
            PopulatePathListRuntime(window,*unit,false);
        }
    }
    else if(PropertyEquals(property,L"\u5355\u9009")&&IsCheckList(window)) {
        if(Unit* unit=UnitFromWindow(window))unit->checkOnlyOne=ToBool(value);
    }
    else if(PropertyEquals(property,L"\u73b0\u884c\u9009\u4e2d\u9879")) {
        if(ClassEquals(window,L"LISTBOX")) {
            const LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);
            if((style&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))==0)SendMessageW(window,LB_SETCURSEL,ToInteger(value),0);
        }
        else if(ClassEquals(window,L"COMBOBOX"))SendMessageW(window,CB_SETCURSEL,ToInteger(value),0);
    }
    else if(PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e")||PropertyEquals(property,L"\u6700\u5927\u4f4d\u7f6e")) {
        const int valueInt=static_cast<int>(ToInteger(value));
        if(ClassEquals(window,L"msctls_progress32")) { PBRANGE range{};SendMessageW(window,PBM_GETRANGE,TRUE,reinterpret_cast<LPARAM>(&range)); const int minimum=PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e")?valueInt:range.iLow; const int maximum=PropertyEquals(property,L"\u6700\u5927\u4f4d\u7f6e")?valueInt:range.iHigh;SendMessageW(window,PBM_SETRANGE32,minimum,maximum); }
        else if(ClassEquals(window,L"msctls_trackbar32")) { const int minimum=PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e")?valueInt:static_cast<int>(SendMessageW(window,TBM_GETRANGEMIN,0,0)); const int maximum=PropertyEquals(property,L"\u6700\u5927\u4f4d\u7f6e")?valueInt:static_cast<int>(SendMessageW(window,TBM_GETRANGEMAX,0,0));SendMessageW(window,TBM_SETRANGE,TRUE,MAKELONG(minimum,maximum)); }
        else if(ClassEquals(window,L"SCROLLBAR"))if(Unit* unit=UnitFromWindow(window)){if(PropertyEquals(property,L"\u6700\u5c0f\u4f4d\u7f6e"))unit->scrollMin=valueInt;else unit->scrollMax=valueInt;SCROLLINFO info{sizeof(SCROLLINFO),SIF_RANGE|SIF_PAGE|SIF_POS,unit->scrollMin,unit->scrollMax,static_cast<UINT>(unit->scrollPage),0,0};GetScrollInfo(window,SB_CTL,&info);info.nMin=unit->scrollMin;info.nMax=unit->scrollMax;SetScrollInfo(window,SB_CTL,&info,TRUE);}
    }
    else if(PropertyEquals(property,L"\u9875\u6539\u53d8\u503c")||PropertyEquals(property,L"\u884c\u6539\u53d8\u503c")) {
        const int valueInt=(std::max)(0,static_cast<int>(ToInteger(value)));
        if(ClassEquals(window,L"msctls_trackbar32"))SendMessageW(window,PropertyEquals(property,L"\u9875\u6539\u53d8\u503c")?TBM_SETPAGESIZE:TBM_SETLINESIZE,0,valueInt);
        else if(ClassEquals(window,L"SCROLLBAR"))if(Unit* unit=UnitFromWindow(window)){if(PropertyEquals(property,L"\u9875\u6539\u53d8\u503c"))unit->scrollPage=valueInt;else unit->scrollLine=valueInt;}
    }
    else if(PropertyEquals(property,L"\u73b0\u884c\u5b50\u5939")&&ClassEquals(window,L"SysTabControl32")) {
        const int selected=static_cast<int>(ToInteger(value));
        const int count=TabCtrl_GetItemCount(window);
        if(selected>=0&&selected<count) {
            Value result;
            if(!DispatchNativeResult(window,native_selection_changing,TCN_SELCHANGING,{},&result) || result.type!=T_BOOL || ToBool(result)) {
                const int old=static_cast<int>(SendMessageW(window,TCM_GETCURSEL,0,0));
                SendMessageW(window,TCM_SETCURSEL,selected,0);
                if(old!=selected) { UpdateTabVisibility(id);DispatchNative(window,native_selection_changed,TCN_SELCHANGE); }
            }
        }
    }
    else if(PropertyEquals(property,L"\u4eca\u5929")) {
        SYSTEMTIME date{};if(OleDateSystemTime(ToNumber(value),date))SetWindowDateValue(window,date);
    }
    else if(PropertyEquals(property,L"\u6700\u5c0f\u65e5\u671f")||PropertyEquals(property,L"\u6700\u5927\u65e5\u671f")) {
        if(ClassEquals(window,L"SysDateTimePick32")) {
            SYSTEMTIME date{};if(OleDateSystemTime(ToNumber(value),date)) {
                SYSTEMTIME oldMinimum{},oldMaximum{};GetDateRange(window,&oldMinimum,&oldMaximum);
                SetDateRange(window,PropertyEquals(property,L"\u6700\u5c0f\u65e5\u671f")?&date:&oldMinimum,PropertyEquals(property,L"\u6700\u5927\u65e5\u671f")?&date:&oldMaximum);
            }
        }
        else if(ClassEquals(window,L"SysMonthCal32")) {
            SYSTEMTIME date{};if(OleDateSystemTime(ToNumber(value),date)) {
                SYSTEMTIME range[2]{};SendMessageW(window,MCM_GETRANGE,0,reinterpret_cast<LPARAM>(range));
                const bool minimum=PropertyEquals(property,L"\u6700\u5c0f\u65e5\u671f");range[minimum?0:1]=date;
                SendMessageW(window,MCM_SETRANGE,minimum?GDTR_MIN:GDTR_MAX,reinterpret_cast<LPARAM>(range));
            }
        }
        else if(Unit* unit=UnitFromWindow(window);unit!=nullptr&&unit->type!=nullptr&&std::strcmp(unit->type,"file")==0) {
            SYSTEMTIME date{}; if(OleDateSystemTime(ToNumber(value),date)) { if(PropertyEquals(property,L"\u6700\u5c0f\u65e5\u671f")){unit->hasMinDate=true;unit->minDate=date;}else{unit->hasMaxDate=true;unit->maxDate=date;} PopulatePathListRuntime(window,*unit,false); }
        }
    }
    else if(PropertyEquals(property,L"\u9996\u9009\u62e9\u65e5")||PropertyEquals(property,L"\u5c3e\u9009\u62e9\u65e5")) {
        if(ClassEquals(window,L"SysMonthCal32")) {
            SYSTEMTIME date{};if(OleDateSystemTime(ToNumber(value),date)) {SYSTEMTIME range[2]{};SendMessageW(window,MCM_GETSELRANGE,0,reinterpret_cast<LPARAM>(range));range[PropertyEquals(property,L"\u5c3e\u9009\u62e9\u65e5")?1:0]=date;SendMessageW(window,MCM_SETSELRANGE,0,reinterpret_cast<LPARAM>(range));}
        }
    }
    else if(PropertyEquals(property,L"\u9996\u9009\u62e9\u4f4d\u7f6e"))SendMessageW(window,TBM_SETSELSTART,TRUE,ToInteger(value));
    else if(PropertyEquals(property,L"\u9009\u62e9\u957f\u5ea6"))SendMessageW(window,TBM_SETSELEND,TRUE,TrackSelectionStart(window)+static_cast<int>(ToInteger(value)));
    else if(PropertyEquals(property,L"\u6700\u591a\u9009\u62e9\u5929\u6570")&&ClassEquals(window,L"SysMonthCal32"))SendMessageW(window,MCM_SETMAXSELCOUNT,ToInteger(value),0);
    else if(PropertyEquals(property,L"\u6eda\u52a8\u6708\u6570")&&ClassEquals(window,L"SysMonthCal32"))SendMessageW(window,MCM_SETMONTHDELTA,ToInteger(value),0);
    else if(PropertyEquals(property,L"\u5f00\u59cb\u661f\u671f\u9996\u65e5")&&ClassEquals(window,L"SysMonthCal32"))SendMessageW(window,MCM_SETFIRSTDAYOFWEEK,0,ToInteger(value));
    else if(PropertyEquals(property,L"\u5141\u8bb8\u9009\u62e9\u591a\u5929")&&ClassEquals(window,L"SysMonthCal32")) { LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); if(ToBool(value))style|=MCS_MULTISELECT;else style&=~MCS_MULTISELECT; SetWindowLongPtrW(window,GWL_STYLE,style); }
    else if(ClassEquals(window,L"SysMonthCal32")&&PropertyEquals(property,L"\u4e0d\u663e\u793a\u4eca\u5929")) { LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); if(ToBool(value))style|=MCS_NOTODAY;else style&=~MCS_NOTODAY; SetWindowLongPtrW(window,GWL_STYLE,style); }
    else if(ClassEquals(window,L"SysMonthCal32")&&PropertyEquals(property,L"\u4e0d\u5708\u6ce8\u4eca\u5929")) { LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); if(ToBool(value))style|=MCS_NOTODAYCIRCLE;else style&=~MCS_NOTODAYCIRCLE; SetWindowLongPtrW(window,GWL_STYLE,style); }
    else if(ClassEquals(window,L"SysMonthCal32")&&PropertyEquals(property,L"\u663e\u793a\u661f\u671f\u5e8f\u53f7")) { LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE); if(ToBool(value))style|=MCS_WEEKNUMBERS;else style&=~MCS_WEEKNUMBERS; SetWindowLongPtrW(window,GWL_STYLE,style); }
    else if(ClassEquals(window,L"SysMonthCal32")&&PropertyEquals(property,L"\u6807\u9898\u989c\u8272"))SendMessageW(window,MCM_SETCOLOR,MCSC_TITLETEXT,static_cast<COLORREF>(ToInteger(value)));
    else if(ClassEquals(window,L"SysMonthCal32")&&PropertyEquals(property,L"\u6807\u9898\u80cc\u666f\u989c\u8272"))SendMessageW(window,MCM_SETCOLOR,MCSC_TITLEBK,static_cast<COLORREF>(ToInteger(value)));
    else if(ClassEquals(window,L"SysMonthCal32")&&PropertyEquals(property,L"\u5185\u80cc\u666f\u8272"))SendMessageW(window,MCM_SETCOLOR,MCSC_MONTHBK,static_cast<COLORREF>(ToInteger(value)));
    else if(ClassEquals(window,L"SysMonthCal32")&&PropertyEquals(property,L"\u975e\u672c\u6708\u989c\u8272"))SendMessageW(window,MCM_SETCOLOR,MCSC_TRAILINGTEXT,static_cast<COLORREF>(ToInteger(value)));
    else if(PropertyEquals(property,L"\u8d77\u59cb\u9009\u62e9\u4f4d\u7f6e")) {
        HWND edit=EditPart(window); if(edit!=nullptr) { DWORD start=0,end=0;SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));const int position=static_cast<int>(ToInteger(value));SendMessageW(edit,EM_SETSEL,position<0?-1:position,position<0?-1:static_cast<int>(end)); }
    }
    else if(PropertyEquals(property,L"\u88ab\u9009\u62e9\u5b57\u7b26\u6570")) {
        HWND edit=EditPart(window); if(edit!=nullptr) { DWORD start=0,end=0;SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));const int length=static_cast<int>(ToInteger(value));SendMessageW(edit,EM_SETSEL,start,length<0?-1:static_cast<int>(start)+length); }
    }
    else if(PropertyEquals(property,L"\u88ab\u9009\u62e9\u6587\u672c")) {
        HWND edit=EditPart(window); if(edit!=nullptr) { const std::wstring text=Wide(ToText(value).c_str());SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(text.c_str())); }
    }
    else if(PropertyEquals(property,L"\u4f4d\u7f6e"))SetPosition(window,static_cast<int>(ToInteger(value)));
    else if(PropertyEquals(property,L"\u5de6\u8fb9")||PropertyEquals(property,L"\u9876\u8fb9")||
        PropertyEquals(property,L"\u5bbd\u5ea6")||PropertyEquals(property,L"\u9ad8\u5ea6"))SetWindowGeometry(window,property,value);
}
)CPP";
		bool hasSpecs = false;
		const auto typeToken = [](const std::string& type) {
			const std::string_view token = NativeWindowControlToken(type);
			return token.empty() ? "unsupported" : token.data();
		};
		for (const auto& form : program_.windows) {
			if (!form.attributes.empty()) {
				prefix << "static const XmlAttribute xml_form_attributes_" << form.id << "[]={";
				for (const auto& [name, value] : form.attributes) {
					prefix << "{" << EscapeCppString(name) << "," << EscapeCppString(value) << "},";
				}
				prefix << "};\n";
			}
			for (const auto& control : form.controls) {
				if (!HasNativeWin32Class(control.typeName)) continue;
				if (!control.attributes.empty()) {
					prefix << "static const XmlAttribute xml_attributes_" << control.id << "[]={";
					for (const auto& [name, value] : control.attributes) {
						prefix << "{" << EscapeCppString(name) << "," << EscapeCppString(value) << "},";
					}
					prefix << "};\n";
				}
				if (control.listItemsDefined && !control.listItems.empty()) {
					prefix << "static const char* xml_items_" << control.id << "[]={";
					for (const auto& item : control.listItems) prefix << EscapeCppString(item) << ",";
					prefix << "};\n";
				}
				if (control.itemValuesDefined && !control.itemValues.empty()) {
					prefix << "static const int xml_item_values_" << control.id << "[]={";
					for (const int value : control.itemValues) prefix << value << ",";
					prefix << "};\n";
				}
				if (control.itemCheckedDefined && !control.itemChecked.empty()) {
					prefix << "static const unsigned char xml_item_checked_" << control.id << "[]={";
					for (const bool value : control.itemChecked) prefix << (value ? "1" : "0") << ",";
					prefix << "};\n";
				}
				if (control.itemEnabledDefined && !control.itemEnabled.empty()) {
					prefix << "static const unsigned char xml_item_enabled_" << control.id << "[]={";
					for (const bool value : control.itemEnabled) prefix << (value ? "1" : "0") << ",";
					prefix << "};\n";
				}
			}
		}
		prefix << "static const Spec specs[]={\n";
		for (const auto& form : program_.windows) {
			for (const auto& control : form.controls) {
				if (!HasNativeWin32Class(control.typeName)) continue;
				const char* token = typeToken(control.typeName);
				// Only controls with an explicit native Win32 mapping are emitted.
				// Unknown support-library units (including provider/database/grid
				// controls) must not be fabricated as STATIC or container windows.
				if (std::strcmp(token, "unsupported") == 0) continue;
				hasSpecs = true;
				const std::string attributes = control.attributes.empty() ? "nullptr" : "xml_attributes_" + std::to_string(control.id);
				const std::string items = control.listItemsDefined && !control.listItems.empty()
					? "xml_items_" + std::to_string(control.id) : "nullptr";
				const std::string itemValues = control.itemValuesDefined && !control.itemValues.empty()
					? "xml_item_values_" + std::to_string(control.id) : "nullptr";
				const std::string itemChecked = control.itemCheckedDefined && !control.itemChecked.empty()
					? "xml_item_checked_" + std::to_string(control.id) : "nullptr";
				const std::string itemEnabled = control.itemEnabledDefined && !control.itemEnabled.empty()
					? "xml_item_enabled_" + std::to_string(control.id) : "nullptr";
				prefix << "{" << control.id << "u," << form.id << "u," << control.parentId << "u," << control.left << "," << control.top << "," << control.width << "," << control.height << "," << (control.visible ? "true" : "false") << "," << (control.disabled ? "true" : "false") << "," << (control.tabStop ? "true" : "false") << "," << control.tabOwner << "," << control.tabPage << "," << control.tabPageTitles.size() << "," << control.tabCurrentPage << "," << EscapeCppString(token) << "," << EscapeCppString(control.text) << "," << attributes << "," << control.attributes.size() << "," << items << "," << (control.listItemsDefined ? control.listItems.size() : 0) << "," << itemValues << "," << (control.itemValuesDefined ? control.itemValues.size() : 0) << "," << itemChecked << "," << (control.itemCheckedDefined ? control.itemChecked.size() : 0) << "," << itemEnabled << "," << (control.itemEnabledDefined ? control.itemEnabled.size() : 0) << "},\n";
			}
		}
		if (!hasSpecs) prefix << "{0u,0u,0u,0,0,0,0,false,false,false,0,-1,0,0,\"unsupported\",\"\",nullptr,0,nullptr,0,nullptr,0,nullptr,0,nullptr,0},\n";
		prefix << R"CPP(};
static void Initialize() {
    initializing=true;
    INITCOMMONCONTROLSEX common{sizeof(INITCOMMONCONTROLSEX),ICC_WIN95_CLASSES|ICC_DATE_CLASSES|ICC_BAR_CLASSES|ICC_TAB_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSW klass{};klass.hInstance=instance;klass.lpfnWndProc=FormProc;klass.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(IDC_ARROW));klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);klass.lpszClassName=L"ecompiler_window_form";RegisterClassW(&klass);
    WNDCLASSW containerClass{};containerClass.hInstance=instance;containerClass.lpfnWndProc=ContainerProc;containerClass.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(IDC_ARROW));containerClass.lpszClassName=L"ecompiler_window_container";RegisterClassW(&containerClass);
)CPP";
		for (const auto& form : program_.windows) {
			prefix << "    DWORD formStyle_" << form.id << "=FormStyle(" << form.border << "," << (form.controlButtons ? "true" : "false") << "," << (form.maximizeButton ? "true" : "false") << "," << (form.minimizeButton ? "true" : "false") << ");DWORD formExStyle_" << form.id << "=FormExtendedStyle(" << (form.showInTaskbar ? "true" : "false") << ");RECT formRect_" << form.id << "{0,0," << form.width << "," << form.height << "};AdjustWindowRectEx(&formRect_" << form.id << ",formStyle_" << form.id << ",FALSE,formExStyle_" << form.id << ");HWND form_" << form.id << "=CreateWindowExW(formExStyle_" << form.id << ",L\"ecompiler_window_form\",Wide(" << EscapeCppString(form.title) << ").c_str(),formStyle_" << form.id << "," << form.left << "," << form.top << ",formRect_" << form.id << ".right-formRect_" << form.id << ".left,formRect_" << form.id << ".bottom-formRect_" << form.id << ".top,nullptr,nullptr,instance,nullptr);if(form_" << form.id << "==nullptr)return;ApplyDefaultFont(form_" << form.id << ");forms.push_back(Form{" << form.id << "u,form_" << form.id << "," << (form.escapeCloses ? "true" : "false") << "," << (form.canMove ? "true" : "false") << ",false});if(auto* stored=FindFormRecord(form_" << form.id << ")){stored->enterToNext=" << (form.enterToNext ? "true" : "false") << ";stored->f1OpenHelp=" << (form.f1OpenHelp ? "true" : "false") << ";stored->hitMove=" << (form.hitMove ? "true" : "false") << ";stored->topmost=" << (form.topmost ? "true" : "false") << ";stored->keepTitleBarActive=" << (form.keepTitleBarActive ? "true" : "false") << ";stored->showInTaskbar=" << (form.showInTaskbar ? "true" : "false") << ";stored->border=" << form.border << ";stored->controlButtons=" << (form.controlButtons ? "true" : "false") << ";stored->maximizeButton=" << (form.maximizeButton ? "true" : "false") << ";stored->minimizeButton=" << (form.minimizeButton ? "true" : "false") << ";stored->shape=" << form.shape << ";stored->hasBackColor=" << (form.hasBackColor ? "true" : "false") << ";stored->backColor=static_cast<COLORREF>(" << form.backColor << ");stored->helpFileName=Wide(" << EscapeCppString(form.helpFileName) << ");stored->helpContext=" << form.helpContext << ";StoreSpecProperties(stored->properties," << (form.attributes.empty() ? "nullptr" : "xml_form_attributes_" + std::to_string(form.id)) << "," << form.attributes.size() << ");}PlaceForm(form_" << form.id << "," << form.left << "," << form.top << "," << form.position << ");if(" << (form.topmost ? "true" : "false") << ")SetWindowPos(form_" << form.id << ",HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);if(" << (form.disabled ? "true" : "false") << ")EnableWindow(form_" << form.id << ",FALSE);\n";
		}
		prefix << R"CPP(    constexpr std::size_t count=sizeof(specs)/sizeof(specs[0]);
    for(std::size_t pass=0;pass<count+1;++pass)for(const auto& spec:specs)if(spec.id!=0&&spec.tabOwner==0&&FindUnit(spec.id)==nullptr&& (spec.parent==0||FindUnit(spec.parent)!=nullptr))CreateUnit(spec);
)CPP";
		for (const auto& form : program_.windows) {
			for (const auto& control : form.controls) {
				if (!HasNativeWin32Class(control.typeName) || control.typeName != "选择夹") continue;
				for (std::size_t page = 0; page < control.tabPageTitles.size(); ++page) {
				prefix << "    if(auto* tab_" << control.id << "=FindUnit(" << control.id << "u)){TCITEMW item_" << control.id << "_" << page << "{};item_" << control.id << "_" << page << ".mask=TCIF_TEXT;std::wstring title_" << control.id << "_" << page << "=Wide(" << EscapeCppString(control.tabPageTitles[page]) << ");item_" << control.id << "_" << page << ".pszText=title_" << control.id << "_" << page << ".data();TabCtrl_InsertItem(tab_" << control.id << "->hwnd," << page << ",&item_" << control.id << "_" << page << ");}\n";
				}
				prefix << "    if(auto* tab_" << control.id << "=FindUnit(" << control.id << "u)){const int tab_count_" << control.id << "=TabCtrl_GetItemCount(tab_" << control.id << "->hwnd);const int tab_page_" << control.id << "=" << control.tabCurrentPage << ";if(tab_count_" << control.id << ">0)TabCtrl_SetCurSel(tab_" << control.id << "->hwnd,(std::max)(0,(std::min)(tab_page_" << control.id << ",tab_count_" << control.id << "-1)));}\n";
			}
		}
		prefix << R"CPP(    for(std::size_t pass=0;pass<count+1;++pass)for(const auto& spec:specs)if(spec.id!=0&&spec.tabOwner!=0&&FindUnit(spec.id)==nullptr&& (spec.parent==0||FindUnit(spec.parent)!=nullptr))CreateUnit(spec);
)CPP";
		for (const auto& form : program_.windows) {
			for (const auto& control : form.controls) {
				if (!HasNativeWin32Class(control.typeName) || control.typeName != "选择夹") continue;
				prefix << "    UpdateTabVisibility(" << control.id << "u);\n";
			}
		}
		for (const auto& form : program_.windows) {
			prefix << "    Dispatch(" << form.id << "u,native_created,0,{});\n";
			const char* showCommand = !form.visible ? "SW_HIDE" : (form.position == 2 ? "SW_MINIMIZE" : (form.position == 3 ? "SW_MAXIMIZE" : "SW_SHOW"));
			const bool hasIdle = std::any_of(form.events.begin(), form.events.end(), [](const WindowEventBinding& event) { return event.trigger == WindowEventTrigger::Idle; });
			prefix << "    ShowWindow(form_" << form.id << "," << showCommand << ");UpdateWindow(form_" << form.id << ");";
			if (hasIdle) prefix << "if(auto* idleForm=FindFormRecord(form_" << form.id << ")){idleForm->idleSince=GetTickCount();SetTimer(form_" << form.id << ",0xE1D1u,50,nullptr);} ";
			prefix << "\n";
		}
		prefix << "    initializing=false;\n";
		prefix << R"CPP(}
}
static ert::Value WindowGetProperty(unsigned int id,const char* property){return ecompiler_window_host::GetProperty(id,property);}
static void WindowSetProperty(unsigned int id,const char* property,const ert::Value& value){ecompiler_window_host::SetProperty(id,property,value);}
static ert::Value WindowInvokeMember(unsigned int id,const char* operation,std::vector<ert::Value> args){return ecompiler_window_host::WindowInvokeMember(id,operation,std::move(args));}
static void EWindowInitialize(){ecompiler_window_host::Initialize();}
)CPP";
	}

	void EmitDeclarationsAndStartup()
	{
		std::ostringstream prefix;
		const bool targetX64 = program_.targetArchitecture == TargetArchitecture::X64;
		if (!program_.buildDll) {
			// Match the IDE's default EXE manifest and activate common-controls 6.0.
			prefix << "#pragma comment(linker,\"/manifestdependency:\\\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\\\"\")\n";
		}
		prefix << "\n";
		for (const std::size_t method : pendingMethods_) {
			prefix << "static ert::Value method_" << method
				<< "(std::vector<ert::Arg>,ert::Value* self=nullptr);\n";
		}
		for (const std::string& symbol : reachableSymbols_) {
			prefix << "extern \"C\" void __cdecl " << symbol
				<< "(ert::MData*,int,ert::MData*);\n";
		}
		for (const std::size_t libraryIndex : reachableLibraries_) {
			const std::string& notify = program_.libraries[libraryIndex].metadata.notifySymbol;
			if (!notify.empty() && IsCppIdentifier(notify)) {
				prefix << "extern \"C\" ert::EIntPtr __stdcall " << notify
					<< "(int,ert::EPointer,ert::EPointer);\n";
			}
		}
		if (!program_.windows.empty()) {
			prefix << "static ert::Value WindowGetProperty(unsigned int,const char*);\n";
			prefix << "static void WindowSetProperty(unsigned int,const char*,const ert::Value&);\n";
			prefix << "static ert::Value WindowInvokeMember(unsigned int,const char*,std::vector<ert::Value>);\n";
		}
		prefix << declarations_.str() << body_.str();
		EmitWindowRuntime(prefix);
		prefix << "\nextern \"C\" ert::EPointer BlackMoonFuncForeLib="
			<< "reinterpret_cast<ert::EPointer>(&ert::BlackMoonFuncForeLibNotifySys);\n";
		prefix << R"CPP(
namespace ecompiler_runtime_host {
static std::unordered_set<void*> files;
static char modulePath[MAX_PATH]{};
static const char* ModuleDirectory() {
    DWORD length=GetModuleFileNameA(nullptr,modulePath,MAX_PATH);
    if(length==0)return "";
    modulePath[length]=0;
    char* slash=strrchr(modulePath,'\\');
    if(slash)*slash=0;
    return modulePath;
}
static const char* ModuleFileName() {
    DWORD length=GetModuleFileNameA(nullptr,modulePath,MAX_PATH);
    if(length==0)return "";
    modulePath[length]=0;
    char* slash=strrchr(modulePath,'\\');
    return slash?slash+1:modulePath;
}
static const char* CommandTail() {
    const char* text=GetCommandLineA();
    if(text==nullptr)return "";
    if(*text=='"'){++text;while(*text&&*text!='"')++text;if(*text)++text;}
    else while(*text&&*text!=' ')++text;
    while(*text==' ')++text;
    return text;
}
static void FreeArray(ert::EPointer type,void* value) {
    if(value==nullptr)return;
    auto* raw=static_cast<unsigned char*>(value);
    const int dimensions=*reinterpret_cast<int*>(raw);
    int count=1;
    for(int index=0;index<dimensions;++index)
        count*=*reinterpret_cast<int*>(raw+4+index*4);
    auto* data=raw+4+dimensions*4;
    const auto base=type&~ert::T_ARRAY;
    if(base==ert::T_TEXT||base==ert::T_BIN) {
        for(int index=0;index<count;++index) {
            void* item=*reinterpret_cast<void**>(data+index*sizeof(void*));
            if(item)HeapFree(GetProcessHeap(),0,item);
        }
    }
    HeapFree(GetProcessHeap(),0,raw);
}
}
extern "C" ert::EIntPtr __stdcall BlackMoonFuncForeLibNotifySys(
    int message,ert::EPointer param1,ert::EPointer param2) {
    using namespace ecompiler_runtime_host;
    switch(message) {
    case ecompiler_nrs_get_cmd_line:
        return reinterpret_cast<ert::EIntPtr>(CommandTail());
    case ecompiler_nrs_get_exe_path:
        return reinterpret_cast<ert::EIntPtr>(ModuleDirectory());
    case ecompiler_nrs_get_exe_name:
        return reinterpret_cast<ert::EIntPtr>(ModuleFileName());
    case ecompiler_nrs_convert_num_to_int: {
        const ert::MData* data=reinterpret_cast<const ert::MData*>(param1);
        if(data==nullptr)return 0;
        switch(data->type&~ert::T_ARRAY) {
        case ert::T_BYTE:return static_cast<int>(data->byteValue);
        case ert::T_SHORT:return static_cast<int>(data->shortValue);
        case ert::T_INT:case ert::T_BOOL:return data->intValue;
        case ert::T_INT64:return static_cast<int>(data->int64Value);
        case ert::T_FLOAT:return static_cast<int>(data->floatValue);
        case ert::T_DOUBLE:case ert::T_DATE:return static_cast<int>(data->doubleValue);
        default:return 0;
        }
    }
    case ecompiler_nrs_file_check:
        return files.contains(reinterpret_cast<void*>(param1))?0:-1;
    case ecompiler_nrs_file_register:
        if(param1)files.insert(reinterpret_cast<void*>(param1));
        return 0;
    case ecompiler_nrs_file_unregister:
        return files.erase(reinterpret_cast<void*>(param1))?1:0;
    case ecompiler_nrs_free_array:
        FreeArray(param1,reinterpret_cast<void*>(param2));
        return 0;
    case ecompiler_nrs_malloc:
        return reinterpret_cast<ert::EIntPtr>(
            HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,static_cast<SIZE_T>(param1)));
    case ecompiler_nrs_mfree:
        if(param1)HeapFree(GetProcessHeap(),0,reinterpret_cast<void*>(param1));
        return 0;
    case ecompiler_nrs_mrealloc:
        return reinterpret_cast<ert::EIntPtr>(
            param1
                ? HeapReAlloc(GetProcessHeap(),0,reinterpret_cast<void*>(param1),static_cast<SIZE_T>(param2))
                : HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,static_cast<SIZE_T>(param2)));
    case ecompiler_nrs_get_program_type:
        return 3;
    case ecompiler_nrs_exit_program:
        ExitProcess(static_cast<UINT>(param1));
        return 0;
    default:
        return 0;
    }
}
)CPP";
		prefix << "extern \"C\" void* __cdecl krnl_MMalloc(unsigned int size){return HeapAlloc(GetProcessHeap(),0,size);}\n";
		prefix << "extern \"C\" void* __cdecl krnl_MMallocNoCheck(unsigned int size){return HeapAlloc(GetProcessHeap(),0,size);}\n";
		prefix << "extern \"C\" void* __cdecl krnl_MRealloc(void* value,unsigned int size){return value?HeapReAlloc(GetProcessHeap(),0,value,size):HeapAlloc(GetProcessHeap(),0,size);}\n";
		prefix << "extern \"C\" void __cdecl krnl_MFree(void* value){if(value)HeapFree(GetProcessHeap(),0,value);}\n";
		prefix << "extern \"C\" ert::EPointer BlackMoonCalleLibList[]={";
		for (const std::size_t libraryIndex : reachableLibraries_) {
			const std::string& notify = program_.libraries[libraryIndex].metadata.notifySymbol;
			if (!notify.empty() && IsCppIdentifier(notify)) {
				prefix << "reinterpret_cast<ert::EPointer>(&" << notify << "),";
			}
		}
		prefix << "0};\n";
		prefix << "extern \"C\" void* hBlackMoonHeap=GetProcessHeap();\n";
		prefix << "namespace ecompiler_runtime_host { static std::vector<void(__cdecl*)()> destroyCallbacks; }\n";
		prefix << "extern \"C\" void __cdecl E_Destroy(void(__cdecl* callback)()){if(callback) ecompiler_runtime_host::destroyCallbacks.push_back(callback);}\n";
		prefix << "extern \"C\" void* __cdecl E_MAlloc(unsigned int size){return HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,size);}\n";
		prefix << "extern \"C\" void* __cdecl E_MAlloc_Nzero(unsigned int size){return HeapAlloc(GetProcessHeap(),0,size);}\n";
		prefix << "extern \"C\" void* __cdecl E_Realloc(void* value,unsigned int size){return value?HeapReAlloc(GetProcessHeap(),0,value,size):HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,size);}\n";
		prefix << "extern \"C\" void __cdecl E_MFree(void* value){if(value)HeapFree(GetProcessHeap(),0,value);}\n";
		prefix << "extern \"C\" void __cdecl E_ReportError(unsigned int,unsigned int,unsigned int){OutputDebugStringA(\"ecompiler: support library runtime error\\r\\n\");}\n";
		prefix << "extern \"C\" void __cdecl E_End(unsigned int code){ExitProcess(code);}\n";
		prefix << "enum : unsigned int { ecompiler_program_release = 3u };\n";
		prefix << "extern \"C\" volatile unsigned int eapp_info_data[13]={ecompiler_program_release,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u};\n";
		prefix << "extern \"C\" int __cdecl cominf_LoadLibrary(){return 0;}\nextern \"C\" int __cdecl cominf_GetProcAddress(){return 0;}\nextern \"C\" void __cdecl cominf_FreeLibrary(){}\n";
		prefix << "extern \"C\" int __cdecl EStartup(){return 1;}\n";
		prefix << "extern \"C\" int __cdecl ECodeStart(){return static_cast<int>(ert::ToInteger(method_" << program_.methodByName.at("_启动子程序") << "({},nullptr)));}\n";
		if (program_.buildDll) {
			prefix << "extern \"C\" void __cdecl E_Init(); extern \"C\" void __cdecl E_DestroyRes();\n";
			prefix << "extern \"C\" BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID reserved){(void)reserved;if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(module);E_Init();ECodeStart();}else if(reason==DLL_PROCESS_DETACH){E_DestroyRes();}return TRUE;}\n";
		}
		prefix << "extern \"C\" void __cdecl E_Init(){hBlackMoonHeap=GetProcessHeap();using Notify=ert::EIntPtr(__stdcall*)(int,ert::EPointer,ert::EPointer);for(ert::EPointer* item=BlackMoonCalleLibList;item!=nullptr&&*item;++item)reinterpret_cast<Notify>(*item)(ecompiler_nl_sys_notify_function,BlackMoonFuncForeLib,BlackMoonFuncForeLib);}\n";
		prefix << "extern \"C\" void __cdecl E_DestroyRes(){";
		for (std::size_t index = 0; index < program_.globals.size(); ++index) prefix << "ert::DestroyValue(g_global_" << index << "());";
		for (std::size_t assemblyIndex = 0; assemblyIndex < program_.assemblies.size(); ++assemblyIndex)
			for (std::size_t index = 0; index < program_.assemblies[assemblyIndex].variables.size(); ++index)
				prefix << "ert::DestroyValue(g_" << assemblyIndex << '_' << index << "());";
		prefix << "for(auto callback:ecompiler_runtime_host::destroyCallbacks)if(callback)callback();using Notify=ert::EIntPtr(__stdcall*)(int,ert::EPointer,ert::EPointer);for(ert::EPointer* item=BlackMoonCalleLibList;item!=nullptr&&*item;++item)reinterpret_cast<Notify>(*item)(ecompiler_nl_free_library_data,0,0);ecompiler_runtime_host::destroyCallbacks.clear();}\n";
		prefix << "static void ecompiler_safe_destroy(){__try{E_DestroyRes();}__except(EXCEPTION_EXECUTE_HANDLER){}}\n";
		if (!targetX64 && program_.useLegacyX86RuntimeBridge) {
			prefix << "int __stdcall AfxWinInit(HINSTANCE,HINSTANCE,LPSTR,int);\n";
		}
		prefix << "int nBMProtectESP=0;int nBMProtectEBP=0;\n";
		if (!program_.buildDll) {
			if (program_.windowsGui) {
				if (targetX64) {
					if (!program_.windows.empty()) prefix << "int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int){E_Init();if(!EStartup())ExitProcess(1);EWindowInitialize();const int result=ECodeStart();MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}ecompiler_safe_destroy();return result;}\n";
					else prefix << "int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int){E_Init();if(!EStartup())ExitProcess(1);const int result=ECodeStart();ecompiler_safe_destroy();return result;}\n";
				}
				else if (program_.useLegacyX86RuntimeBridge) {
					if (!program_.windows.empty()) prefix << "int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int){ert::InitializeLegacyCrtData();E_Init();if(!EStartup())ExitProcess(1);EWindowInitialize();const int result=ECodeStart();MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}ecompiler_safe_destroy();ExitProcess(static_cast<UINT>(result));}\n";
					else prefix << "int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int){ert::InitializeLegacyCrtData();E_Init();if(!EStartup())ExitProcess(1);const int result=ECodeStart();ecompiler_safe_destroy();ExitProcess(static_cast<UINT>(result));}\n";
				}
				else {
					if (!program_.windows.empty()) prefix << "int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int){ert::InitializeLegacyTimezoneData();E_Init();if(!EStartup())ExitProcess(1);EWindowInitialize();const int result=ECodeStart();MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}ecompiler_safe_destroy();return result;}\n";
					else prefix << "int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int){ert::InitializeLegacyTimezoneData();E_Init();if(!EStartup())ExitProcess(1);const int result=ECodeStart();ecompiler_safe_destroy();return result;}\n";
				}
			}
			else if (targetX64) {
				prefix << "int main(){E_Init();if(!EStartup())ExitProcess(1);const int result=ECodeStart();ecompiler_safe_destroy();return result;}\n";
			}
			else if (program_.useLegacyX86RuntimeBridge) {
				prefix << "int main(){ert::InitializeLegacyCrtData();if(!AfxWinInit(GetModuleHandleA(nullptr),nullptr,GetCommandLineA(),0))ExitProcess(1);E_Init();if(!EStartup())ExitProcess(1);const int result=ECodeStart();ecompiler_safe_destroy();ExitProcess(static_cast<UINT>(result));}\n";
			}
			else {
				prefix << "int main(){ert::InitializeLegacyTimezoneData();E_Init();if(!EStartup())ExitProcess(1);const int result=ECodeStart();ecompiler_safe_destroy();return result;}\n";
			}
		}
		result_->text += prefix.str();
	}
	const Program& program_;
	GeneratedSource* result_ = nullptr;
	std::string* error_ = nullptr;
	std::ostringstream body_;
	std::vector<std::size_t> pendingMethods_;
	std::unordered_set<std::size_t> emittedMethods_;
	std::set<std::size_t> reachableLibraries_;
	std::set<std::pair<std::size_t, std::size_t>> reachableCommands_;
	std::set<std::string> reachableSymbols_;
	std::vector<LifecycleBinding> lifecycle_;
	std::vector<GeneratedSource::ExportedFunction> exports_;
	std::vector<GeneratedSource::ImportedFunction> imports_;
	std::ostringstream declarations_;
	std::unordered_map<std::size_t, std::string> dllImportSymbols_;
	std::unordered_set<std::uint32_t> usedTypes_;
};

}  // namespace

bool EmitCppSource(const Program& program, GeneratedSource& outSource, std::string& outError)
{
	outError.clear();
	Emitter emitter(program);
	return emitter.Run(outSource, outError);
}

}  // namespace ecompiler
