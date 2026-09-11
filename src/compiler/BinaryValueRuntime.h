#pragma once

namespace ecompiler {

// 字节集读取直接保留实际返回类型，并按易语言规则更新可选索引。
inline constexpr const char* kBinaryValueRuntime = R"CPP(
namespace ert {
static Value ReadMemoryBytes(std::vector<Arg> arguments) {
    const auto* pointer=reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(ToInteger(arguments[0].Get())));
    const auto count=ToInteger(arguments[1].Get());
    return !pointer||count<=0?Bytes({}):Bytes(std::vector<unsigned char>(pointer,pointer+count));
}
static Value ReadBinaryElement(std::vector<Arg> arguments) {
    const auto& bytes=arguments[0].Get().bytes;
    const int kind=static_cast<int>(ToInteger(arguments[1].Get()));
    static const std::uint32_t types[]={T_BYTE,T_SHORT,T_INT,T_INT64,T_FLOAT,T_DOUBLE,T_BOOL,T_DATE,T_SUB,T_TEXT};
    if(kind<1||kind>10) { OutputDebugStringA("ecompiler: invalid binary element type");ExitProcess(87); }
    const auto type=types[kind-1];
    Value value=MakeVar(type);
    const long long offset=arguments.size()<3||arguments[2].Get().missing?0:ToInteger(arguments[2].Get())-1;
    long long next=-1;
    if(offset>=0&&static_cast<unsigned long long>(offset)<bytes.size()) {
        const auto* source=bytes.data()+static_cast<std::size_t>(offset);
        const auto remaining=bytes.size()-static_cast<std::size_t>(offset);
        std::size_t width=ScalarSize(type);
        if(type==T_TEXT) {
            const auto* end=static_cast<const unsigned char*>(std::memchr(source,0,remaining));
            const auto length=end?static_cast<std::size_t>(end-source):remaining;
            value.text.assign(reinterpret_cast<const char*>(source),length);width=length+1;
        } else {
            unsigned char buffer[8]{};std::memcpy(buffer,source,(std::min)(width,remaining));
            if(type==T_FLOAT) {float number;std::memcpy(&number,buffer,4);value.number=number;}
            else if(type==T_DOUBLE||type==T_DATE)std::memcpy(&value.number,buffer,8);
            else {
                std::memcpy(&value.integer,buffer,width);
                if(type==T_SHORT)value.integer=static_cast<short>(value.integer);
                else if(width==4)value.integer=static_cast<int>(value.integer);
                if(type==T_BOOL)value.integer=value.integer!=0;
                value.number=static_cast<double>(value.integer);
            }
        }
        next=offset+static_cast<long long>(width)+1;
        if(next>static_cast<long long>(bytes.size()))next=-1;
    }
    if(arguments.size()>2&&arguments[2].reference&&!arguments[2].Get().missing)Assign(arguments[2].Get(),Integer(next));
    return value;
}
} // namespace ert
)CPP";

} // namespace ecompiler
