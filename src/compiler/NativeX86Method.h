#pragma once
#include "NativeX86Access.h"
#include <charconv>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace ecompiler {

// 整体生成可静态确定的 x86 子程序；语义语句和原始机器码共用一个 EBP 栈帧。
struct NativeX86MethodBody {
    std::string assembly;
    NativeX86Access access = NativeX86Access::FrameOnly;
};

inline std::optional<NativeX86MethodBody> GenerateNativeX86Method(const Program& program, const Method& method)
{
    if (program.targetArchitecture != TargetArchitecture::X86) return {};
    const auto scalar = [](TypeRef type) {
        return !type.isArray && (type.code == kTypeInt || type.code == kTypeBool || type.code == kTypeSubroutine);
    };
    if (method.returnType.code != kTypeNull && !scalar(method.returnType)) return {};
    struct Slot { int offset; bool reference; };
    std::unordered_map<std::string, Slot> slots;
    int offset = 0;
    for (const auto& local : method.locals) {
        if (local.isStatic || !scalar(local.type)) return {};
        offset -= 4;
        slots.emplace(local.name, Slot{offset, false});
    }
    offset = method.ownerType.valid ? 12 : 8;
    for (const auto& parameter : method.parameters) {
        if (!parameter.byReference && parameter.type.code == kTypeAll) return {};
        if (!parameter.nullable && scalar(parameter.type)) slots.emplace(parameter.name, Slot{offset, parameter.byReference});
        offset += parameter.byReference || parameter.type.isArray ? 4 :
            (parameter.type.code == kTypeInt64 || parameter.type.code == kTypeDouble || parameter.type.code == kTypeDateTime ? 8 : 4);
        if (parameter.nullable) offset += 4;
    }
    const auto address = [](int displacement) {
        return std::string("[ebp") + (displacement >= 0 ? "+" : "") + std::to_string(displacement) + "]";
    };
    // 普通表达式仅接受具有精确 32 位语义的局部值；不猜测通用值、对象或浮点转换。
    const auto operand = [&](const e2txt::SourceExpressionNode* node) -> std::optional<std::string> {
        using Kind = e2txt::SourceExpressionKind;
        while (node && node->kind == Kind::Group && node->children.size() == 1) node = node->children[0].get();
        if (!node) return {};
        if (node->kind == Kind::Name) {
            const auto found = slots.find(node->text);
            if (found != slots.end() && !found->second.reference) return "dword ptr " + address(found->second.offset);
        }
        if (node->kind == Kind::LogicalLiteral) return node->text == "真" ? "1" : "0";
        if (node->kind == Kind::NumberLiteral) {
            std::int32_t value = 0;
            const auto result = std::from_chars(node->text.data(), node->text.data() + node->text.size(), value);
            if (result.ec == std::errc{} && result.ptr == node->text.data() + node->text.size()) return std::to_string(value);
        }
        return {};
    };
    NativeX86MethodBody output;
    std::ostringstream code;
    bool hasMachine = false;
    for (const auto& statement : method.body) {
        switch (statement.kind) {
        case StatementKind::MachineCode: {
            hasMachine = true;
            const auto access = AnalyzeNativeX86Access(program, method, statement.machineCode);
            if (static_cast<int>(access) < static_cast<int>(output.access)) output.access = access;
            for (auto byte : statement.machineCode) code << "        _emit " << static_cast<unsigned>(byte) << '\n';
            break;
        }
        case StatementKind::Assignment: {
            if (!statement.target || statement.target->kind != e2txt::SourceExpressionKind::Name) return {};
            const auto target = slots.find(statement.target->text);
            const auto source = operand(statement.expression.get());
            // 保留机器码片段之间的寄存器值，普通赋值只修改目标栈槽。
            if (target == slots.end() || target->second.reference || !source) return {};
            code << "        push eax\n        mov eax, " << *source << "\n        mov dword ptr "
                 << address(target->second.offset) << ", eax\n        pop eax\n";
            break;
        }
        case StatementKind::Return: {
            if (!statement.expression) {
                if (method.returnType.code != kTypeNull) return {};
                code << "        xor eax, eax\n";
            } else {
                const auto source = operand(statement.expression.get());
                if (!source || method.returnType.code == kTypeNull) return {};
                code << "        mov eax, " << *source << '\n';
            }
            code << "        mov esp, ebp\n        pop ebp\n        ret\n";
            break;
        }
        default: return {};
        }
    }
    if (!hasMachine) return {};
    code << "        jmp NativeMachineFallthrough\n";
    output.assembly = code.str();
    return output;
}
} // namespace ecompiler
