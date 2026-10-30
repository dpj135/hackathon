#include "nds/framework.hpp"

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: nds_validate TRACE SOLUTION\n";
        return 2;
    }
    try {
        nds::Trace trace = nds::load_trace(argv[1]);
        nds::Json claimed = nds::read_json_file(argv[2]);
        nds::ReplayPolicy policy(claimed);
        nds::Simulator simulator(std::move(trace), policy);
        nds::Json replay = simulator.run();
        nds::compare_completion_events(replay.at("events"), claimed.at("events"));
        if (replay.at("metrics") != claimed.at("metrics")) {
            throw nds::ValidationError("metric mismatch");
        }
        nds::Json output = nds::Json::object();
        output["valid"] = true;
        output["metrics"] = replay.at("metrics");
        std::cout << output.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "invalid: " << error.what() << '\n';
        return 1;
    }
}
