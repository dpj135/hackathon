#include "nds/player_api.hpp"

#include <memory>

// 负例：读路径仍是贪心，但第二次未命中一律 PROMOTE_SSD。
// 用来和 PROMOTE_HBM 对照。大对象、D2H 并发为 1 时，这条路径会错过复用窗口。
std::unique_ptr<nds::SchedulerPolicy> nds::create_policy(
    const std::map<std::string, std::int64_t>& resources,
    const std::map<std::string, std::int64_t>& fixed_parameters) {
    (void)fixed_parameters;
    return std::make_unique<nds::GreedyBaseline>(resources, true, true);
}
