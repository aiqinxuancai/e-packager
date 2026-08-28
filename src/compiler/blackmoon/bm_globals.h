// BlackMoonNG MIT 适配层：独立转换器只保留易语言安装目录上下文。
#pragma once

#include <string>

namespace bm {

struct PathInfo {
	std::string eidePath;
};

extern PathInfo g_path;

}  // namespace bm
