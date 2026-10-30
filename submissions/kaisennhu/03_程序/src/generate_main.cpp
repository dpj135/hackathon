#include "nds/framework.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using nds::Json;
constexpr std::int64_t KiB = 1024;
constexpr std::int64_t MiB = 1024 * 1024;

Json object(std::initializer_list<std::pair<const std::string, Json>> values) {
    Json result = Json::object();
    for (const auto& [key, value] : values) result[key] = value;
    return result;
}

struct Config {
    std::string output;
    std::string preset{"balanced"};
    int submits{8};
    int iocbs_min{16};
    int iocbs_max{32};
    int iovs_min{1};
    int iovs_max{4};
    double reuse_rate{0.20};
    double unaligned_rate{0.30};
    double adjacent_rate{0.30};
    double hbm_fragment_rate{0.50};
    bool hbm_unaligned{false};
    int hotset_size{64};
    int cycle_per_card{0};
    std::string size_profile{"mixed"};
    std::vector<std::int64_t> custom_sizes;
    std::int64_t arrival_gap_us{100};
    std::int64_t arrival_jitter_us{0};
    int cards{8};
    bool balance_cards{false};
    std::string instance_id;
    bool keep_metadata{true};
    std::uint64_t seed{1};

    std::int64_t nvme_bw{21504};
    std::int64_t iocb_capacity{1024};
    std::int64_t hbm_capacity{256};
    std::int64_t ddr_capacity{1024};
    std::int64_t cpu_copy_bw{30720};
    std::int64_t h2d_bw{20480};
    std::int64_t d2h_bw{20480};
    std::int64_t d2d_bw{1572864};
    std::int64_t d2d_concurrency{1024};
    std::int64_t h2d_concurrency{1024};
    std::int64_t d2h_concurrency{10};
    std::int64_t mooncake_capacity{10248576};
    std::int64_t promote_ttl_us{60000000};
};

struct ObjectTemplate {
    std::int64_t device_id{};
    std::int64_t ssd_offset{};
    std::vector<std::int64_t> lengths;
    bool unaligned{};

    std::int64_t total_length() const {
        std::int64_t total = 0;
        for (auto length : lengths) total += length;
        return total;
    }
};

[[noreturn]] void usage(int status = 0) {
    std::ostream& out = status == 0 ? std::cout : std::cerr;
    out << R"(NDS configurable trace generator

Usage:
  nds_generate --output FILE [options]

Scale:
  --preset NAME                 balanced|aligned|low-unaligned|high-unaligned|
                                fragmented|reuse-hot|no-reuse|ttl-expiry|
                                buffer-pressure|concurrency-pressure
  --submits N                   number of IO_SUBMIT events
  --iocbs N                     exact iocbs per submit
  --iocbs-min N --iocbs-max N   randomized iocbs per submit (1..1024)
  --iovs N                      exact iovs per iocb
  --iovs-min N --iovs-max N     randomized iovs per iocb

Workload mix (rates are 0..1):
  --reuse-rate R                probability of reusing an existing SSD object
  --unaligned-rate R            probability a new object is non-512B-aligned
  --adjacent-rate R             probability a new SSD range follows previous range
  --hbm-fragment-rate R         probability of a gap between adjacent HBM iovs
  --hbm-unaligned               keep SSD and length 512B-aligned, shift HBM by 1B
  --hotset-size N               maximum recent objects selected for reuse
  --cycle-per-card N            each card repeats a fixed set of N objects
  --size-profile NAME           tiny|mixed|large
  --sizes CSV                   custom logical iov sizes, e.g. 512,584,4096,65536
  --arrival-gap-us N            base interval between submits
  --arrival-jitter-us N         non-negative random additional interval
  --cards N                     device/card IDs are generated in [0,N)
  --balance-cards               each submit has an equal iocb count per card
  --seed N                      deterministic random seed
  --instance-id TEXT            override instance_id
  --no-metadata                 omit generator_metadata from the trace

Resource pressure:
  --iocb-capacity N
  --hbm-capacity-mb N
  --ddr-capacity-mb N
  --mooncake-capacity-mb N
  --h2d-concurrency N
  --d2h-concurrency N
  --d2d-concurrency N
  --promote-ttl-us N

The output contains generator_metadata with requested and actual distributions.
)";
    std::exit(status);
}

