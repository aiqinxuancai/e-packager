#include "WorkspaceProjectSupport.h"

#include <Windows.h>
#include <Wincrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

#include "..\thirdparty\json.hpp"

#pragma comment(lib, "Advapi32.lib")

namespace workspace_support {

namespace {

using json = nlohmann::json;

struct SourceFileInfo {
	std::string fileName;
	std::string fullPath;
	std::string sourceFileKind;
	std::string modifiedTimeUtc;
	std::uint64_t fileSize = 0;
	std::string md5;
};

std::string WideToUtf8(const std::wstring& text)
{
	if (text.empty()) {
		return std::string();
	}

	const int utf8Len = WideCharToMultiByte(
		CP_UTF8,
		0,
		text.data(),
		static_cast<int>(text.size()),
		nullptr,
		0,
		nullptr,
		nullptr);
	if (utf8Len <= 0) {
		return std::string();
	}

	std::string utf8(static_cast<size_t>(utf8Len), '\0');
	if (WideCharToMultiByte(
			CP_UTF8,
			0,
			text.data(),
			static_cast<int>(text.size()),
			utf8.data(),
			utf8Len,
			nullptr,
			nullptr) <= 0) {
		return std::string();
	}
	return utf8;
}

std::wstring Utf8ToWide(const std::string& text)
{
	if (text.empty()) {
		return std::wstring();
	}

	const int wideLen = MultiByteToWideChar(
		CP_UTF8,
		0,
		text.data(),
		static_cast<int>(text.size()),
		nullptr,
		0);
	if (wideLen <= 0) {
		return std::wstring();
	}

	std::wstring wide(static_cast<size_t>(wideLen), L'\0');
	if (MultiByteToWideChar(
			CP_UTF8,
			0,
			text.data(),
			static_cast<int>(text.size()),
			wide.data(),
			wideLen) <= 0) {
		return std::wstring();
	}
	return wide;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
	return WideToUtf8(path.wstring());
}

std::string DetectSourceFileKind(const std::filesystem::path& path)
{
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return extension == ".ec" ? "ec" : "e";
}

bool IsEcSourceFileKind(const std::string& sourceFileKind)
{
	std::string normalized = sourceFileKind;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return normalized == "ec";
}

std::string NormalizeCrLf(const std::string& text)
{
	std::string normalized;
	normalized.reserve(text.size() + 16);
	for (size_t index = 0; index < text.size(); ++index) {
		const char ch = text[index];
		if (ch == '\r') {
			normalized.append("\r\n");
			if (index + 1 < text.size() && text[index + 1] == '\n') {
				++index;
			}
		}
		else if (ch == '\n') {
			normalized.append("\r\n");
		}
		else {
			normalized.push_back(ch);
		}
	}
	return normalized;
}

bool WriteUtf8TextFileBom(const std::filesystem::path& path, const std::string& utf8Text)
{
	std::error_code ec;
	if (path.has_parent_path()) {
		std::filesystem::create_directories(path.parent_path(), ec);
		if (ec) {
			return false;
		}
	}

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		return false;
	}

