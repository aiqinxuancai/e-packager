#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace e2txt {

struct ProjectBundle;

// 合并常量页与资源索引的顺序；快照仅提供交错槽位，不覆盖各自的顺序。
std::vector<size_t> BuildConstantResourceOrder(
    const ProjectBundle& bundle,
    std::span<const size_t> localConstantIndices,
    std::span<const size_t> resourceIndices,
    size_t totalConstantCount);

}
