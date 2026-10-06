// 在真实 Win32 FNE ABI 上验证所有 UD 类型及赋值持久化。
#include "FormControlPropertyCodec.h"
#include <iostream>
#include <stdexcept>
using namespace e2txt;
static void Require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv)
{
    try {
        Require(argc == 2, "FNE path required");
        FormControlPropertyCodec codec("", {{"Lib2Properties", argv[1]}}, {}, false);
        const std::vector<std::string> expected = {"-7", "42", "1.25", "真", "45292.5",
            "plain", "2", "choice", "editable", "AAEC/w==", "AAEC/w==", "AAEC/w==",
            "AAEC/w==", "AAEC/w==", "255", "-16777216", "sample.txt", "-16777216", "AAEC/w==", "AAEC/w=="};
        std::vector<std::pair<std::string, std::string>> attributes;
        for (size_t i = 0; i < expected.size(); ++i) attributes.emplace_back("Value" + std::to_string(i), expected[i]);
        std::vector<std::uint8_t> data;
        std::string error;
        Require(codec.Apply(65537, {}, 1, 1, attributes, data, &error), error);
        std::vector<FormControlPropertyValue> values;
        Require(codec.Decode(65537, data, 1, 1, values, &error), error);
        Require(values.size() == expected.size(), "property count");
        for (size_t i = 0; i < values.size(); ++i)
            Require(FormControlPropertyCodec::ValueToXmlText(values[i]) == expected[i], "mismatch " + std::to_string(i));
        std::vector<std::uint8_t> denied;
        Require(!codec.Apply(65537, data, 1, 1, {{"Value1", "-1"}, {"Value19", "AwQ="}}, denied, &error), "disabled custom accepted");
        Require(error.find("not_persisted") != std::string::npos, error);
        Require(codec.Apply(65537, data, 1, 1, {{"Value19", "AwQ="}}, denied, &error), error);
        Require(codec.Decode(65537, denied, 1, 1, values, &error), error);
        Require(FormControlPropertyCodec::ValueToXmlText(values.back()) == "AwQ=", "custom bytes not saved");
        std::cout << "PASS 20 lib2.h property types, custom bytes, recreate, rejected setter detection\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