	static constexpr unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
	out.write(reinterpret_cast<const char*>(kBom), sizeof(kBom));
	const std::string normalized = NormalizeCrLf(utf8Text);
	if (!normalized.empty()) {
		out.write(normalized.data(), static_cast<std::streamsize>(normalized.size()));
	}
	return out.good();
}

bool ReadUtf8TextFile(const std::filesystem::path& path, std::string& outText)
{
	outText.clear();
	std::ifstream in(path, std::ios::binary);
	if (!in.is_open()) {
		return false;
	}

	std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (bytes.size() >= 3 &&
		static_cast<unsigned char>(bytes[0]) == 0xEF &&
		static_cast<unsigned char>(bytes[1]) == 0xBB &&
		static_cast<unsigned char>(bytes[2]) == 0xBF) {
		bytes.erase(0, 3);
	}
	outText = std::move(bytes);
	return true;
}

std::string FormatFileTimeUtc(const FILETIME& fileTime)
{
	SYSTEMTIME systemTime = {};
	if (!FileTimeToSystemTime(&fileTime, &systemTime)) {
		return std::string();
	}

	char buffer[64] = {};
	std::snprintf(
		buffer,
		sizeof(buffer),
		"%04u-%02u-%02uT%02u:%02u:%02uZ",
		static_cast<unsigned>(systemTime.wYear),
		static_cast<unsigned>(systemTime.wMonth),
		static_cast<unsigned>(systemTime.wDay),
		static_cast<unsigned>(systemTime.wHour),
		static_cast<unsigned>(systemTime.wMinute),
		static_cast<unsigned>(systemTime.wSecond));
	return buffer;
}

bool QuerySourceFileInfo(const std::filesystem::path& inputFile, SourceFileInfo& outInfo, std::string& outError)
{
	outInfo = {};
	outError.clear();

	const std::filesystem::path absolutePath = std::filesystem::absolute(inputFile);
	HANDLE file = CreateFileW(
		absolutePath.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		outError = "open_source_file_failed: " + PathToUtf8(absolutePath);
		return false;
	}

	bool ok = false;
	HCRYPTPROV provider = 0;
	HCRYPTHASH hash = 0;
	do {
		LARGE_INTEGER sizeValue = {};
		if (!GetFileSizeEx(file, &sizeValue) || sizeValue.QuadPart < 0) {
			outError = "query_source_size_failed: " + PathToUtf8(absolutePath);
			break;
		}

		FILETIME creationTime = {};
		FILETIME accessTime = {};
		FILETIME writeTime = {};
		if (!GetFileTime(file, &creationTime, &accessTime, &writeTime)) {
			outError = "query_source_time_failed: " + PathToUtf8(absolutePath);
			break;
		}

		if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) ||
			!CryptCreateHash(provider, CALG_MD5, 0, 0, &hash)) {
			outError = "create_md5_failed: " + PathToUtf8(absolutePath);
			break;
		}

		std::array<BYTE, 8192> buffer = {};
		DWORD readBytes = 0;
		while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &readBytes, nullptr) && readBytes > 0) {
			if (!CryptHashData(hash, buffer.data(), readBytes, 0)) {
				outError = "hash_source_file_failed: " + PathToUtf8(absolutePath);
				break;
			}
		}
		if (!outError.empty()) {
			break;
		}

		BYTE digest[16] = {};
		DWORD digestSize = sizeof(digest);
		if (CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0) == FALSE) {
			outError = "read_md5_failed: " + PathToUtf8(absolutePath);
			break;
		}

		static constexpr char kHex[] = "0123456789abcdef";
		outInfo.md5.reserve(digestSize * 2);
		for (DWORD index = 0; index < digestSize; ++index) {
			outInfo.md5.push_back(kHex[(digest[index] >> 4) & 0x0F]);
			outInfo.md5.push_back(kHex[digest[index] & 0x0F]);
		}

		outInfo.fileName = PathToUtf8(absolutePath.filename());
		outInfo.fullPath = PathToUtf8(absolutePath);
		outInfo.sourceFileKind = DetectSourceFileKind(absolutePath);
		outInfo.modifiedTimeUtc = FormatFileTimeUtc(writeTime);
		outInfo.fileSize = static_cast<std::uint64_t>(sizeValue.QuadPart);
		ok = true;
	}
	while (false);

	if (hash != 0) {
		CryptDestroyHash(hash);
	}
	if (provider != 0) {
		CryptReleaseContext(provider, 0);
	}
	CloseHandle(file);
	return ok;
}

std::filesystem::path GetCurrentExecutablePath()
{
	std::wstring buffer;
	buffer.resize(static_cast<size_t>(MAX_PATH));

	for (;;) {
		const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (written == 0) {
			return std::filesystem::path();
		}
		if (written < buffer.size() - 1) {
			buffer.resize(written);
			return std::filesystem::path(buffer);
		}
		buffer.resize(buffer.size() * 2);
	}
}

json BuildInfoJson(const SourceFileInfo& info, const WorkspaceWriteOptions& options)
{
	json infoJson;
	infoJson["version"] = 1;
	infoJson["sourceFileKind"] = info.sourceFileKind;
	infoJson["sourceFileName"] = info.fileName;
	infoJson["sourcePath"] = info.fullPath;
	infoJson["sourceModifiedTimeUtc"] = info.modifiedTimeUtc;
	infoJson["sourceSize"] = info.fileSize;
	infoJson["sourceMd5"] = info.md5;
	infoJson["toolUrl"] = "https://github.com/aiqinxuancai/e-packager";
	if (!options.defaultPackOutputFileName.empty()) {
		infoJson["defaultPackOutputFileName"] = options.defaultPackOutputFileName;
	}
	return infoJson;
}

