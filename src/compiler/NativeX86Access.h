#pragma once

#include "CompilerModel.h"
#include <array>
#include <cstring>
#include <span>

namespace ecompiler {

// 保守分析短 x86 片段的内存效果；未识别指令仍使用完整原生同步。
enum class NativeX86Access { Full, ReadOnly, FrameOnly };

inline NativeX86Access AnalyzeNativeX86Access(const Program& program, const Method& method,
    std::span<const std::uint8_t> code)
{
    struct Slot { int offset, width; bool reference, writable; };
    std::vector<Slot> slots;
    const auto width = [](TypeRef type) {
        return !type.isArray && (type.code == kTypeInt64 || type.code == kTypeDouble || type.code == kTypeDateTime) ? 8 : 4;
    };
    int offset = 0;
    for (const auto& local : method.locals) {
        if (local.isStatic) continue;
        offset -= width(local.type);
        // 只允许改写普通数值局部槽；改写对象指针可能引入新的外部读取。
        const bool numeric = !local.type.isArray &&
            (local.type.code == kTypeByte || local.type.code == kTypeShort || local.type.code == kTypeInt ||
             local.type.code == kTypeInt64 || local.type.code == kTypeFloat || local.type.code == kTypeDouble ||
             local.type.code == kTypeDateTime || local.type.code == kTypeBool || local.type.code == kTypeSubroutine);
        slots.push_back({offset, width(local.type), false, numeric});
    }
    offset = 8;
    if (method.ownerType.valid) { slots.push_back({offset, 4, true, false}); offset += 4; }
    for (const auto& parameter : method.parameters) {
        // 通用值参数的实际栈宽由运行时类型决定，其后参数无法静态定位。
        if (!parameter.byReference && parameter.type.code == kTypeAll) break;
        const auto* type = program.FindType(parameter.type.code);
        const bool indirect = !parameter.type.isArray &&
            (parameter.type.code == kTypeText || parameter.type.code == kTypeBinary || (type && !type->isEnum));
        const int size = parameter.byReference ? 4 : width(parameter.type);
        slots.push_back({offset, size, parameter.byReference || indirect, false});
        offset += size;
        if (parameter.nullable) { slots.push_back({offset, 4, false, false}); offset += 4; }
    }
    const auto slotAt = [&](int address) -> const Slot* {
        for (const auto& slot : slots) if (address >= slot.offset && address <= slot.offset + slot.width - 4) return &slot;
        return nullptr;
    };
    // 仅跟踪可证明的 4 字节引用槽。加载槽内容后不猜测它是否仍是有效指针。
    std::array<bool, 8> reference{};
    bool frameOnly = true;
    std::size_t cursor = 0;
    const auto take = [&](std::size_t count) { return count <= code.size() - cursor; };
    while (cursor < code.size()) {
        const auto op = code[cursor++];
        if (op == 0x90) continue;
        if (op >= 0xB8 && op <= 0xBF) {
            const auto reg = op - 0xB8;
            if (reg == 4 || reg == 5 || !take(4)) return NativeX86Access::Full;
            reference[reg] = false; cursor += 4; continue;
        }
        if (op == 0xC9) {
            // leave 只允许作为末尾 return 的一部分，防止后续访问已改变的 EBP。
            if (!take(1) || (code[cursor] != 0xC2 && code[cursor] != 0xC3)) return NativeX86Access::Full;
            continue;
        }
        if (op == 0xC2 || op == 0xC3) {
            if (op == 0xC2) { if (!take(2)) return NativeX86Access::Full; cursor += 2; }
            if (cursor != code.size()) return NativeX86Access::Full;
            break;
        }
        if (op != 0x8B && op != 0x89 && op != 0x31) return NativeX86Access::Full;
        if (!take(1)) return NativeX86Access::Full;
        const auto modrm = code[cursor++];
        const unsigned mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
        if (mod == 3) {
            const auto destination = op == 0x8B ? reg : rm;
            if (destination == 4 || destination == 5) return NativeX86Access::Full;
            reference[destination] = op == 0x31 ? false : reference[op == 0x8B ? rm : reg];
            continue;
        }
        if (op == 0x31 || rm == 4) return NativeX86Access::Full; // 不猜测 SIB 或读改写指令。
        std::int32_t displacement = 0;
        if (mod == 1) {
            if (!take(1)) return NativeX86Access::Full;
            displacement = static_cast<std::int8_t>(code[cursor++]);
        } else if (mod == 2 || (mod == 0 && rm == 5)) {
            if (!take(4)) return NativeX86Access::Full;
            std::memcpy(&displacement, code.data() + cursor, 4); cursor += 4;
        }
        const bool frame = rm == 5 && mod != 0;
        const auto* slot = frame ? slotAt(displacement) : nullptr;
        if (op == 0x89) {
            if (!slot || !slot->writable) return NativeX86Access::Full;
        } else {
            if (reg == 4 || reg == 5) return NativeX86Access::Full;
            if (frame) {
                if (!slot) frameOnly = false;
                reference[reg] = slot && displacement == slot->offset && slot->reference;
            } else {
                if (!reference[rm] || displacement != 0 || (mod == 0 && rm == 5)) frameOnly = false;
                reference[reg] = false;
            }
        }
    }
    return frameOnly ? NativeX86Access::FrameOnly : NativeX86Access::ReadOnly;
}

} // namespace ecompiler