int parse_int(const std::string& text, const std::string& name, int min_value = 1,
              int max_value = std::numeric_limits<int>::max()) {
    std::size_t used = 0;
    long long value = 0;
    try { value = std::stoll(text, &used); } catch (...) { throw std::runtime_error(name + " is not an integer"); }
    if (used != text.size() || value < min_value || value > max_value) {
        throw std::runtime_error(name + " is outside its valid range");
    }
    return static_cast<int>(value);
}

std::int64_t parse_i64(const std::string& text, const std::string& name,
                       std::int64_t min_value = 0) {
    std::size_t used = 0;
    long long value = 0;
    try { value = std::stoll(text, &used); } catch (...) { throw std::runtime_error(name + " is not an integer"); }
    if (used != text.size() || value < min_value) throw std::runtime_error(name + " is outside its valid range");
    return value;
}

double parse_rate(const std::string& text, const std::string& name) {
    std::size_t used = 0;
    double value = 0;
    try { value = std::stod(text, &used); } catch (...) { throw std::runtime_error(name + " is not a number"); }
    if (used != text.size() || value < 0 || value > 1) throw std::runtime_error(name + " must be in [0,1]");
    return value;
}

std::vector<std::int64_t> parse_sizes(const std::string& text) {
    std::vector<std::int64_t> result;
    std::stringstream input(text);
    std::string token;
    while (std::getline(input, token, ',')) result.push_back(parse_i64(token, "size", 1));
    if (result.empty()) throw std::runtime_error("--sizes must not be empty");
    return result;
}

void apply_preset(Config& c, const std::string& preset) {
    c = Config{};
    c.preset = preset;
    if (preset == "balanced") return;
    if (preset == "aligned") {
        c.unaligned_rate = 0; c.reuse_rate = 0; c.adjacent_rate = 0.4;
    } else if (preset == "low-unaligned") {
        c.unaligned_rate = 0.05; c.reuse_rate = 0.1;
    } else if (preset == "high-unaligned") {
        c.unaligned_rate = 0.9; c.reuse_rate = 0.1; c.iovs_max = 8;
    } else if (preset == "fragmented") {
        c.submits = 12; c.iocbs_min = 32; c.iocbs_max = 64;
        c.iovs_min = 4; c.iovs_max = 16; c.unaligned_rate = 0.8;
        c.adjacent_rate = 0.8; c.hbm_fragment_rate = 1.0; c.size_profile = "tiny";
    } else if (preset == "reuse-hot") {
        c.submits = 24; c.iocbs_min = 32; c.iocbs_max = 64;
        c.reuse_rate = 0.8; c.hotset_size = 24; c.arrival_gap_us = 5000;
    } else if (preset == "no-reuse") {
        c.submits = 16; c.iocbs_min = 32; c.iocbs_max = 64; c.reuse_rate = 0;
    } else if (preset == "ttl-expiry") {
        c.submits = 16; c.iocbs_min = 16; c.iocbs_max = 32;
        c.reuse_rate = 0.75; c.hotset_size = 16;
        c.promote_ttl_us = 1000; c.arrival_gap_us = 5000;
    } else if (preset == "buffer-pressure") {
        c.submits = 6; c.iocbs_min = 32; c.iocbs_max = 64;
        c.iovs_min = 2; c.iovs_max = 6; c.unaligned_rate = 0.9;
        c.size_profile = "mixed"; c.hbm_capacity = 1; c.ddr_capacity = 4;
    } else if (preset == "concurrency-pressure") {
        c.submits = 4; c.iocbs_min = 256; c.iocbs_max = 512;
        c.iovs_min = 1; c.iovs_max = 2; c.size_profile = "tiny";
        c.reuse_rate = 0; c.unaligned_rate = 0.4; c.arrival_gap_us = 5;
        c.iocb_capacity = 128; c.h2d_concurrency = 16; c.d2d_concurrency = 16;
    } else {
        throw std::runtime_error("unknown preset: " + preset);
    }
}

