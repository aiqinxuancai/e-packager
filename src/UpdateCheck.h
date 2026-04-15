#pragma once

#include <string>

namespace update_check {

// 若版本字符串包含预发布标记（alpha/beta/pre/rc），返回 true。
bool IsPreRelease(const std::string& version);

// 从 GitHub 获取最新稳定版的 tag，失败时返回空字符串。
std::string FetchLatestTag();

// 若 latestTag 的语义化版本号大于 currentVersion，返回 true。
bool IsNewer(const std::string& latestTag, const std::string& currentVersion);

}  // namespace update_check
