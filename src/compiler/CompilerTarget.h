#pragma once

#include <cstdint>

namespace ecompiler {

// 源码编译目标架构。Host 表示跟随当前 e-packager 进程位数。
enum class TargetArchitecture : std::uint8_t {
	Host,
	X86,
	X64,
};

}  // namespace ecompiler