std::string value_after(int& index, int argc, char** argv, const std::string& option) {
    if (++index >= argc) throw std::runtime_error(option + " requires a value");
    return argv[index];
}

Config parse_args(int argc, char** argv) {
    if (argc == 1) usage(2);
    std::string preset = "balanced";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") usage();
        if (arg == "--preset") preset = value_after(i, argc, argv, arg);
    }
    Config c;
    apply_preset(c, preset);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--preset") { ++i; continue; }
        if (arg == "--output" || arg == "-o") c.output = value_after(i, argc, argv, arg);
        else if (arg == "--submits") c.submits = parse_int(value_after(i, argc, argv, arg), arg);
        else if (arg == "--iocbs") c.iocbs_min = c.iocbs_max = parse_int(value_after(i, argc, argv, arg), arg, 1, 1024);
        else if (arg == "--iocbs-min") c.iocbs_min = parse_int(value_after(i, argc, argv, arg), arg, 1, 1024);
        else if (arg == "--iocbs-max") c.iocbs_max = parse_int(value_after(i, argc, argv, arg), arg, 1, 1024);
        else if (arg == "--iovs") c.iovs_min = c.iovs_max = parse_int(value_after(i, argc, argv, arg), arg, 1, 4096);
        else if (arg == "--iovs-min") c.iovs_min = parse_int(value_after(i, argc, argv, arg), arg, 1, 4096);
        else if (arg == "--iovs-max") c.iovs_max = parse_int(value_after(i, argc, argv, arg), arg, 1, 4096);
        else if (arg == "--reuse-rate") c.reuse_rate = parse_rate(value_after(i, argc, argv, arg), arg);
        else if (arg == "--unaligned-rate") c.unaligned_rate = parse_rate(value_after(i, argc, argv, arg), arg);
        else if (arg == "--adjacent-rate") c.adjacent_rate = parse_rate(value_after(i, argc, argv, arg), arg);
        else if (arg == "--hbm-fragment-rate") c.hbm_fragment_rate = parse_rate(value_after(i, argc, argv, arg), arg);
        else if (arg == "--hbm-unaligned") c.hbm_unaligned = true;
        else if (arg == "--hotset-size") c.hotset_size = parse_int(value_after(i, argc, argv, arg), arg);
        else if (arg == "--cycle-per-card") c.cycle_per_card = parse_int(value_after(i, argc, argv, arg), arg, 1);
        else if (arg == "--size-profile") c.size_profile = value_after(i, argc, argv, arg);
        else if (arg == "--sizes") c.custom_sizes = parse_sizes(value_after(i, argc, argv, arg));
        else if (arg == "--arrival-gap-us") c.arrival_gap_us = parse_i64(value_after(i, argc, argv, arg), arg);
        else if (arg == "--arrival-jitter-us") c.arrival_jitter_us = parse_i64(value_after(i, argc, argv, arg), arg);
        else if (arg == "--cards") c.cards = parse_int(value_after(i, argc, argv, arg), arg, 1, 1024);
        else if (arg == "--balance-cards") c.balance_cards = true;
        else if (arg == "--seed") c.seed = static_cast<std::uint64_t>(parse_i64(value_after(i, argc, argv, arg), arg));
        else if (arg == "--instance-id") c.instance_id = value_after(i, argc, argv, arg);
        else if (arg == "--no-metadata") c.keep_metadata = false;
        else if (arg == "--iocb-capacity") c.iocb_capacity = parse_i64(value_after(i, argc, argv, arg), arg, 1);
        else if (arg == "--hbm-capacity-mb") c.hbm_capacity = parse_i64(value_after(i, argc, argv, arg), arg, 1);
        else if (arg == "--ddr-capacity-mb") c.ddr_capacity = parse_i64(value_after(i, argc, argv, arg), arg, 1);
        else if (arg == "--mooncake-capacity-mb") c.mooncake_capacity = parse_i64(value_after(i, argc, argv, arg), arg, 1);
        else if (arg == "--h2d-concurrency") c.h2d_concurrency = parse_i64(value_after(i, argc, argv, arg), arg, 1);
        else if (arg == "--d2h-concurrency") c.d2h_concurrency = parse_i64(value_after(i, argc, argv, arg), arg, 1);
        else if (arg == "--d2d-concurrency") c.d2d_concurrency = parse_i64(value_after(i, argc, argv, arg), arg, 1);
        else if (arg == "--promote-ttl-us") c.promote_ttl_us = parse_i64(value_after(i, argc, argv, arg), arg, 1);
        else if (arg == "--help" || arg == "-h") usage();
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (c.output.empty()) throw std::runtime_error("--output is required");
    if (c.iocbs_min > c.iocbs_max || c.iovs_min > c.iovs_max) {
        throw std::runtime_error("minimum count must not exceed maximum count");
    }
    static const std::vector<std::string> profiles = {"tiny", "mixed", "large"};
    if (c.custom_sizes.empty() && std::find(profiles.begin(), profiles.end(), c.size_profile) == profiles.end()) {
        throw std::runtime_error("unknown size profile: " + c.size_profile);
    }
    return c;
}

