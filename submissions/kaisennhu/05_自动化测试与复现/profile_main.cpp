// Keep the official allocation meter and entry point; add callback timing only.
#include <chrono>
#include <iostream>
#include <memory>
#include "nds/player_api.hpp"
namespace {
double max_submit_ms = 0, max_complete_ms = 0, total_ms = 0;
std::int64_t submit_calls = 0, complete_calls = 0;
class TimedPolicy final : public nds::SchedulerPolicy {
    std::unique_ptr<nds::SchedulerPolicy> policy_;
    using Clock = std::chrono::steady_clock;
public:
    explicit TimedPolicy(std::unique_ptr<nds::SchedulerPolicy> p) : policy_(std::move(p)) {}
    nds::Json on_submit(const nds::Submit& s, const nds::Json& r) override {
        auto start = Clock::now();
        auto result = policy_->on_submit(s, r);
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        max_submit_ms = std::max(max_submit_ms, ms); total_ms += ms; ++submit_calls;
        return result;
    }
    std::optional<nds::Json> on_iocb_complete(const nds::Iocb& i, const nds::Json& r) override {
        auto start = Clock::now();
        auto result = policy_->on_iocb_complete(i, r);
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        max_complete_ms = std::max(max_complete_ms, ms); total_ms += ms; ++complete_calls;
        return result;
    }
};
}
namespace nds {
std::unique_ptr<SchedulerPolicy> create_profiled_policy(
    const std::map<std::string, std::int64_t>& r,
    const std::map<std::string, std::int64_t>& c) {
    return std::make_unique<TimedPolicy>(create_policy(r, c));
}
}
// This includes the exact bundled upstream source, including all allocation hooks.
#define create_policy create_profiled_policy
#define main metered_player_main
#include "../01_调度算法源代码/src/player_main.cpp"
#undef main
#undef create_policy
int main(int argc, char** argv) {
    int status = metered_player_main(argc, argv);
    if (status == 0) {
        auto p = nds::Json::object();
        p["submit_calls"] = submit_calls; p["complete_calls"] = complete_calls;
        p["max_on_submit_ms"] = max_submit_ms; p["max_on_complete_ms"] = max_complete_ms;
        p["policy_total_ms"] = total_ms;
        std::cerr << "NDS_POLICY_TIMING " << p.dump(-1) << '\n';
    }
    return status;
}