std::string BuildAgentsMarkdown(const SourceFileInfo& info, const WorkspaceWriteOptions& options)
{
	const bool isEcProject = IsEcSourceFileKind(info.sourceFileKind);
	const std::wstring headerDirectoryLine = isEcProject
		? L"- `header/`：自动生成的公开接口头文件，仅供查阅与分发，不参与回包。\r\n"
		: std::wstring();
	const std::wstring dependencyDirectoryLines = isEcProject
		? L"- `elib/`：当前工程依赖支持库的公开接口导出文本，仅供查阅与 AI 理解，不参与回包。\r\n"
		: L"- `ecom/`：当前工程引用的易模块工作区副本，仅供查阅、检索与辅助编辑。\r\n"
		  L"- `elib/`：当前工程依赖支持库的公开接口导出文本，仅供查阅与 AI 理解，不参与回包。\r\n";
	const std::wstring infoJsonSourceType = isEcProject ? L".ec" : L".e";
	std::wstring packOutputFileName = Utf8ToWide(options.defaultPackOutputFileName);
	if (packOutputFileName.empty()) {
		if (!info.fileName.empty()) {
			packOutputFileName = Utf8ToWide(info.fileName);
		}
		else {
			packOutputFileName = isEcProject ? L"project.ec" : L"project.e";
		}
	}
	std::wostringstream stream;
	stream
		<< L"# AGENTS.md\r\n"
		<< L"\r\n"
		<< L"## 项目说明\r\n"
		<< L"\r\n"
		<< L"当前目录是由 `" << Utf8ToWide(info.fileName) << L"` 解包得到的易语言目录工程。\r\n"
		<< L"外部编辑器应直接修改本目录内容，再通过 `tool\\\\e-packager.exe` 回包生成 `" << packOutputFileName << L"` 文件。\r\n"
		<< L"\r\n"
		<< L"## 目录结构\r\n"
		<< L"\r\n"
		<< L"- `src/`：源码目录。普通程序集、类、窗口程序集使用 `.txt` 保存；窗口界面定义使用同名 `.xml` 保存。\r\n"
		<< L"- `src/.数据类型.txt`：数据类型定义。\r\n"
		<< L"- `src/.DLL声明.txt`：DLL 声明。\r\n"
		<< L"- `src/.常量.txt`：常量定义。\r\n"
		<< L"- `src/.全局变量.txt`：全局变量定义。\r\n"
		<< L"- `project/`：封包所需元数据与原生快照，请勿随意删除。\r\n"
		<< headerDirectoryLine
		<< dependencyDirectoryLines
		<< L"- `image/`：图片资源及 `list.json`。\r\n"
		<< L"- `audio/`：音频资源及 `list.json`。\r\n"
		<< L"- `tool/`：当前目录自带的 `e-packager.exe`。\r\n"
		<< L"- `info.json`：记录本目录来源 `" << infoJsonSourceType << L"` 的文件名、路径、类型、修改时间、尺寸、MD5。\r\n"
		<< L"\r\n"
		<< L"## 易语言基础约定\r\n"
		<< L"\r\n"
		<< L"- 以 `#` 开头的标识通常表示常量。\r\n"
		<< L"- 以 `&` 开头通常表示对子程序取址，常用于回调或传递函数地址，例如 `到整数 (&枚举窗口过程)`；当某个 API、DLL 或外部接口要求传递“整数型函数地址”时，通常可写作 `到整数 (&子程序名)`。\r\n"
		<< L"- 以 `.` 开头的是易语言系统指令或关键字，例如：`.版本`、`.程序集`、`.子程序`、`.参数`、`.局部变量`、`.程序集变量`、`.全局变量`、`.常量`、`.DLL声明`、`.数据类型`、`.成员`、`.如果`、`.如果真`、`.否则`、`.返回`。修改源码时不要删掉前导的 `.`，也不要把这些关键字改写成 C/C++ / JavaScript 风格。\r\n"
		<< L"- 单引号 `'` 开头表示注释。注释是一整行语句，不要把注释内容当成真正代码，也不要改成 `//` 或 `/* */`。\r\n"
		<< L"- 布尔值使用 `真` / `假`。\r\n"
		<< L"- 数组下标通常从 `1` 开始，访问第一个元素常写作 `数组 [1]`，不要按多数编程语言习惯改成从 `0` 开始推导。\r\n"
		<< L"- `.变量循环首 (起始值, 终止值, 步长, 变量)` 的循环范围由参数决定，不是固定从 `1` 开始；遍历数组时常见写法是 `.变量循环首 (1, 取数组成员数 (数组), 1, i)`，但其它起止范围也可能是合法的。\r\n"
		<< L"- `.计次循环首 (次数, i)` 的计次变量 `i` 通常从 `1` 递增到 `次数`；如果配合数组成员数使用，常见有效下标范围也是 `1..取数组成员数 (数组)`。\r\n"
		<< L"- 赋值常写作 `变量 ＝ 值`，不要误写成半角 `=`。\r\n"
		<< L"- 自增 / 自减通常写作 `a ＝ a ＋ 1`、`a ＝ a － 1`，不要写成 `a++`、`--a`。\r\n"
		<< L"- 返回常见写法为 `返回 (...)`。\r\n"
		<< L"- 普通程序集、窗口程序集中的子程序名按全局解析，没有命名空间可用于隔离同名函数；新增或重命名这类子程序时，必须确保它在整个工程范围内全局唯一，避免与其它程序集 / 窗口程序集中的现有子程序重名。\r\n"
		<< L"- 类中的成员子程序通常不能直接写成 `&类内子程序名` 作为普通回调传给 API 或 DLL；需要回调时，优先使用普通程序集子程序、窗口程序集子程序，或额外做桥接子程序。\r\n"
		<< L"- 代码中常见全角中文标点、全角运算符和中文引号，编辑时不要擅自替换成其它语言习惯写法。\r\n"
		<< L"\r\n"
		<< L"## 源码文件规则\r\n"
		<< L"\r\n"
		<< L"- 程序集、类、窗口程序集源码文件通常以 `.版本 2` 开头，随后出现 `.程序集 ...`、`.程序集变量 ...`、`.子程序 ...`、`.参数 ...`、`.局部变量 ...` 等定义。\r\n"
		<< L"- `src` 目录中以 `.` 开头的固定文件不是普通源码页，而是映射到易语言内部专用表的固定入口；它们应保留原文件名，不要改名成别的页面，也不要挪到子目录。\r\n"
		<< L"- `src/.数据类型.txt`：只放 `.数据类型` / `.成员` 相关定义，用于自定义数据类型表。\r\n"
		<< L"- `src/.DLL声明.txt`：只放 DLL 声明相关内容，用于外部 DLL 命令 / 声明表，不要在这里写普通 `.子程序`。\r\n"
		<< L"- `src/.常量.txt`：只放常量定义，用于常量表；`#常量名` 之类引用通常来自这里或支持库。\r\n"
		<< L"- `src/.全局变量.txt`：只放全局变量定义，用于全局变量表。\r\n"
		<< L"- 这些固定文件中的定义会在封包时分别写回易语言内部对应的数据区；如果把它们并到普通程序集 `.txt` 中，或把普通源码写进这些固定文件，回包后很容易出现编译错误、名称链接错误或结构错位。\r\n"
		<< L"- 窗口界面定义放在 `src/<窗口名>.xml`，窗口事件实现代码放在对应窗口程序集 `.txt` 中；不要把窗口 XML 内容写回源码 `.txt`。\r\n"
		<< L"- 控件事件子程序依赖窗口界面定义和窗口程序集之间的名称绑定，必须保留在对应的窗口程序集源码页中，不能挪到普通程序集、其它窗口程序集或类里。像 `_图片框_定位_鼠标左键被按下`、`_按钮_Clear_被单击` 这类事件子程序，一旦脱离所属窗口程序集，回包后通常会出现事件失联或编译错误。\r\n"
		<< L"- 新增子程序时，必须写完整的 `.子程序` 头、必要的 `.参数` / `.局部变量`，以及函数体；不要只写一段裸代码。\r\n"
		<< L"- 新增 DLL 声明、数据类型、常量、全局变量时，放到各自固定文件中，不要误加到程序集源码页里。\r\n"
		<< L"\r\n"
		<< L"## 新增程序集 / 类 / 窗口操作\r\n"
		<< L"\r\n"
		<< L"- 新增普通程序集：在 `src/` 或其子目录中新建一个文件名不以 `.` 开头的 `.txt` 文件，例如 `src/网络/下载工具.txt`。该文件会在封包时自动识别为源码页。最小示例：\r\n"
		<< L"\r\n"
		<< L"```e\r\n"
		<< L".版本 2\r\n"
		<< L"\r\n"
		<< L".程序集 下载工具\r\n"
		<< L"\r\n"
		<< L".子程序 示例, 整数型\r\n"
		<< L"返回 (0)\r\n"
		<< L"```\r\n"
		<< L"\r\n"
		<< L"- 新增类：同样是在 `src/` 或其子目录中新建普通 `.txt` 文件，但 `.程序集` 头中要写基类。自定义类通常写作 `.程序集 类名, 对象`；如果继承别的类，则把第二个字段写成基类名。最小示例：\r\n"
		<< L"\r\n"
		<< L"```e\r\n"
		<< L".版本 2\r\n"
		<< L"\r\n"
		<< L".程序集 下载任务, 对象\r\n"
		<< L".程序集变量 地址, 文本型\r\n"
		<< L"\r\n"
		<< L".子程序 开始\r\n"
		<< L"' TODO: 在此补充实现\r\n"
		<< L"```\r\n"
		<< L"\r\n"
		<< L"- 新增窗口：至少需要同时新增两个文件。\r\n"
		<< L"  1. `src/<窗口名>.xml`：窗口界面定义。\r\n"
		<< L"  2. `src/窗口程序集_<窗口名>.txt`：窗口程序集源码，建议严格按这个名字来创建，便于封包时和窗口 XML 自动匹配。\r\n"
		<< L"- 新增窗口程序集时，推荐直接写成 `.程序集 窗口程序集_<窗口名>`，并把窗口 XML 根节点的 `名称` 属性保持为同一个窗口名。最小示例：\r\n"
		<< L"\r\n"
		<< L"```e\r\n"
		<< L".版本 2\r\n"
		<< L"\r\n"
		<< L".程序集 窗口程序集_主窗口\r\n"
		<< L"\r\n"
		<< L".子程序 __启动窗口_创建完毕\r\n"
		<< L"' TODO: 在此补充窗口初始化代码\r\n"
		<< L"```\r\n"
		<< L"\r\n"
		<< L"- 封包器会优先按这些规则匹配窗口和窗口程序集：同名、`窗口程序集_<窗口名>`，以及少量兼容写法。为了减少名称链接错误，新增窗口时优先使用 `窗口程序集_<窗口名>.txt` 这种命名。\r\n"
		<< L"- 新增源码页后，不需要手动编辑窗口列表页或其它目录索引；`pack` 时会自动扫描 `src/**/*.txt`（排除 `src/.数据类型.txt`、`src/.DLL声明.txt`、`src/.常量.txt`、`src/.全局变量.txt` 这些固定文件）以及 `src/**/*.xml`。\r\n"
		<< L"- 新增完成后，立即运行 `tool\\\\e-packager.exe` 或 `tool\\\\e-packager.exe pack . .\\\\pack\\\\" << Utf8ToWide(info.fileName) << L"` 做一次封包验证，尽早发现语法错误、窗口绑定错误或名称链接错误。\r\n"
		<< L"\r\n"
		<< L"## 代码格式要求\r\n"
		<< L"\r\n"
		<< L"- 保持一条逻辑语句一行，不要把多行代码压成一行。\r\n"
		<< L"- 保持原有缩进、空行和注释风格，尽量与现有源码一致。\r\n"
		<< L"- 如果只是修改某个子程序，不要重写整个页面，更不要重复输出 `.版本 2`。\r\n"
		<< L"- 不要修改任何以 `.` 开头的系统指令拼写，也不要擅自替换成其它语言关键字。\r\n"
		<< L"- 新增普通程序集 / 窗口程序集子程序前，先检查工程内是否已有同名子程序；没有命名空间可兜底，同名很容易在封包或编译时触发名称冲突。\r\n"
		<< L"- 如果修改的是窗口事件、类方法或程序集方法，要确保该子程序仍保留在原有所属页面中。\r\n"
		<< L"- 如果修改的是控件事件子程序，除了保留在原所属页面中，还要继续留在对应窗口的窗口程序集内，不要跨窗口移动，也不要改成普通工具子程序。\r\n"
		<< L"\r\n"
		<< L"## 常见流程控制示例\r\n"
		<< L"\r\n"
		<< L"```e\r\n"
		<< L".如果真 ()\r\n"
		<< L"    a ＝ 0\r\n"
		<< L".如果真结束\r\n"
		<< L"\r\n"
		<< L".如果 (a ＝ 0)\r\n"
		<< L"    a ＝ 1\r\n"
		<< L".否则\r\n"
		<< L"    b ＝ 1\r\n"
		<< L".如果结束\r\n"
		<< L"\r\n"
		<< L".计次循环首 (count, i)\r\n"
		<< L"    a ＝ a ＋ 1\r\n"
		<< L".计次循环尾 ()\r\n"
		<< L"\r\n"
		<< L".变量循环首 (1, 100, 1, i)\r\n"
		<< L"    输出调试文本 (i)\r\n"
		<< L".变量循环尾 ()\r\n"
		<< L"\r\n"
		<< L".判断开始 (b ＝ 1)\r\n"
		<< L"    a ＝ 1\r\n"
		<< L".判断 (b ＝ 2)\r\n"
		<< L"    a ＝ 2\r\n"
		<< L".默认\r\n"
		<< L"    a ＝ 0\r\n"
		<< L".判断结束\r\n"
		<< L"```\r\n"
		<< L"\r\n"
		<< L"## 回包方法\r\n"
		<< L"\r\n"
		<< L"在项目根目录执行以下任一方式：\r\n"
		<< L"\r\n"
		<< L"- 默认方式：`tool\\\\e-packager.exe`\r\n"
		<< L"  默认输出到 `pack/` 目录，文件名优先使用 `info.json` 中记录的默认封包文件名。\r\n"
		<< L"- 显式方式：`tool\\\\e-packager.exe pack . .\\\\pack\\\\" << packOutputFileName << L"`\r\n"
		<< L"\r\n"
		<< L"回包后的 `" << packOutputFileName << L"` 可直接在易语言 IDE 中打开并编译。\r\n"
		<< L"\r\n"
		<< L"## 更新依赖\r\n"
		<< L"\r\n"
		<< L"当你新增模块、支持库，或希望刷新当前目录下的派生依赖文件时，可在项目根目录执行：\r\n"
		<< L"\r\n"
		<< L"- 刷新当前 `project/.module.json`、`ecom/` 以及可用时的 `elib/`：`tool\\\\e-packager.exe update .`\r\n"
		<< L"- 新增模块后顺带写入依赖并刷新：`tool\\\\e-packager.exe update . --add-ecom 某模块.ec`\r\n"
		<< L"- 新增支持库后顺带写入依赖并刷新：`tool\\\\e-packager.exe update . --add-elib 某支持库.fne`\r\n"
		<< L"- `--add-elib` 也可直接填写支持库名称，例如：`tool\\\\e-packager.exe update . --add-elib spec`\r\n"
		<< L"\r\n"
		<< L"`update` 不会直接回包；它用于在继续编辑源码前，同步项目中模块 / 支持库的依赖描述以及导出的辅助资料。\r\n"
		<< L"\r\n"
		<< L"## 错误定位\r\n"
		<< L"\r\n"
		<< L"如果回包时检测到语法错误，`e-packager.exe` 会输出类似以下信息：\r\n"
		<< L"\r\n"
		<< L"- `source_syntax_error: file=src/某页面.txt, line=行号, ...`\r\n"
		<< L"- `xml_syntax_error: file=src/某窗口.xml, line=行号, ...`\r\n"
		<< L"\r\n"
		<< L"请按报错中的文件与行号修正后再重新回包。\r\n"
		<< L"\r\n"
		<< L"## 工具来源\r\n"
		<< L"\r\n"
		<< L"本目录由 [e-packager](https://github.com/aiqinxuancai/e-packager) 解包生成。\r\n";
	return WideToUtf8(stream.str());
}

