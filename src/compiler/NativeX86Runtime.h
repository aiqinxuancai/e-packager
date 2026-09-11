#pragma once

namespace ecompiler {

// 生成程序使用的 x86 栈帧桥接；机器码自行返回和继续执行分别处理。
inline constexpr const char* kNativeX86Runtime = R"CPP(
#if defined(_M_IX86)
namespace ert {
struct NativeRun {
    unsigned char* storage;
    unsigned size, locals;
    void* code;
    void* hostStack=nullptr;
    unsigned char* actual=nullptr;
    unsigned low=0, high=0, returned=0;
    unsigned floating=0;
    double real=0;
};
static thread_local NativeRun* nativeRun=nullptr;
extern "C" unsigned long _tls_index;
static void NativeMachineReturn();
static void NativeMachineFallthrough();
static void NativeMachineComplete();
static void __declspec(naked) __cdecl NativeInvoke(NativeRun* state) {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov ebx, [esp+20]
        mov [ebx+16], esp
        mov eax, [ebx+4]
        add eax, 64
    probe:
        cmp eax, 4096
        jb native_probe_remainder
        sub esp, 4096
        test [esp], eax
        sub eax, 4096
        jmp probe
    native_probe_remainder:
        sub esp, eax
        test [esp], eax
        mov edi, esp
        mov [ebx+20], edi
        mov esi, [ebx]
        mov ecx, [ebx+4]
        cld
        rep movsb
        mov ebp, esp
        add ebp, [ebx+8]
        mov dword ptr [ebp+4], offset NativeMachineReturn
        jmp dword ptr [ebx+12]
    }
}
static void __declspec(naked) NativeMachineFallthrough() {
    __asm { xor edi, edi }
    __asm { jmp NativeMachineComplete }
}
static void __declspec(naked) NativeMachineReturn() {
    __asm { mov edi, 1 }
    __asm { jmp NativeMachineComplete }
}
static void __declspec(naked) NativeMachineComplete() {
    __asm {
        mov ecx, fs:[2Ch]
        mov ebx, _tls_index
        mov ecx, [ecx+ebx*4]
        mov ebx, offset nativeRun
        mov ebx, [ecx+ebx]
        mov [ebx+24], eax
        mov [ebx+28], edx
        mov [ebx+32], edi
        test edi, edi
        jz native_result_saved
        cmp dword ptr [ebx+36], 0
        je native_result_saved
        fstp qword ptr [ebx+40]
    native_result_saved:
        cld
        mov esi, [ebx+20]
        mov edi, [ebx]
        mov ecx, [ebx+4]
        rep movsb
        mov esp, [ebx+16]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
static_assert(offsetof(NativeRun,hostStack)==16 && offsetof(NativeRun,returned)==32);
struct NativeParameter { bool reference, nullable; };
struct NativeFrame;
static thread_local NativeFrame* nativeFrame=nullptr;
extern const void* const* NativeClassTable(std::uint32_t type);
static NativeStorage& NativeStorageFor(Value& value) {
    if(!value.native) value.native=std::make_shared<NativeStorage>();
    value.native->owner=&value;
    return *value.native;
}
static void NativeStore(Value& value,void* destination);
static void NativeRead(Value& value,const void* source);
static unsigned NativeWidth(const Value& value) {
    return !value.declaredArray && (value.type==T_INT64||value.type==T_DOUBLE||value.type==T_DATE)?8:4;
}
static void NativeStore(Value& value,void* destination) {
    std::memset(destination,0,NativeWidth(value));
    if(value.missing)return;
    if(value.declaredArray || value.type==T_BIN || value.type==T_TEXT || FindType(value.type)) {
        auto& memory=NativeStorageFor(value);
        if(!memory.storing) {
            memory.storing=true;
            if(value.type==T_TEXT && !value.declaredArray) memory.pointer=value.text.data();
            else if(value.type==T_BIN && !value.declaredArray) {
                memory.bytes.resize(value.bytes.size()+8);
                *reinterpret_cast<unsigned*>(memory.bytes.data())=1;
                *reinterpret_cast<unsigned*>(memory.bytes.data()+4)=static_cast<unsigned>(value.bytes.size());
                std::copy(value.bytes.begin(),value.bytes.end(),memory.bytes.begin()+8);
                memory.pointer=memory.bytes.data();
            } else if(value.declaredArray) {
                const auto dimensions=value.dimensions.empty()?std::vector<int>{static_cast<int>(value.elements.size())}:value.dimensions;
                const unsigned header=4+static_cast<unsigned>(dimensions.size())*4;
                const unsigned width=(value.type==T_TEXT||value.type==T_BIN||FindType(value.type))?4:static_cast<unsigned>(ScalarSize(value.type));
                memory.bytes.resize(header+width*value.elements.size());
                *reinterpret_cast<unsigned*>(memory.bytes.data())=static_cast<unsigned>(dimensions.size());
                std::memcpy(memory.bytes.data()+4,dimensions.data(),dimensions.size()*4);
                for(std::size_t i=0;i<value.elements.size();++i) {
                    unsigned char slot[8]{};NativeStore(value.elements[i],slot);
                    std::memcpy(memory.bytes.data()+header+i*width,slot,width);
                }
                memory.pointer=memory.bytes.data();
            } else if(const auto* desc=FindType(value.type)) {
                const auto* table=NativeClassTable(value.type);
                const unsigned prefix=table?4:0;
                memory.bytes.resize(desc->size+prefix);
                memory.pointer=memory.bytes.data();
                if(table) *reinterpret_cast<const void* const**>(memory.bytes.data())=table;
                for(std::size_t i=0;i<desc->fieldCount;++i) {
                    unsigned char slot[8]{};NativeStore(value.fields[i],slot);
                    const auto& field=desc->fields[i];
                    const auto size=(field.array||field.type==T_TEXT||field.type==T_BIN||FindType(field.type))?4:ScalarSize(field.type);
                    std::memcpy(memory.bytes.data()+prefix+field.offset,slot,size);
                }
            }
            memory.storing=false;
        }
        std::memcpy(destination,&memory.pointer,4);
        return;
    }
    switch(value.type) {
    case T_FLOAT: { float number=static_cast<float>(ToNumber(value));std::memcpy(destination,&number,4);break; }
    case T_DOUBLE:case T_DATE: { double number=ToNumber(value);std::memcpy(destination,&number,8);break; }
    default: { long long number=ToInteger(value);std::memcpy(destination,&number,NativeWidth(value));break; }
    }
}
static void NativeRead(Value& value,const void* source) {
    if(value.byteReference) { *value.byteReference=*static_cast<const unsigned char*>(source);value.integer=*value.byteReference;value.number=static_cast<double>(value.integer);return; }
    if(value.declaredArray) {
        const auto* pointer=*static_cast<unsigned char* const*>(source);
        if(!pointer)return;
        const unsigned rank=*reinterpret_cast<const unsigned*>(pointer);
        std::vector<int> dimensions(rank);
        std::memcpy(dimensions.data(),pointer+4,rank*4);
        if(value.dimensions!=dimensions)Redim(value,dimensions,false);
        const auto width=(value.type==T_TEXT||value.type==T_BIN||FindType(value.type))?4:ScalarSize(value.type);
        for(std::size_t i=0;i<value.elements.size();++i)NativeRead(value.elements[i],pointer+4+rank*4+i*width);
        return;
    }
    if(value.type==T_TEXT) { const auto* pointer=*static_cast<char* const*>(source);value.text=pointer?pointer:"";return; }
    if(value.type==T_BIN) {
        const auto* pointer=*static_cast<unsigned char* const*>(source);
        if(pointer) { const auto count=*reinterpret_cast<const unsigned*>(pointer+4);value.bytes.assign(pointer+8,pointer+8+count); }
        else value.bytes.clear();
        return;
    }
    if(const auto* desc=FindType(value.type)) {
        const auto* pointer=*static_cast<unsigned char* const*>(source);
        if(!pointer)return;
        const unsigned prefix=NativeClassTable(value.type)?4:0;
        for(std::size_t i=0;i<desc->fieldCount;++i) NativeRead(value.fields[i],pointer+prefix+desc->fields[i].offset);
        return;
    }
    switch(value.type) {
    case T_BYTE:value.integer=*static_cast<const unsigned char*>(source);break;
    case T_SHORT:{short number;std::memcpy(&number,source,2);value.integer=number;break;}
    case T_FLOAT:{float number;std::memcpy(&number,source,4);value.number=number;value.integer=static_cast<long long>(number);return;}
    case T_DOUBLE:case T_DATE:std::memcpy(&value.number,source,8);value.integer=static_cast<long long>(value.number);return;
    case T_INT64:std::memcpy(&value.integer,source,8);break;
    default:{int number;std::memcpy(&number,source,4);value.integer=number;break;}
    }
    value.number=static_cast<double>(value.integer);
}
static bool NativeIndirect(const Value& value) {
    const auto* type=FindType(value.type);
    return !value.declaredArray && type && !type->enumeration;
}
static void* NativeReference(Value& value) {
    if(value.byteReference)return value.byteReference;
    unsigned char slot[8]{};NativeStore(value,slot);
    if(value.declaredArray||value.type==T_TEXT||value.type==T_BIN||FindType(value.type)) return &value.native->pointer;
    if(value.type==T_DOUBLE||value.type==T_DATE)return &value.number;
    if(value.type==T_FLOAT) {
        auto& memory=NativeStorageFor(value);memory.bytes.resize(4);std::memcpy(memory.bytes.data(),slot,4);return memory.bytes.data();
    }
    return &value.integer;
}
static void NativeSyncObjects(bool store) {
    std::vector<std::shared_ptr<NativeStorage>> objects;
    for(auto* memory:NativeObjects()) if(memory->owner)objects.push_back(memory->owner->native);
    for(auto& memory:objects) if(memory->owner && NativeClassTable(memory->owner->type)) {
        if(store) { unsigned slot;NativeStore(*memory->owner,&slot); }
        else NativeRead(*memory->owner,&memory->pointer);
    }
}
static Value* NativeResolveSelf(void* handle) {
    if(!handle)return nullptr;
    const void* pointer=*static_cast<void**>(handle);
    for(auto* memory:NativeObjects()) if(memory->pointer==pointer && memory->owner && NativeClassTable(memory->owner->type)) return memory->owner;
    return nullptr;
}
static thread_local unsigned nativeExternalDepth=0;
static void NativeEnterExternal() {
    NativeSyncObjects(true);
    ++nativeExternalDepth;
}
static void NativeLeaveExternal() {
    NativeSyncObjects(false);
    --nativeExternalDepth;
}
// 只有原生调用期间内存副本才是权威数据；消息循环回调先发布语义对象。
struct NativeCallbackScope {
    unsigned previous;
    NativeCallbackScope():previous(nativeExternalDepth) {
        NativeSyncObjects(previous==0 && nativeRun==nullptr);
        nativeExternalDepth=0;
    }
    ~NativeCallbackScope() {
        NativeSyncObjects(true);
        nativeExternalDepth=previous;
    }
};
struct NativeFrame {
    NativeFrame* parent;
    std::vector<Value>& parameters;
    std::vector<Value*> locals;
    std::vector<Arg>& arguments;
    std::vector<NativeParameter> specs;
    std::vector<unsigned> parameterOffsets,localOffsets;
    std::vector<void*> referencePointers;
    std::vector<unsigned char> storage;
    unsigned localSize=0;
    Value* self;
    NativeFrame(std::vector<Value>& p,std::initializer_list<Value*> v,std::vector<Arg>& a,
                std::initializer_list<NativeParameter> s,Value* receiver):parent(nativeFrame),parameters(p),locals(v),arguments(a),specs(s),self(receiver) {
        for(auto* value:v) { localSize+=NativeWidth(*value);localOffsets.push_back(localSize); }
        unsigned size=localSize+8+(self?4:0);
        for(std::size_t i=0;i<p.size();++i) {
            parameterOffsets.push_back(size);
            size+=(specs[i].reference?4:NativeWidth(p[i]))+(specs[i].nullable?4:0);
        }
        storage.resize(size);
        referencePointers.resize(p.size());
        *reinterpret_cast<void**>(storage.data()+localSize)=parent?parent->Base():nullptr;
        nativeFrame=this;
        Store();
    }
    ~NativeFrame() { nativeFrame=parent; }
    void* Base() { return storage.data()+localSize; }
    void Store() {
        if(self) *reinterpret_cast<void**>(storage.data()+localSize+8)=NativeReference(*self);
        for(std::size_t i=0;i<locals.size();++i) NativeStore(*locals[i],storage.data()+localSize-localOffsets[i]);
        for(std::size_t i=0;i<parameters.size();++i) {
            auto* slot=storage.data()+parameterOffsets[i];
            if(specs[i].reference) {
                Value& target=i<arguments.size()?arguments[i].Get():parameters[i];
                referencePointers[i]=NativeReference(target);
                *reinterpret_cast<void**>(slot)=referencePointers[i];
            } else if(NativeIndirect(parameters[i])) *reinterpret_cast<void**>(slot)=NativeReference(parameters[i]);
            else NativeStore(parameters[i],slot);
            if(specs[i].nullable) *reinterpret_cast<unsigned*>(slot+(specs[i].reference?4:NativeWidth(parameters[i])))=
                i<arguments.size()&&!arguments[i].Get().missing?1:0;
        }
    }
    void Read(bool returned) {
        if(!returned)for(std::size_t i=0;i<locals.size();++i) NativeRead(*locals[i],storage.data()+localSize-localOffsets[i]);
        for(std::size_t i=0;i<parameters.size();++i) {
            const auto* slot=storage.data()+parameterOffsets[i];
            if(specs[i].reference) {
                Value& target=i<arguments.size()?arguments[i].Get():parameters[i];
                NativeRead(target,referencePointers[i]);
            } else if(!returned) NativeRead(parameters[i],NativeIndirect(parameters[i])?*reinterpret_cast<void* const*>(slot):slot);
        }
    }
    bool Execute(void* code,std::uint32_t type,Value& result) {
        Store();
        NativeSyncObjects(true);
        NativeRun run{storage.data(),static_cast<unsigned>(storage.size()),localSize,code};
        run.floating=type==T_FLOAT||type==T_DOUBLE||type==T_DATE;
        NativeRun* previous=nativeRun;
        nativeRun=&run;
        NativeInvoke(&run);
        nativeRun=previous;
        Read(run.returned!=0);
        NativeSyncObjects(false);
        result=run.floating?Number(run.real):Integer(type==T_INT64?static_cast<long long>((static_cast<unsigned long long>(run.high)<<32)|run.low):static_cast<int>(run.low),type);
        result.type=result.declared=type;
        return run.returned!=0;
    }
};
} // namespace ert
#endif
)CPP";

} // namespace ecompiler