std::vector<std::int64_t> sizes_for(const Config& c) {
    if (!c.custom_sizes.empty()) return c.custom_sizes;
    if (c.size_profile == "tiny") return {512, 584, 768, 1024, 4096, 8192};
    if (c.size_profile == "large") return {64 * KiB, 128 * KiB, 1 * MiB, 2 * MiB, 8 * MiB};
    return {512, 584, 768, 4096, 32 * KiB, 64 * KiB, 128 * KiB, 1 * MiB};
}

std::int64_t align_up(std::int64_t value, std::int64_t alignment = 512) {
    return (value + alignment - 1) / alignment * alignment;
}

class Generator {
public:
    explicit Generator(Config config) : c_(std::move(config)), rng_(c_.seed), sizes_(sizes_for(c_)) {
        next_hbm_.resize(c_.cards);
        next_ssd_.resize(c_.cards);
        previous_ssd_end_.assign(c_.cards, 0);
        cycles_.assign(static_cast<std::size_t>(c_.cards), {});
        cycle_cursor_.assign(static_cast<std::size_t>(c_.cards), 0);
        for (int card = 0; card < c_.cards; ++card) {
            next_hbm_[card] = 0x1000000000LL + static_cast<std::int64_t>(card) * 0x100000000LL;
            next_ssd_[card] = 0x100000 + static_cast<std::int64_t>(card) * 0x100000000LL;
        }
    }

    Json generate();

private:
    Config c_;
    std::mt19937_64 rng_;
    std::vector<std::int64_t> sizes_;
    std::vector<ObjectTemplate> objects_;
    std::vector<std::vector<ObjectTemplate>> cycles_;
    std::vector<int> cycle_cursor_;
    std::vector<std::int64_t> next_hbm_;
    std::vector<std::int64_t> next_ssd_;
    std::vector<std::int64_t> previous_ssd_end_;
    std::int64_t logical_iocbs_{0};
    std::int64_t reused_iocbs_{0};
    std::int64_t unaligned_iocbs_{0};
    std::int64_t logical_unaligned_iocbs_{0};
    std::int64_t adjacent_iocbs_{0};
    std::int64_t fragmented_edges_{0};
    std::int64_t total_iov_edges_{0};
    std::int64_t total_iovs_{0};
    std::int64_t logical_bytes_{0};
    std::map<std::int64_t, std::int64_t> size_counts_;