bool HasProjectMarkers(const std::filesystem::path& root)
{
	std::error_code ec;
	return std::filesystem::exists(root / "info.json", ec) &&
		std::filesystem::exists(root / "src", ec) &&
		(
			std::filesystem::exists(root / "project" / ".module.json", ec) ||
			std::filesystem::exists(root / "project" / "模块.json", ec) ||
			std::filesystem::exists(root / "src" / "模块.json", ec)
		);
}

bool ReadInfoJson(const std::filesystem::path& root, json& outInfo, std::string& outError)
{
	outError.clear();
	std::string text;
	if (!ReadUtf8TextFile(root / "info.json", text)) {
		outError = "read_info_json_failed: " + PathToUtf8(root / "info.json");
		return false;
	}

	try {
		outInfo = json::parse(text);
	}
	catch (const std::exception& ex) {
		outError = std::string("parse_info_json_failed: ") + ex.what();
		return false;
	}
	return true;
}

std::string ResolveDefaultOutputFileName(const json& infoJson, const std::filesystem::path& projectRoot)
{
	if (const auto it = infoJson.find("defaultPackOutputFileName");
		it != infoJson.end() && it->is_string()) {
		std::string fileName = it->get<std::string>();
		if (!fileName.empty()) {
			std::filesystem::path filePath = std::filesystem::path(Utf8ToWide(fileName)).filename();
			const std::string resolved = PathToUtf8(filePath);
			if (!resolved.empty()) {
				return resolved;
			}
		}
	}

	std::string sourceFileKind = infoJson.value("sourceFileKind", std::string());
	if (sourceFileKind.empty()) {
		const std::string sourceFileName = infoJson.value("sourceFileName", std::string());
		if (!sourceFileName.empty()) {
			sourceFileKind = DetectSourceFileKind(std::filesystem::path(Utf8ToWide(sourceFileName)));
		}
		else {
			const std::string sourcePath = infoJson.value("sourcePath", std::string());
			if (!sourcePath.empty()) {
				sourceFileKind = DetectSourceFileKind(std::filesystem::path(Utf8ToWide(sourcePath)));
			}
		}
	}

	const bool isEcProject = IsEcSourceFileKind(sourceFileKind);
	const std::wstring defaultExtension = isEcProject ? L".ec" : L".e";
	std::string fileName = infoJson.value("sourceFileName", std::string());
	if (fileName.empty()) {
		const std::string sourcePath = infoJson.value("sourcePath", std::string());
		if (!sourcePath.empty()) {
			fileName = PathToUtf8(std::filesystem::path(Utf8ToWide(sourcePath)).filename());
		}
	}
	if (fileName.empty()) {
		fileName = PathToUtf8(projectRoot.filename()) + WideToUtf8(defaultExtension);
	}

	std::filesystem::path filePath = std::filesystem::path(Utf8ToWide(fileName)).filename();
	if (filePath.extension().empty()) {
		filePath += defaultExtension;
	}

	const std::string resolved = PathToUtf8(filePath);
	return resolved.empty() ? std::string("project") + WideToUtf8(defaultExtension) : resolved;
}

