#pragma once

#include <string>

namespace self_update {

// 自升级执行结果。
struct UpdateResult {
	bool ok = false;
	std::string message;
};

// 查询 GitHub Release，并启动后台替换脚本更新当前可执行文件。
UpdateResult ScheduleSelfUpdate(const std::string& currentVersion, bool force);

}  // namespace self_update
