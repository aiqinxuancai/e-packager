#pragma once

#include "CompilerModel.h"
#include "NativeWindowControl.h"

namespace ecompiler {

// 从窗口 XML 建立独立编译器使用的窗体、控件和事件模型。
bool BuildWindowModel(Program& program, std::string& outError);

}  // namespace ecompiler
