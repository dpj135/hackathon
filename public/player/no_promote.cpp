#include "nds/player_api.hpp"

#include <memory>

// 负例：路径选择和参考贪心相同，但从不 PROMOTE。
// 用来确认热键用例上的分数能把“会提升”和“不会提升”分开。
std::unique_ptr<nds::SchedulerPolicy> nds::create_policy(
    const std::map<std::string, std::int64_t>& resources,
    const std::map<std::string, std::int64_t>& fixed_parameters) {
    (void)fixed_parameters;
    return std::make_unique<nds::GreedyBaseline>(resources, false);
}
