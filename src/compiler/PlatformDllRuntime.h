#pragma once

namespace ecompiler {

// 系统 DLL 使用内联的结构成员和定长数组，不使用易语言对象中的指针槽。
inline constexpr const char* kPlatformDllRuntime = R"CPP(
namespace ert {
static std::size_t PlatformSize(std::uint32_t type);
static std::size_t PlatformFieldCount(const FieldDesc& field) {
    if(!field.array)return 1;
    std::size_t count=1;
    for(int dimension:field.dimensions)count*=static_cast<std::size_t>((std::max)(dimension,0));
    return field.dimensions.size()?count:0;
}
static std::size_t PlatformSize(std::uint32_t type) {
    const auto* desc=FindType(type);
    if(!desc || desc->enumeration)return desc?4:ScalarSize(type);
    std::size_t size=0;
    for(std::size_t i=0;i<desc->fieldCount;++i)size+=PlatformFieldCount(desc->fields[i])*PlatformSize(desc->fields[i].type);
    return size;
}
static void PlatformTransfer(Value& value,unsigned char* memory,bool read) {
    if(const auto* desc=FindType(value.type);desc && !desc->enumeration) {
        std::size_t offset=0;
        for(std::size_t i=0;i<desc->fieldCount;++i) {
            const auto& field=desc->fields[i];auto& target=value.fields[i];
            const auto count=PlatformFieldCount(field),width=PlatformSize(field.type);
            if(field.array) {
                if(target.elements.size()!=count)Redim(target,std::vector<int>(field.dimensions),false);
                for(std::size_t n=0;n<count;++n)PlatformTransfer(target.elements[n],memory+offset+n*width,read);
            } else PlatformTransfer(target,memory+offset,read);
            offset+=count*width;
        }
        return;
    }
    const auto size=PlatformSize(value.type);
    if(value.type==T_TEXT) {
        if(read) { const auto* text=*reinterpret_cast<char**>(memory);value.text=text?text:""; }
        else *reinterpret_cast<char**>(memory)=value.text.data();
    } else if(value.type==T_BIN) {
        if(!read)*reinterpret_cast<void**>(memory)=value.bytes.data();
    } else if(value.type==T_FLOAT) {
        float number=static_cast<float>(value.number);
        if(read) { std::memcpy(&number,memory,4);value.number=number;value.integer=static_cast<long long>(number); }
        else std::memcpy(memory,&number,4);
    } else if(value.type==T_DOUBLE||value.type==T_DATE) {
        if(read) { std::memcpy(&value.number,memory,8);value.integer=static_cast<long long>(value.number); }
        else std::memcpy(memory,&value.number,8);
    } else if(read) {
        value.integer=0;std::memcpy(&value.integer,memory,size);
        if(value.type==T_SHORT)value.integer=static_cast<short>(value.integer);
        else if(size==4)value.integer=static_cast<int>(value.integer);
        value.number=static_cast<double>(value.integer);
    } else { const auto number=ToInteger(value);std::memcpy(memory,&number,size); }
}
static void* PlatformObject(Value& value) {
    if(value.missing && !FindType(value.type))return nullptr;
    value.object.resize((std::max)(PlatformSize(value.type),std::size_t(64)));
    PlatformTransfer(value,value.object.data(),false);
    return value.object.data();
}
static void ReadPlatformObject(Value& value,const void* source) {
    PlatformTransfer(value,static_cast<unsigned char*>(const_cast<void*>(source)),true);
}
} // namespace ert
)CPP";

} // namespace ecompiler