bool CopyExecutableToToolDirectory(const std::filesystem::path& outputDir, std::string& outError)
{
	const std::filesystem::path executablePath = GetCurrentExecutablePath();
	if (executablePath.empty()) {
		outError = "query_current_executable_failed";
		return false;
	}

	const std::filesystem::path toolDir = outputDir / "tool";
	std::error_code ec;
	std::filesystem::create_directories(toolDir, ec);
	if (ec) {
		outError = "create_tool_dir_failed: " + PathToUtf8(toolDir);
		return false;
	}

	const std::filesystem::path destination = toolDir / executablePath.filename();
	std::filesystem::copy_file(executablePath, destination, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		outError = "copy_tool_exe_failed: " + PathToUtf8(destination);
		return false;
	}
	return true;
}

}  // namespace

static constexpr int kSupportedInfoVersion = 1;

bool WriteWorkspaceFiles(
	const std::filesystem::path& inputFile,
	const std::filesystem::path& outputDir,
	std::string& outError,
	const WorkspaceWriteOptions& options)
{
	outError.clear();

	SourceFileInfo info;
	if (!QuerySourceFileInfo(inputFile, info, outError)) {
		return false;
	}

	if (!WriteUtf8TextFileBom(outputDir / "info.json", BuildInfoJson(info, options).dump(2))) {
		outError = "write_info_json_failed: " + PathToUtf8(outputDir / "info.json");
		return false;
	}

	if (!CopyExecutableToToolDirectory(outputDir, outError)) {
		return false;
	}

	if (options.writeAgentsMarkdown) {
		if (!WriteUtf8TextFileBom(outputDir / "AGENTS.md", BuildAgentsMarkdown(info, options))) {
			outError = "write_agents_md_failed: " + PathToUtf8(outputDir / "AGENTS.md");
			return false;
		}
	}

	return true;
}

