// 直接验证语法树绑定，不依赖原生快照或 IDE 缓存。
#include "../src/SourceCallBindingCodec.h"
#include <cstdlib>
#include <iostream>
#include <string>
using namespace e2txt::source_text;
void Check(bool value, const char* name) {
    if (!value) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}
int main() {
    const std::string header = ".版本 2\r\n.程序集 同名类, <对象>\r\n.子程序 测试\r\n";
    const std::string body = "a ＝ krnln::取运行目录 () ＋ “\\xx\\”\r\n.如果 (krnln::取运行目录 () ＝ “路径”)\r\n调试输出 (“krnln::取运行目录 ()”) ' krnln::取运行目录 ()\r\n.如果结束\r\n";
    const auto origin = header + body;
    std::string visible, restored, error;
    Check(RenderCallBindings(origin, visible), "render found calls");
    Check(visible.find("a ＝ 取运行目录 ()") != std::string::npos, "assignment");
    Check(visible.find(".如果 (取运行目录 ()") != std::string::npos, "condition nested call");
    Check(visible.find("“krnln::取运行目录 ()”") != std::string::npos, "literal untouched");
    Check(visible.find("' krnln::取运行目录 ()") != std::string::npos, "comment untouched");
    Check(visible.find("<对象>") != std::string::npos, "object unchanged");
    Check(RestoreCallBindings(origin, visible, restored, error) && restored == origin, "untouched exact");
    auto edited = visible;
    edited.replace(edited.find("\\xx\\"), 4, "\\yy\\");
    edited.insert(edited.find("a ＝"), "调试输出 (“新增语句”)\r\n' 注释\r\n");
    Check(RestoreCallBindings(origin, edited, restored, error), "edit and insert");
    Check(restored.find("a ＝ krnln::取运行目录 () ＋ “\\yy\\”") != std::string::npos, "edited literal bound");
    auto renamed = visible;
    renamed.replace(renamed.find(".子程序 测试"), std::string(".子程序 测试").size(), ".子程序 改名");
    Check(RestoreCallBindings(origin, renamed, restored, error), "rename method");
    auto duplicate = visible + "a ＝ 取运行目录 () ＋ “\\xx\\”\r\n";
    Check(!RestoreCallBindings(origin, duplicate, restored, error), "new duplicate requires qualification");
    duplicate = visible + "a ＝ krnln::取运行目录 () ＋ “\\xx\\”\r\n";
    Check(RestoreCallBindings(origin, duplicate, restored, error), "explicit new target");
    const auto ambiguous = header + "返回 (krnln::取运行目录 () ＋ “一”)\r\n返回 (取运行目录 () ＋ “二”)\r\n";
    Check(!RestoreCallBindings(ambiguous, header + "返回 (取运行目录 () ＋ “三”)\r\n", restored, error), "ambiguous structural change");
    Check(RestoreCallBindings(ambiguous, header + "返回 (krnln::取运行目录 () ＋ “三”)\r\n", restored, error), "explicit resolves ambiguity");
    Check(RestoreCallBindings(ambiguous, header + "返回 (取运行目录 () ＋ “二”)\r\n返回 (取运行目录 () ＋ “一”)\r\n", restored, error), "reorder unique statements");
    Check(restored == header + "返回 (取运行目录 () ＋ “二”)\r\n返回 (krnln::取运行目录 () ＋ “一”)\r\n", "reorder keeps both targets");
    Check(RestoreCallBindings(origin, header, restored, error) && restored == header, "delete bound calls");
    Check(!RestoreCallBindings(origin, visible + "a ＝ 取运行目录 (\r\n", restored, error), "unparsed edit rejected");
    const auto types = ".程序集 类, library::Type\r\n.子程序 过程\r\n.局部变量 x, library::Type\r\nx.Method ()\r\n";
    Check(!RenderCallBindings(types, restored) && restored == types, "types and member names untouched");
    std::cout << "PASS: call binding syntax and conservative edit matching\n";
}
