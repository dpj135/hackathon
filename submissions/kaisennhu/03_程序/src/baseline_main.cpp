#include "nds/framework.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: nds_baseline TRACE OUTPUT [--no-promote]\n";
        return 2;
    }
    try {
        nds::Trace trace = nds::load_trace(argv[1]);
        bool promote = argc != 4 || std::string(argv[3]) != "--no-promote";
        nds::GreedyBaseline policy(trace.instance.resources, promote);
        nds::Simulator simulator(std::move(trace), policy);
        nds::Json result = simulator.run();
        nds::write_json_file(argv[2], result);
        std::cout << result.at("metrics").dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
