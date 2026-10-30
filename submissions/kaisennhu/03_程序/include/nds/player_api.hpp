#pragma once

#include "nds/framework.hpp"

#include <map>
#include <memory>

namespace nds {

// Contestants replace the definition in player/solution.cpp.
// The evaluator compiles that file against this header and the organizer's
// simulator. resources and fixed_parameters are the trace instance values.
std::unique_ptr<SchedulerPolicy> create_policy(
    const std::map<std::string, std::int64_t>& resources,
    const std::map<std::string, std::int64_t>& fixed_parameters);

}  // namespace nds