bool ResolveDefaultPackOutput(
	const std::filesystem::path& currentDir,
	std::filesystem::path& outProjectRoot,
	std::filesystem::path& outOutputFile,
	std::string& outError)
{
	outProjectRoot.clear();
	outOutputFile.clear();
	outError.clear();

	std::vector<std::filesystem::path> candidates;
	candidates.push_back(currentDir);
	if (currentDir.filename() == "tool") {
		candidates.push_back(currentDir.parent_path());
	}

	const std::filesystem::path executablePath = GetCurrentExecutablePath();
	if (!executablePath.empty()) {
		const std::filesystem::path executableDir = executablePath.parent_path();
		candidates.push_back(executableDir);
		if (executableDir.filename() == "tool") {
			candidates.push_back(executableDir.parent_path());
		}
	}

	for (const auto& candidate : candidates) {
		if (candidate.empty()) {
			continue;
		}
		if (HasProjectMarkers(candidate)) {
			outProjectRoot = std::filesystem::absolute(candidate);
			break;
		}
	}

	if (outProjectRoot.empty()) {
		outError = "default_pack_project_root_not_found";
		return false;
	}

	json infoJson;
	if (!ReadInfoJson(outProjectRoot, infoJson, outError)) {
		return false;
	}

	const std::string outputFileName = ResolveDefaultOutputFileName(infoJson, outProjectRoot);
	std::error_code ec;
	std::filesystem::create_directories(outProjectRoot / "pack", ec);
	if (ec) {
		outError = "create_pack_dir_failed: " + PathToUtf8(outProjectRoot / "pack");
		return false;
	}

	outOutputFile = outProjectRoot / "pack" / std::filesystem::path(Utf8ToWide(outputFileName));
	return true;
}