    bool chance(double rate) { return std::bernoulli_distribution(rate)(rng_); }
    int range(int minimum, int maximum) {
        return std::uniform_int_distribution<int>(minimum, maximum)(rng_);
    }
    std::int64_t i64_range(std::int64_t minimum, std::int64_t maximum) {
        return std::uniform_int_distribution<std::int64_t>(minimum, maximum)(rng_);
    }
    std::int64_t choose_size() { return sizes_[range(0, static_cast<int>(sizes_.size()) - 1)]; }
    int choose_iocb_count();
    ObjectTemplate make_object(std::optional<int> pinned_device);
    Json make_iocb(std::int64_t iocb_id, const ObjectTemplate& item);
    Json instance() const;
    Json metadata() const;
};

int Generator::choose_iocb_count() {
    if (!c_.balance_cards) return range(c_.iocbs_min, c_.iocbs_max);
    int lo = c_.iocbs_min;
    int hi = c_.iocbs_max;
    if (lo % c_.cards) lo += c_.cards - (lo % c_.cards);
    hi -= hi % c_.cards;
    if (lo > hi || lo < 1 || hi > 1024) {
        throw std::runtime_error("--balance-cards requires an iocb count range containing a multiple of --cards");
    }
    const int steps = (hi - lo) / c_.cards;
    return lo + range(0, steps) * c_.cards;
}

ObjectTemplate Generator::make_object(std::optional<int> pinned_device) {
    ObjectTemplate object;
    object.device_id = pinned_device ? *pinned_device : range(0, c_.cards - 1);
    const std::size_t device = static_cast<std::size_t>(object.device_id);
    object.unaligned = chance(c_.unaligned_rate);
    int iov_count = range(c_.iovs_min, c_.iovs_max);
    for (int i = 0; i < iov_count; ++i) {
        std::int64_t length = choose_size();
        if (object.unaligned) {
            if (length % 512 == 0) length += 1 + (i == 0 ? 71 : range(0, 200));
        } else {
            length = std::max<std::int64_t>(512, align_up(length));
        }
        object.lengths.push_back(length);
    }
    const bool adjacent = previous_ssd_end_[device] != 0 && chance(c_.adjacent_rate);
    if (adjacent) {
        object.ssd_offset = previous_ssd_end_[device];
        ++adjacent_iocbs_;
    } else {
        object.ssd_offset = align_up(next_ssd_[device] + range(1, 16) * 512);
    }
    if (object.unaligned && object.ssd_offset % 512 == 0) ++object.ssd_offset;
    if (!object.unaligned) object.ssd_offset = align_up(object.ssd_offset);
    previous_ssd_end_[device] = object.ssd_offset + object.total_length();
    next_ssd_[device] = align_up(previous_ssd_end_[device] + range(1, 16) * 512);
    objects_.push_back(object);
    return object;
}

Json Generator::make_iocb(std::int64_t iocb_id, const ObjectTemplate& item) {
    Json iovs = Json::array();
    std::int64_t& hbm_cursor = next_hbm_.at(static_cast<std::size_t>(item.device_id));
    for (std::size_t index = 0; index < item.lengths.size(); ++index) {
        if (index > 0) {
            ++total_iov_edges_;
            if (chance(c_.hbm_fragment_rate)) {
                hbm_cursor += range(1, 64) * 512;
                ++fragmented_edges_;
            }
        }
        std::int64_t address = align_up(hbm_cursor);
        if (item.unaligned && index == 0) address += 1 + range(0, 510);
        else if (c_.hbm_unaligned && index == 0) address += 1;
        iovs.push_back(object({{"iov_id", static_cast<std::int64_t>(index)},
                               {"hbm_addr", address}, {"length", item.lengths[index]}}));
        hbm_cursor = address + item.lengths[index];
        ++total_iovs_;
        logical_bytes_ += item.lengths[index];
        ++size_counts_[item.lengths[index]];
    }
    hbm_cursor = align_up(hbm_cursor + 4096);
    return object({
        {"iocb_id", iocb_id}, {"opcode", "READV"}, {"device_id", item.device_id},
        {"ssd_offset", item.ssd_offset}, {"length", item.total_length()}, {"iovs", iovs},
    });
}

