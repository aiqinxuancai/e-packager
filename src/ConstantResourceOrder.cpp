#include "ConstantResourceOrder.h"
#include "e2txt.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>

namespace e2txt {

std::vector<size_t> BuildConstantResourceOrder(
    const ProjectBundle& bundle,
    const std::span<const size_t> localConstantIndices,
    const std::span<const size_t> resourceIndices,
    const size_t totalConstantCount)
{
    // 统计原快照中的全部常量槽，包括已删除、改名及匿名的常量。
    // 槽位不依赖当前名称，因此重命名不会改变资源的插入位置。
    std::unordered_map<std::string_view, size_t> snapshotSlots;
    size_t constantSlot = 0;
    for (const auto& key : bundle.rootChildKeys) {
        if (key.starts_with("constant:")) {
            ++constantSlot;
        } else {
            snapshotSlots.try_emplace(key, constantSlot);
        }
    }

    std::vector<size_t> order;
    order.reserve(totalConstantCount);
    std::vector<bool> assigned(totalConstantCount, false);
    const auto append = [&](const size_t index) {
        order.push_back(index);
        assigned[index] = true;
    };

    // 资源顺序以索引为准（读取目录时已按 order 合并图片与音频）。
    // 插入槽位只能前进，确保索引调序或常量增删后资源仍保持相对顺序。
    size_t nextConstant = 0;
    for (size_t ordinal = 0; ordinal < resourceIndices.size(); ++ordinal) {
        const auto slot = snapshotSlots.find(bundle.resources[ordinal].key);
        const size_t insertAt = slot == snapshotSlots.end()
            ? localConstantIndices.size()
            : (std::min)(slot->second, localConstantIndices.size());
        while (nextConstant < insertAt) {
            append(localConstantIndices[nextConstant++]);
        }
        append(resourceIndices[ordinal]);
    }
    while (nextConstant < localConstantIndices.size()) {
        append(localConstantIndices[nextConstant++]);
    }

    // 依赖模块常量不参与本地排序，按模型中的原顺序追加。
    for (size_t index = 0; index < totalConstantCount; ++index) {
        if (!assigned[index]) {
            order.push_back(index);
        }
    }
    return order;
}

}