bool ResolvePackOutputPath(
	const std::filesystem::path& projectRoot,
	const std::filesystem::path& requestedOutputPath,
	std::filesystem::path& outOutputPath,
	std::string& outError)
{
	outOutputPath = requestedOutputPath;
	outError.clear();

	json infoJson;
	if (!ReadInfoJson(projectRoot, infoJson, outError)) {
		return false;
	}

	std::string sourceFileKind = infoJson.value("sourceFileKind", std::string());
	if (sourceFileKind.empty()) {
		const std::string sourceFileName = infoJson.value("sourceFileName", std::string());
		if (!sourceFileName.empty()) {
			sourceFileKind = DetectSourceFileKind(std::filesystem::path(Utf8ToWide(sourceFileName)));
		}
	}

	if (!IsEcSourceFileKind(sourceFileKind)) {
		return true;
	}

	std::string extension = outOutputPath.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	if (extension != ".e") {
		outOutputPath += L".e";
	}
	return true;
}

bool ValidateInfoJsonVersion(const std::filesystem::path& projectRoot, std::string& outError)
{
	outError.clear();

	json infoJson;
	if (!ReadInfoJson(projectRoot, infoJson, outError)) {
		return false;
	}

	if (!infoJson.contains("version")) {
		outError = "info_json_missing_version: " + PathToUtf8(projectRoot / "info.json");
		return false;
	}

	const int version = infoJson.value("version", -1);
	if (version != kSupportedInfoVersion) {
		outError =
			"info_json_version_unsupported: version=" + std::to_string(version) +
			", supported=" + std::to_string(kSupportedInfoVersion) +
			", file=" + PathToUtf8(projectRoot / "info.json");
		return false;
	}

	return true;
}

}  // namespace workspace_support
