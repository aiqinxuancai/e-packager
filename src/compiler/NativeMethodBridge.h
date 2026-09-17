#pragma once
// x86 原生方法包装共用参数解码与写回；描述表保持原始参数布局。
namespace ecompiler {
inline constexpr const char* kNativeMethodBridge = R"CPP(
struct NativeMethodParameter {
    unsigned type, offset, presence;
    bool array, indirect, reference;
};
static __declspec(noinline) Value InvokeNativeMethod(
    Value (*method)(std::vector<Arg>,Value*,bool), void* receiver,
    const unsigned* raw, const NativeMethodParameter* specs, unsigned count) {
    std::vector<Value> parameters;
    parameters.reserve(count);
    std::vector<Arg> arguments;
    arguments.reserve(count);
    for(unsigned i=0;i<count;++i) {
        const auto& spec=specs[i];
        parameters.push_back(MakeVar(spec.type,spec.array));
        auto& value=parameters.back();
        value.missing=spec.presence!=~0u && raw[spec.presence]==0;
        if(!value.missing) NativeRead(value,spec.indirect
            ? reinterpret_cast<const void*>(raw[spec.offset]) : &raw[spec.offset]);
        arguments.push_back(Arg::Ref(value));
    }
    Value result=method(std::move(arguments),NativeResolveSelf(receiver),true);
    for(unsigned i=0;i<count;++i)
        if(specs[i].reference && raw[specs[i].offset])
            NativeStore(parameters[i],reinterpret_cast<void*>(raw[specs[i].offset]));
    return result;
}
)CPP";
}
