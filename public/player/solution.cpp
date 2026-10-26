#include "nds/player_api.hpp"

#include <memory>

// 默认策略是逐条贪心。公开集参考分来自会合并连续区间的调度，
// 这份实现的自动分应落在 60 到 80。高于参考分才表示超过主办方参考调度。
// 比赛提交时只替换本文件。可以在同目录放头文件并用 #include 引入，
// 评测编译命令只编译 solution.cpp，不会编译提交目录里的其他 .cpp。
std::unique_ptr<nds::SchedulerPolicy> nds::create_policy(
    const std::map<std::string, std::int64_t>& resources,
    const std::map<std::string, std::int64_t>& fixed_parameters) {
    (void)fixed_parameters;
    return std::make_unique<nds::GreedyBaseline>(resources, true);
}
