// 验证原生身份视图的恢复边界，不将形似占位符的用户文字解释成符号身份。
#include "../src/SourceIdentityCodec.h"
#include <cstdlib>
#include <iostream>
#include <string>
using e2txt::source_text::RestoreIdentityProgram;
void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL " << message << '\n'; std::exit(1); }
}
int main() {
    const std::string head = ".版本 2\r\n.程序集 类, <对象>\r\n.子程序 方法\r\n";
    const auto origin = head + ".参数 路径, _Cls_0x99, 参考\r\n' .如果真 (_Local_0x12 ≠ 0)\r\n    ' 注释\r\n' .如果真结束\r\n调试输出 (“_Local_0x12”)\r\n";
    const auto display = head + ".参数 路径, , 参考\r\n.' 如果真 ( ≠ 0)\r\n    ' 注释\r\n.如果真结束\r\n调试输出 (“_Local_0x12”)\r\n";
    std::string result, error;
    Check(RestoreIdentityProgram(origin, display, display, result, error) && result == origin, "unchanged identity");
    auto edit = display;
    edit.replace(edit.find("≠ 0"), std::string("≠ 0").size(), "≠ 15");
    edit += "' another edit\r\n";
    Check(RestoreIdentityProgram(origin, display, edit, result, error), "literal edit");
    Check(result.find("_Local_0x12 ≠ 15") != std::string::npos, "reference preserved with new literal");
    Check(result.find("“_Local_0x12”") != std::string::npos, "literal is not a reference");
    edit = display;
    edit.replace(edit.find("( ≠"), std::string("( ≠").size(), "(真实变量 ≠");
    Check(RestoreIdentityProgram(origin, display, edit, result, error), "replace missing operand");
    Check(result.find("' .如果真 (真实变量 ≠ 0)") != std::string::npos, "explicit symbol wins");
    edit = display;
    edit.replace(edit.find("路径, ,"), std::string("路径, ,").size(), "路径, 整数型,");
    Check(RestoreIdentityProgram(origin, display, edit, result, error) && result.find("_Cls_") == std::string::npos, "explicit type wins");
    const auto ambiguous = head + "' .如果真 (_Local_0x12 ≠ 0)\r\n' .如果真 (_Local_0x34 ≠ 0)\r\n";
    const auto ambiguousDisplay = head + ".' 如果真 ( ≠ 0)\r\n.' 如果真 ( ≠ 0)\r\n";
    Check(RestoreIdentityProgram(ambiguous, ambiguousDisplay, ambiguousDisplay + "' edit\r\n", result, error) &&
        result == ambiguous + "' edit\r\n", "unchanged repeated references retain occurrence identity");
    Check(!RestoreIdentityProgram(ambiguous, ambiguousDisplay, head + ".' 如果真 ( ≠ 0)\r\n", result, error),
        "deleting an indistinguishable occurrence fails closed");
    const auto user = head + "_Local_0x12 ＝ 7\r\n' .如果真 (真)\r\n' 这是普通注释\r\n' .如果真结束\r\n";
    Check(RestoreIdentityProgram(user, user, user, result, error) && result == user, "ordinary names and comments unchanged");
    Check(!RestoreIdentityProgram(origin, "", display, result, error), "broken provenance rejected");
    Check(RestoreIdentityProgram("", "", "a ＝ 读入文件 (取运行目录 () ＋ “\\xx\\”)\r\n", result, error), "new source accepted");
    Check(result == "a ＝ 读入文件 (取运行目录 () ＋ “\\xx\\”)\r\n", "path untouched");
    Check(RestoreIdentityProgram("", "", head + ".' 如果真 (_Local_0x1E217 ≠ 0)\r\n    标准输出 (1, “OK”)\r\n.如果真结束\r\n", result, error), "new disabled block");
    Check(result == head + ".' 如果真 (_Local_0x1E217 ≠ 0)\r\n    标准输出 (1, “OK”)\r\n.' 如果真结束\r\n", "new disabled block structure");
    edit = display;
    edit.replace(edit.find(".子程序 方法"), std::string(".子程序 方法").size(), ".子程序 改名");
    Check(!RestoreIdentityProgram(origin, display, edit, result, error) && error.starts_with("unmatched_native_identity:"), "renamed method cannot lose identity");
    edit = display;
    edit.replace(edit.find(".参数 路径"), std::string(".参数 路径").size(), ".参数 改名");
    Check(!RestoreIdentityProgram(origin, display, edit, result, error) && error.starts_with("unmatched_native_identity:"), "renamed blank type cannot become default type");
    edit = display + ".子程序 新方法\r\n.' 如果真 ( ≠ 0)\r\n.如果真结束\r\n";
    Check(!RestoreIdentityProgram(origin, display, edit, result, error), "moving a hidden local across methods fails closed");
    edit = display + "' .如果真 (真)\r\n' 普通注释\r\n' .如果真结束\r\n";
    Check(RestoreIdentityProgram(origin, display, edit, result, error) && result.ends_with("' .如果真 (真)\r\n' 普通注释\r\n' .如果真结束\r\n"), "comments survive an edited identity page");
    const auto explicitOrigin = head + "' preceding comment\r\n.' 如果真 (_Local_0x12 ≠ 0)\r\n\r\n.' 如果真结束\r\n";
    const auto explicitDisplay = head + "' preceding comment\r\n.' 如果真 ( ≠ 0)\r\n\r\n.如果真结束\r\n";
    Check(RestoreIdentityProgram(explicitOrigin, explicitDisplay, explicitDisplay + "' edit\r\n", result, error) &&
        result == explicitOrigin + "' edit\r\n", "explicit empty disabled control stays structural");
    const auto commentOrigin = head + ".' _Dll_0x108EC ()\r\n.子程序 其他\r\n";
    const auto commentDisplay = head + "' 注册IE右键 ()\r\n.子程序 其他\r\n";
    Check(RestoreIdentityProgram(commentOrigin, commentDisplay, commentDisplay + "' added comment\r\n", result, error) &&
        result == commentOrigin + "' added comment\r\n", "unrelated whole comments cannot imply moved identity");
    std::cout << "PASS source identity provenance\n";
}
