#include <chrono>
#include <iostream>
#include <memory>

#include "nds/player_api.hpp"
class TimedPolicy final : public nds::SchedulerPolicy {
    std::unique_ptr<nds::SchedulerPolicy> policy;
    using Clock = std::chrono::steady_clock;

   public:
    double max_submit_ms = 0, max_complete_ms = 0, total_ms = 0;
    std::int64_t submit_calls = 0, complete_calls = 0;
    explicit TimedPolicy(std::unique_ptr<nds::SchedulerPolicy> p) : policy(std::move(p)) {}
    nds::Json on_submit(const nds::Submit& s, const nds::Json& r) override {
        auto start = Clock::now();
        auto result = policy->on_submit(s, r);
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        max_submit_ms = std::max(max_submit_ms, ms);
        total_ms += ms;
        ++submit_calls;
        return result;
    }
    std::optional<nds::Json> on_iocb_complete(const nds::Iocb& i, const nds::Json& r) override {
        auto start = Clock::now();
        auto result = policy->on_iocb_complete(i, r);
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        max_complete_ms = std::max(max_complete_ms, ms);
        total_ms += ms;
        ++complete_calls;
        return result;
    }
};
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        auto t = nds::load_trace(argv[1]);
        TimedPolicy p(nds::create_policy(t.instance.resources, t.instance.fixed_parameters));
        nds::Simulator sim(std::move(t), p);
        auto result = sim.run();
        nds::write_json_file(argv[2], result);
        nds::Json profile = nds::Json::object();
        profile["submit_calls"] = p.submit_calls;
        profile["complete_calls"] = p.complete_calls;
        profile["max_on_submit_ms"] = p.max_submit_ms;
        profile["max_on_complete_ms"] = p.max_complete_ms;
        profile["policy_total_ms"] = p.total_ms;
        profile["metrics"] = result.at("metrics");
        std::cout << profile.dump(2) << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
