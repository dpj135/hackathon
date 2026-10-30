#include "nds/framework.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string join(const std::string& directory, const std::string& name) {
    return directory + "/" + name;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: nds_tests DATA_DIR\n";
        return 2;
    }
    try {
        const std::vector<std::string> names = {
            "small_aligned.json", "small_mixed.json", "small_repeated.json",
        };
        for (const auto& name : names) {
            nds::Trace trace = nds::load_trace(join(argv[1], name));
            nds::GreedyBaseline baseline(trace.instance.resources);
            nds::Simulator simulator(trace, baseline);
            nds::Json result = simulator.run();
            require(result.at("metrics").at("total_cost").as_int() > 0, "non-positive total cost");

            nds::ReplayPolicy replay_policy(result);
            nds::Simulator replay_simulator(trace, replay_policy);
            nds::Json replay = replay_simulator.run();
            nds::compare_completion_events(replay.at("events"), result.at("events"));
            require(replay.at("metrics") == result.at("metrics"), "replay metrics differ");
        }

        nds::Trace repeated = nds::load_trace(join(argv[1], "small_repeated.json"));
        nds::GreedyBaseline baseline(repeated.instance.resources);
        nds::Simulator simulator(repeated, baseline);
        nds::Json result = simulator.run();
        require(result.at("metrics").at("promote_count").as_int() >= 1, "promotion missing");
        require(result.at("metrics").at("cache_hits").as_int() >= 1, "cache hit missing");

        bool rejected = false;
        try {
            nds::Json bad = nds::read_json_file(join(argv[1], "small_aligned.json"));
            bad.at("events")[0].at("submit").at("iocbs")[0]["length"] = 32769;
            (void)nds::parse_trace(bad);
        } catch (const nds::ValidationError&) {
            rejected = true;
        }
        require(rejected, "invalid iocb length was accepted");

        auto expect_rejected = [](const nds::Trace& trace, nds::Json solution, const std::string& message) {
            bool failed = false;
            try {
                nds::ReplayPolicy replay_policy(solution);
                nds::Simulator replay_simulator(trace, replay_policy);
                (void)replay_simulator.run();
            } catch (const nds::ValidationError&) {
                failed = true;
            }
            require(failed, message);
        };

        {
            nds::Trace aligned = nds::load_trace(join(argv[1], "small_aligned.json"));
            nds::GreedyBaseline baseline(aligned.instance.resources);
            nds::Simulator simulator(aligned, baseline);
            nds::Json result = simulator.run();
            bool mutated = false;
            for (auto& decision : result.at("schedule_decisions").as_array()) {
                for (auto& operation : decision.at("operations").as_array()) {
                    for (auto& transfer : operation.at("transfers").as_array()) {
                        if (transfer.at("type").as_string() == "DIRECT") {
                            transfer["physical_ssd_offset"] = static_cast<std::int64_t>(0);
                            mutated = true;
                        }
                    }
                }
            }
            require(mutated, "aligned trace produced no DIRECT transfer");
            expect_rejected(aligned, result, "SSD coverage hole was accepted");

            nds::Json device_mismatch = simulator.run();
            for (auto& decision : device_mismatch.at("schedule_decisions").as_array()) {
                for (auto& operation : decision.at("operations").as_array()) {
                    for (auto& transfer : operation.at("transfers").as_array()) {
                        transfer["device_id"] = transfer.at("device_id").as_int() + 1;
                    }
                }
            }
            expect_rejected(aligned, device_mismatch, "cross-device transfer was accepted");
        }

        {
            nds::Trace mixed = nds::load_trace(join(argv[1], "small_mixed.json"));
            nds::GreedyBaseline baseline(mixed.instance.resources);
            nds::Simulator simulator(mixed, baseline);
            nds::Json result = simulator.run();
            bool mutated = false;
            for (auto& decision : result.at("schedule_decisions").as_array()) {
                for (auto& operation : decision.at("operations").as_array()) {
                    if (!operation.contains("buffer_bytes")) continue;
                    operation["buffer_bytes"] = static_cast<std::int64_t>(1);
                    mutated = true;
                }
            }
            require(mutated, "mixed trace produced no buffered operation");
            expect_rejected(mixed, result, "undersized buffer was accepted");
        }

        {
            nds::Trace repeated = nds::load_trace(join(argv[1], "small_repeated.json"));
            nds::GreedyBaseline baseline(repeated.instance.resources);
            nds::Simulator simulator(repeated, baseline);
            nds::Json result = simulator.run();
            require(result.at("promotion_decisions").as_array().size() >= 1, "expected a promotion");
            result.at("promotion_decisions")[0].at("transfer")["bytes"] = static_cast<std::int64_t>(1);
            expect_rejected(repeated, result, "short PROMOTE was accepted");
        }

        std::cout << "all C++ framework tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