Json Generator::instance() const {
    Json resources = object({
        {"nvme_bw", c_.nvme_bw}, {"iocb_capacity", c_.iocb_capacity},
        {"hbm_capacity", c_.hbm_capacity}, {"ddr_capacity", c_.ddr_capacity},
        {"cpu_copy_bw", c_.cpu_copy_bw}, {"h2d_bw", c_.h2d_bw}, {"d2h_bw", c_.d2h_bw},
        {"d2d_bw", c_.d2d_bw},         {"hbm_copy_concurrency", c_.d2d_concurrency},
        {"h2d_copy_concurrency", c_.h2d_concurrency},
        {"d2h_copy_concurrency", c_.d2h_concurrency},
        {"kvcache_pool_capacity", c_.mooncake_capacity},
    });
    Json limits = object({{"max_cpus", 8}, {"max_processes", 32},
                          {"max_memory_mb", 16384}, {"decision_timeout_ms", 60000}});
    Json costs = object({
        {"promote_ttl_us", c_.promote_ttl_us}, {"time_cost_per_us", 1},
        {"CPU_NDS_FIXED_COST", 1}, {"CPU_NDS_PER_CMD_COST", 1},
        {"CPU_PROMOTE_FIXED_COST", 80}, {"CPU_PROMOTE_PER_CMD_COST", 1},
        {"CPU_FALLBACK_FIXED_COST", 8}, {"CPU_FALLBACK_PER_CMD_COST", 2},
        {"CPU_H2D_CALL_COST", 1}, {"CPU_D2H_CALL_COST", 1}, {"CPU_D2D_CALL_COST", 2},
        {"NPU_H2D_CALL_COST", 1}, {"NPU_D2H_CALL_COST", 1}, {"NPU_D2D_CALL_COST", 1},
    });
    const std::string instance_id = c_.instance_id.empty()
        ? "generated-" + c_.preset + "-seed-" + std::to_string(c_.seed) : c_.instance_id;
    return object({{"instance_id", instance_id},
                   {"resources", resources}, {"runtime_limits", limits},
                   {"fixed_parameters", costs}});
}

Json Generator::metadata() const {
    auto ratio = [](std::int64_t numerator, std::int64_t denominator) {
        return denominator ? static_cast<double>(numerator) / denominator : 0.0;
    };
    Json sizes = Json::array();
    for (auto size : sizes_) sizes.push_back(size);
    Json size_counts = Json::object();
    for (const auto& [size, count] : size_counts_) size_counts[std::to_string(size)] = count;
    return object({
        {"preset", c_.preset}, {"seed", static_cast<std::int64_t>(c_.seed)},
        {"submit_count", c_.submits}, {"logical_iocb_count", logical_iocbs_},
        {"unique_object_count", static_cast<std::int64_t>(objects_.size())},
        {"total_iov_count", total_iovs_}, {"logical_bytes", logical_bytes_},
        {"average_iovs_per_iocb", ratio(total_iovs_, logical_iocbs_)},
        {"requested_reuse_rate", c_.reuse_rate}, {"actual_reuse_rate", ratio(reused_iocbs_, logical_iocbs_)},
        {"requested_unaligned_rate", c_.unaligned_rate},
        {"actual_new_object_unaligned_rate", ratio(unaligned_iocbs_, static_cast<std::int64_t>(objects_.size()))},
        {"actual_logical_unaligned_rate", ratio(logical_unaligned_iocbs_, logical_iocbs_)},
        {"requested_adjacent_rate", c_.adjacent_rate},
        {"actual_new_object_adjacent_rate", ratio(adjacent_iocbs_, static_cast<std::int64_t>(objects_.size()))},
        {"requested_hbm_fragment_rate", c_.hbm_fragment_rate},
        {"actual_hbm_fragment_rate", ratio(fragmented_edges_, total_iov_edges_)},
        {"size_profile", c_.size_profile}, {"sizes", sizes}, {"logical_iov_size_counts", size_counts},
        {"iocbs_per_submit_min", c_.iocbs_min}, {"iocbs_per_submit_max", c_.iocbs_max},
        {"iovs_per_iocb_min", c_.iovs_min}, {"iovs_per_iocb_max", c_.iovs_max},
        {"hotset_size", c_.hotset_size}, {"cycle_per_card", c_.cycle_per_card},
        {"arrival_gap_us", c_.arrival_gap_us},
        {"arrival_jitter_us", c_.arrival_jitter_us}, {"cards", c_.cards},
        {"balance_cards", c_.balance_cards},
    });
}

