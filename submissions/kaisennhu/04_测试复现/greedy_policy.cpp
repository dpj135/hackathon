// Run the SDK's unchanged greedy policy through its official metered player entry.
#include "nds/player_api.hpp"
std::unique_ptr<nds::SchedulerPolicy> nds::create_policy(
    const std::map<std::string, std::int64_t>& resources,
    const std::map<std::string, std::int64_t>&) {
    return std::make_unique<nds::GreedyBaseline>(resources);
}
