#include "nds/player_api.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: nds_player TRACE OUTPUT\n";
        return 2;
    }
    try {
        nds::Trace trace = nds::load_trace(argv[1]);
        auto policy = nds::create_policy(trace.instance.resources, trace.instance.fixed_parameters);
        if (!policy) {
            std::cerr << "error: create_policy returned null\n";
            return 1;
        }
        nds::Simulator simulator(std::move(trace), *policy);
        nds::Json result = simulator.run();
        nds::write_json_file(argv[2], result);
        std::cout << result.at("metrics").dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