Json Generator::generate() {
    Json events = Json::array();
    std::int64_t time = 0;
    for (int submit_index = 0; submit_index < c_.submits; ++submit_index) {
        if (submit_index > 0) {
            time += c_.arrival_gap_us + (c_.arrival_jitter_us ? i64_range(0, c_.arrival_jitter_us) : 0);
        }
        int iocb_count = choose_iocb_count();
        std::vector<int> devices;
        if (c_.balance_cards) {
            devices.resize(static_cast<std::size_t>(iocb_count));
            for (int i = 0; i < iocb_count; ++i) devices[static_cast<std::size_t>(i)] = i % c_.cards;
            for (int i = iocb_count - 1; i > 0; --i) {
                std::swap(devices[static_cast<std::size_t>(i)], devices[static_cast<std::size_t>(range(0, i))]);
            }
        }
        Json iocbs = Json::array();
        for (int i = 0; i < iocb_count; ++i) {
            const std::optional<int> pinned = c_.balance_cards
                ? std::optional<int>{devices[static_cast<std::size_t>(i)]} : std::nullopt;
            ObjectTemplate selected;
            bool reuse = false;
            if (c_.cycle_per_card > 0) {
                const int device = pinned ? *pinned : (i % c_.cards);
                auto& ring = cycles_.at(static_cast<std::size_t>(device));
                if (static_cast<int>(ring.size()) < c_.cycle_per_card) {
                    selected = make_object(device);
                    if (selected.unaligned) ++unaligned_iocbs_;
                    ring.push_back(selected);
                } else {
                    const std::size_t index = static_cast<std::size_t>(
                        cycle_cursor_[static_cast<std::size_t>(device)] % static_cast<int>(ring.size()));
                    ++cycle_cursor_[static_cast<std::size_t>(device)];
                    selected = ring[index];
                    reuse = true;
                    ++reused_iocbs_;
                }
            } else {
                reuse = !objects_.empty() && chance(c_.reuse_rate);
                if (reuse) {
                    int available = std::min<int>(c_.hotset_size, static_cast<int>(objects_.size()));
                    int begin = static_cast<int>(objects_.size()) - available;
                    std::vector<int> candidates;
                    for (int index = begin; index < static_cast<int>(objects_.size()); ++index) {
                        if (!pinned || objects_[static_cast<std::size_t>(index)].device_id == *pinned) {
                            candidates.push_back(index);
                        }
                    }
                    if (candidates.empty()) reuse = false;
                    else {
                        selected = objects_[static_cast<std::size_t>(candidates[range(0, static_cast<int>(candidates.size()) - 1)])];
                        ++reused_iocbs_;
                    }
                }
                if (!reuse) {
                    selected = make_object(pinned);
                    if (selected.unaligned) ++unaligned_iocbs_;
                }
            }
            if (selected.unaligned) ++logical_unaligned_iocbs_;
            iocbs.push_back(make_iocb(i + 1, selected));
            ++logical_iocbs_;
        }
        Json submit = object({{"submit_id", submit_index + 1}, {"arrival_us", time}, {"iocbs", iocbs}});
        events.push_back(object({{"event", "IO_SUBMIT"}, {"time_us", time}, {"submit", submit}}));
    }
    Json root = object({{"instance", instance()}, {"events", events}});
    if (c_.keep_metadata) root["generator_metadata"] = metadata();
    return root;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Config config = parse_args(argc, argv);
        std::string output = config.output;
        Generator generator(std::move(config));
        Json trace = generator.generate();
        (void)nds::parse_trace(trace);
        nds::write_json_file(output, trace);
        if (trace.contains("generator_metadata")) std::cout << trace.at("generator_metadata").dump(2) << '\n';
        else std::cout << "{\"metadata\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
