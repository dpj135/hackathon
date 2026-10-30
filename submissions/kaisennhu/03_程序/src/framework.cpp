#include "nds/framework.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace nds {
namespace {

constexpr std::int64_t KiB = 1024;
constexpr std::int64_t MiB = 1024 * 1024;
constexpr long double Epsilon = 1e-12L;
constexpr long double RemainingEpsilon = 1e-6L;

Json object(std::initializer_list<std::pair<const std::string, Json>> values) {
    Json result = Json::object();
    for (const auto& [key, value] : values) result[key] = value;
    return result;
}

Json array(std::initializer_list<Json> values) {
    Json result = Json::array();
    for (const auto& value : values) result.push_back(value);
    return result;
}

std::int64_t require_int(const Json& value, const std::string& name, bool positive = false) {
    if (!value.is_int()) throw ValidationError(name + " must be an integer");
    std::int64_t result = value.as_int();
    if ((positive && result <= 0) || (!positive && result < 0)) {
        throw ValidationError(name + (positive ? " must be positive" : " must be non-negative"));
    }
    return result;
}

std::int64_t optional_int(const Json& object_value, const std::string& key,
                          std::int64_t fallback) {
    return object_value.contains(key) ? require_int(object_value.at(key), key) : fallback;
}

std::string require_string(const Json& value, const std::string& name) {
    if (!value.is_string()) throw ValidationError(name + " must be a string");
    return value.as_string();
}

const std::set<std::string> RequiredResources = {
    "nvme_bw", "iocb_capacity", "hbm_capacity", "ddr_capacity", "cpu_copy_bw",
    "h2d_bw", "d2h_bw", "d2d_bw", "hbm_copy_concurrency",
    "h2d_copy_concurrency", "d2h_copy_concurrency", "kvcache_pool_capacity",
};

const std::set<std::string> RequiredCosts = {
    "promote_ttl_us", "time_cost_per_us", "CPU_NDS_FIXED_COST",
    "CPU_NDS_PER_CMD_COST", "CPU_PROMOTE_FIXED_COST", "CPU_PROMOTE_PER_CMD_COST",
    "CPU_FALLBACK_FIXED_COST", "CPU_FALLBACK_PER_CMD_COST", "CPU_H2D_CALL_COST",
    "CPU_D2H_CALL_COST", "CPU_D2D_CALL_COST", "NPU_H2D_CALL_COST",
    "NPU_D2H_CALL_COST", "NPU_D2D_CALL_COST",
};

Submit parse_submit(const Json& raw, std::int64_t event_time) {
    if (!raw.is_object()) throw ValidationError("submit must be an object");
    Submit submit;
    submit.submit_id = require_int(raw.at("submit_id"), "submit_id");
    submit.arrival_us = raw.contains("arrival_us")
        ? require_int(raw.at("arrival_us"), "arrival_us") : event_time;
    if (submit.arrival_us != event_time) {
        throw ValidationError("submit.arrival_us must equal event.time_us");
    }
    const auto& raw_iocbs = raw.at("iocbs").as_array();
    if (raw_iocbs.empty() || raw_iocbs.size() > 1024) {
        throw ValidationError("one io_submit must contain 1..1024 iocbs");
    }
    std::set<std::int64_t> iocb_ids;
    for (const Json& item : raw_iocbs) {
        if (!item.is_object()) throw ValidationError("iocb must be an object");
        Iocb iocb;
        iocb.submit_id = submit.submit_id;
        iocb.iocb_id = require_int(item.at("iocb_id"), "iocb_id");
        if (!iocb_ids.insert(iocb.iocb_id).second) {
            throw ValidationError("duplicate iocb_id in submit");
        }
        iocb.opcode = require_string(item.at("opcode"), "opcode");
        if (iocb.opcode != "READV") throw ValidationError("only READV is supported");
        iocb.device_id = require_int(item.at("device_id"), "device_id");
        iocb.ssd_offset = require_int(item.at("ssd_offset"), "ssd_offset");
        iocb.length = require_int(item.at("length"), "iocb.length", true);
        if (iocb.ssd_offset > std::numeric_limits<std::int64_t>::max() - iocb.length) {
            throw ValidationError("ssd range overflows");
        }
        const auto& raw_iovs = item.at("iovs").as_array();
        if (raw_iovs.empty()) throw ValidationError("iocb.iovs must not be empty");
        std::set<std::int64_t> iov_ids;
        std::int64_t total = 0;
        for (const Json& raw_iov : raw_iovs) {
            Iov iov;
            iov.iov_id = require_int(raw_iov.at("iov_id"), "iov_id");
            if (!iov_ids.insert(iov.iov_id).second) throw ValidationError("duplicate iov_id");
            iov.hbm_addr = require_int(raw_iov.at("hbm_addr"), "hbm_addr");
            iov.length = require_int(raw_iov.at("length"), "iov.length", true);
            if (iov.hbm_addr > std::numeric_limits<std::int64_t>::max() - iov.length) {
                throw ValidationError("hbm range overflows");
            }
            total += iov.length;
            iocb.iovs.push_back(iov);
        }
        if (total != iocb.length) throw ValidationError("iocb.length != sum(iov.length)");
        submit.iocbs.push_back(std::move(iocb));
    }
    return submit;
}

using Profile = std::map<std::int64_t, std::map<int, int>>;

const Profile DirectProfile = {
    {512, {{1, 2}}},
    {4 * KiB, {{1, 20}}},
    {32 * KiB, {{1, 10}, {4, 30}, {16, 50}, {64, 75}, {1024, 90}}},
    {64 * KiB, {{1, 15}, {4, 40}, {16, 60}, {64, 100}}},
    {1 * MiB, {{1, 55}, {4, 90}, {16, 100}}},
    {2 * MiB, {{1, 70}, {4, 100}}},
    {8 * MiB, {{1, 95}, {2, 100}}},
};

const Profile SsdToDdrProfile = {
    {512, {{1, 2}}},
    {4 * KiB, {{1, 15}}},
    {32 * KiB, {{1, 10}, {4, 28}, {16, 48}, {64, 75}, {1024, 85}}},
    {64 * KiB, {{1, 15}, {4, 40}, {16, 60}, {64, 90}}},
    {1 * MiB, {{1, 50}, {4, 80}, {16, 90}}},
    {2 * MiB, {{1, 60}, {4, 90}}},
    {8 * MiB, {{1, 85}, {2, 90}}},
};

const Profile H2dD2hProfile = {
    {512, {{1, 1}, {4, 2}, {16, 3}, {256, 4}}},
    {4 * KiB, {{1, 1}, {4, 2}, {256, 3}}},
    {32 * KiB, {{1, 6}, {4, 14}, {16, 20}, {64, 22}, {1024, 24}}},
    {64 * KiB, {{1, 12}, {4, 26}, {16, 37}, {256, 45}}},
    {1 * MiB, {{1, 50}, {4, 90}, {16, 100}}},
    {2 * MiB, {{1, 75}, {4, 100}}},
    {8 * MiB, {{1, 80}}},
};

const Profile D2dProfile = {
    {512, {{1, 20}}},
    {4 * KiB, {{1, 130}}},
    {32 * KiB, {{1, 1500}, {4, 5000}, {16, 20000}, {64, 65000},
                {256, 200000}, {1024, 450000}}},
    {64 * KiB, {{1, 4000}, {4, 14000}, {16, 40000}, {64, 150000},
                {256, 360000}, {1024, 520000}}},
};

template <typename Map>
auto floor_iterator(const Map& map, typename Map::key_type value) {
    auto it = map.upper_bound(value);
    if (it == map.begin()) return map.begin();
    return std::prev(it);
}

int bucket_value(const Profile& profile, std::int64_t size, int concurrency) {
    const auto size_it = floor_iterator(profile, size);
    const auto& q_table = size_it->second;
    return floor_iterator(q_table, std::max(1, concurrency))->second;
}

long double bytes_per_us(std::int64_t mb_s) {
    return static_cast<long double>(mb_s) * MiB / 1000000.0L;
}

long double percent_rate(std::int64_t base_mb_s, int utilization, int concurrency) {
    return bytes_per_us(base_mb_s) * utilization / 100.0L / std::max(1, concurrency);
}

bool is_ssd_type(const std::string& kind) {
    return kind == "DIRECT" || kind == "SSD_TO_HBM_BUFFER" ||
           kind == "SSD_TO_DDR" || kind == "SSD_TO_MOONCAKE";
}

const Iocb& find_iocb(const Submit& submit, std::int64_t id) {
    auto it = std::find_if(submit.iocbs.begin(), submit.iocbs.end(),
                           [&](const Iocb& item) { return item.iocb_id == id; });
    if (it == submit.iocbs.end()) throw ValidationError("unknown iocb " + std::to_string(id));
    return *it;
}

std::vector<std::tuple<std::int64_t, std::int64_t, std::int64_t>>
iocb_fragments(const Iocb& iocb, std::int64_t offset, std::int64_t length) {
    if (offset < 0 || length <= 0 || offset + length > iocb.length) {
        throw ValidationError("range exceeds iocb");
    }
    std::vector<std::tuple<std::int64_t, std::int64_t, std::int64_t>> result;
    std::int64_t cursor = 0;
    for (const auto& iov : iocb.iovs) {
        std::int64_t begin = std::max(offset, cursor);
        std::int64_t end = std::min(offset + length, cursor + iov.length);
        if (begin < end) result.emplace_back(iov.iov_id, iov.hbm_addr + begin - cursor, end - begin);
        cursor += iov.length;
    }
    return result;
}

std::int64_t ceil_us(long double value) {
    return static_cast<std::int64_t>(std::ceil(value - Epsilon));
}

double round6(long double value) {
    return std::round(static_cast<double>(value) * 1000000.0) / 1000000.0;
}

}  // namespace

Trace parse_trace(const Json& root) {
    if (!root.is_object()) throw ValidationError("trace root must be an object");
    const Json& raw_instance = root.at("instance");
    if (!raw_instance.is_object()) throw ValidationError("instance must be an object");
    Trace trace;
    trace.instance.instance_id = raw_instance.contains("instance_id")
        ? require_string(raw_instance.at("instance_id"), "instance_id") : "nds-instance";

    const Json& resources = raw_instance.at("resources");
    const Json& limits = raw_instance.at("runtime_limits");
    const Json& costs = raw_instance.at("fixed_parameters");
    for (const auto& [key, value] : resources.as_object()) {
        const std::string canonical = key == "mooncake_capacity" ? "kvcache_pool_capacity" : key;
        const std::int64_t parsed = require_int(value, "resources." + key, true);
        const auto existing = trace.instance.resources.find(canonical);
        if (existing != trace.instance.resources.end() && existing->second != parsed) {
            throw ValidationError("kvcache_pool_capacity and mooncake_capacity differ");
        }
        trace.instance.resources[canonical] = parsed;
    }
    for (const auto& key : RequiredResources) {
        if (!trace.instance.resources.count(key)) throw ValidationError("missing resource: " + key);
    }
    for (const auto& [key, value] : limits.as_object()) {
        trace.instance.runtime_limits[key] = require_int(value, "runtime_limits." + key, true);
    }
    for (const auto& key : RequiredCosts) {
        if (!costs.contains(key)) throw ValidationError("missing fixed parameter: " + key);
    }
    for (const auto& [key, value] : costs.as_object()) {
        trace.instance.fixed_parameters[key] = require_int(value, "fixed_parameters." + key);
    }

    const auto& events = root.at("events").as_array();
    if (events.empty()) throw ValidationError("events must not be empty");
    std::set<std::int64_t> submit_ids;
    std::int64_t previous = -1;
    for (const Json& raw_event : events) {
        if (require_string(raw_event.at("event"), "event") != "IO_SUBMIT") {
            throw ValidationError("trace events must be IO_SUBMIT");
        }
        TraceEvent event;
        event.time_us = require_int(raw_event.at("time_us"), "time_us");
        if (event.time_us < previous) throw ValidationError("events must be sorted by time_us");
        previous = event.time_us;
        event.submit = parse_submit(raw_event.at("submit"), event.time_us);
        if (!submit_ids.insert(event.submit.submit_id).second) throw ValidationError("duplicate submit_id");
        trace.events.push_back(std::move(event));
    }
    return trace;
}

Trace load_trace(const std::string& path) { return parse_trace(read_json_file(path)); }

std::int64_t align_down(std::int64_t value, std::int64_t alignment) {
    return value / alignment * alignment;
}

std::int64_t align_up(std::int64_t value, std::int64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

std::pair<std::int64_t, std::int64_t> physical_span(std::int64_t offset,
                                                     std::int64_t length) {
    std::int64_t begin = align_down(offset);
    std::int64_t end = align_up(offset + length);
    return {begin, end - begin};
}

namespace {

struct ByteInterval {
    std::int64_t begin{};
    std::int64_t end{};
};

std::vector<ByteInterval> merge_intervals(std::vector<ByteInterval> intervals) {
    std::sort(intervals.begin(), intervals.end(), [](const ByteInterval& lhs, const ByteInterval& rhs) {
        return lhs.begin < rhs.begin || (lhs.begin == rhs.begin && lhs.end < rhs.end);
    });
    std::vector<ByteInterval> merged;
    for (const auto& interval : intervals) {
        if (interval.end <= interval.begin) continue;
        if (merged.empty() || interval.begin > merged.back().end) merged.push_back(interval);
        else merged.back().end = std::max(merged.back().end, interval.end);
    }
    return merged;
}

bool interval_covered(const ByteInterval& required, const std::vector<ByteInterval>& merged) {
    for (const auto& interval : merged) {
        if (interval.begin <= required.begin && interval.end >= required.end) return true;
    }
    return false;
}

void require_intervals_covered(const std::vector<ByteInterval>& required,
                               const std::vector<ByteInterval>& provided,
                               const std::string& message) {
    const auto merged = merge_intervals(provided);
    for (const auto& interval : required) {
        if (!interval_covered(interval, merged)) throw ValidationError(message);
    }
}

void require_exact_cover(const std::vector<ByteInterval>& required,
                         const std::vector<ByteInterval>& provided,
                         const std::string& message) {
    require_intervals_covered(required, provided, message);
    const auto required_merged = merge_intervals(required);
    for (const auto& interval : provided) {
        if (!interval_covered(interval, required_merged)) throw ValidationError(message);
    }
}

}  // namespace

void validate_decision(const Submit& submit, const Json& decision,
                       const std::map<std::string, std::int64_t>& resources) {
    if (require_string(decision.at("event"), "decision.event") != "SCHEDULE_DECISION") {
        throw ValidationError("scheduler must return SCHEDULE_DECISION");
    }
    if (require_int(decision.at("submit_id"), "decision.submit_id") != submit.submit_id) {
        throw ValidationError("decision submit_id mismatch");
    }
    const auto& operations = decision.at("operations").as_array();
    std::set<std::int64_t> operation_ids;
    std::set<std::int64_t> transfer_ids;
    std::map<std::int64_t, std::vector<std::pair<std::int64_t, std::int64_t>>> coverage;
    std::map<std::int64_t, std::vector<std::int64_t>> edges;
    std::map<std::int64_t, int> indegree;

    for (const Json& operation : operations) {
        std::int64_t operation_id = require_int(operation.at("operation_id"), "operation_id");
        if (!operation_ids.insert(operation_id).second) throw ValidationError("duplicate operation_id");
        std::string path = require_string(operation.at("path"), "path");
        if (path != "DIRECT" && path != "AGGREGATE" && path != "FALLBACK") {
            throw ValidationError("invalid foreground path");
        }
        const auto& ranges = operation.at("ranges").as_array();
        const auto& transfers = operation.at("transfers").as_array();
        if (ranges.empty() || transfers.empty()) throw ValidationError("operation ranges/transfers empty");
        std::optional<std::int64_t> operation_device;
        std::vector<ByteInterval> physical_ssd_required;
        std::map<std::pair<std::int64_t, std::int64_t>, std::vector<ByteInterval>> logical_iovs;
        for (const Json& range : ranges) {
            std::int64_t iocb_id = require_int(range.at("iocb_id"), "range.iocb_id");
            std::int64_t offset = require_int(range.at("iocb_offset"), "range.iocb_offset");
            std::int64_t length = require_int(range.at("length"), "range.length", true);
            const Iocb& iocb = find_iocb(submit, iocb_id);
            if (!operation_device) operation_device = iocb.device_id;
            else if (*operation_device != iocb.device_id) {
                throw ValidationError("operation mixes device_id values");
            }
            const std::int64_t ssd_begin = iocb.ssd_offset + offset;
            if (path == "DIRECT") physical_ssd_required.push_back({ssd_begin, ssd_begin + length});
            else {
                const auto span = physical_span(ssd_begin, length);
                physical_ssd_required.push_back({span.first, span.first + span.second});
            }
            std::int64_t iov_cursor = 0;
            for (const auto& iov : iocb.iovs) {
                const std::int64_t begin = std::max(offset, iov_cursor);
                const std::int64_t end = std::min(offset + length, iov_cursor + iov.length);
                if (begin < end) {
                    logical_iovs[{iocb.iocb_id, iov.iov_id}].push_back({begin - iov_cursor, end - iov_cursor});
                }
                iov_cursor += iov.length;
            }
            auto fragments = iocb_fragments(iocb, offset, length);
            if (path == "DIRECT") {
                if ((iocb.ssd_offset + offset) % 512 != 0 || length % 512 != 0) {
                    throw ValidationError("DIRECT SSD offset and length must be 512B aligned");
                }
                for (const auto& [iov_id, addr, fragment_length] : fragments) {
                    (void)iov_id;
                    if (addr % 512 != 0 || fragment_length % 512 != 0) {
                        throw ValidationError("DIRECT HBM vectors must be 512B aligned");
                    }
                }
            }
            coverage[iocb_id].push_back({offset, offset + length});
        }

        std::set<std::string> kinds;
        std::map<std::string, std::set<std::int64_t>> ids_by_kind;
        std::vector<ByteInterval> physical_ssd_provided;
        std::map<std::pair<std::int64_t, std::int64_t>, std::vector<ByteInterval>> copy_iovs;
        std::int64_t staged_physical = 0;
        for (const Json& transfer : transfers) {
            std::int64_t id = require_int(transfer.at("transfer_id"), "transfer_id");
            if (!transfer_ids.insert(id).second) throw ValidationError("duplicate transfer_id");
            if (!transfer.contains("depends_on") || !transfer.at("depends_on").is_array()) {
                throw ValidationError("transfer.depends_on must be an array");
            }
            if (!transfer.contains("device_id")) throw ValidationError("transfer.device_id is required");
            if (require_int(transfer.at("device_id"), "transfer.device_id") != *operation_device) {
                throw ValidationError("transfer device_id does not match the operation");
            }
            std::string kind = require_string(transfer.at("type"), "transfer.type");
            static const std::set<std::string> allowed_types = {
                "DIRECT", "SSD_TO_HBM_BUFFER", "SSD_TO_DDR", "SSD_TO_MOONCAKE",
                "H2D", "D2H", "D2D",
            };
            if (!allowed_types.count(kind)) throw ValidationError("unsupported transfer type");
            kinds.insert(kind);
            ids_by_kind[kind].insert(id);
            indegree[id] = 0;
            if (is_ssd_type(kind)) {
                auto physical_offset = require_int(transfer.at("physical_ssd_offset"), "physical_ssd_offset");
                auto physical_length = require_int(transfer.at("physical_length"), "physical_length", true);
                if (physical_offset % 512 != 0 || physical_length % 512 != 0) {
                    throw ValidationError("physical SSD range must be 512B aligned");
                }
                if (physical_offset > std::numeric_limits<std::int64_t>::max() - physical_length) {
                    throw ValidationError("physical SSD range overflows");
                }
                physical_ssd_provided.push_back({physical_offset, physical_offset + physical_length});
                staged_physical += physical_length;
            } else {
                bool has_bytes = transfer.contains("bytes");
                bool has_vectors = transfer.contains("vectors") && transfer.at("vectors").is_array();
                if (!has_bytes && !has_vectors) throw ValidationError("copy needs bytes or vectors");
                std::int64_t bytes = has_bytes ? require_int(transfer.at("bytes"), "bytes", true) : 0;
                if (has_vectors) {
                    std::int64_t vector_bytes = 0;
                    const auto& vectors = transfer.at("vectors").as_array();
                    if (vectors.empty()) throw ValidationError("vectors must not be empty");
                    for (const Json& vector : vectors) {
                        const Iocb& iocb = find_iocb(submit, require_int(vector.at("iocb_id"), "vector.iocb_id"));
                        std::int64_t iov_id = require_int(vector.at("iov_id"), "vector.iov_id");
                        auto iov_it = std::find_if(iocb.iovs.begin(), iocb.iovs.end(),
                            [&](const Iov& item) { return item.iov_id == iov_id; });
                        if (iov_it == iocb.iovs.end()) throw ValidationError("unknown vector iov");
                        auto offset = require_int(vector.at("iov_offset"), "vector.iov_offset");
                        auto length = require_int(vector.at("length"), "vector.length", true);
                        if (offset + length > iov_it->length) throw ValidationError("vector exceeds iov");
                        copy_iovs[{iocb.iocb_id, iov_id}].push_back({offset, offset + length});
                        vector_bytes += length;
                    }
                    if (has_bytes && bytes != vector_bytes) throw ValidationError("bytes != vector sum");
                }
            }
        }

        const std::set<std::string> required = path == "DIRECT"
            ? std::set<std::string>{"DIRECT"}
            : path == "AGGREGATE"
                ? std::set<std::string>{"SSD_TO_HBM_BUFFER", "D2D"}
                : std::set<std::string>{"SSD_TO_DDR", "H2D"};
        if (kinds != required) throw ValidationError("path has incorrect transfer stages");
        if (path != "DIRECT") {
            std::string first = path == "AGGREGATE" ? "SSD_TO_HBM_BUFFER" : "SSD_TO_DDR";
            std::string second = path == "AGGREGATE" ? "D2D" : "H2D";
            for (const Json& transfer : transfers) {
                if (require_string(transfer.at("type"), "type") != second) continue;
                bool depends = false;
                for (const Json& raw_dep : transfer.at("depends_on").as_array()) {
                    if (ids_by_kind[first].count(require_int(raw_dep, "dependency"))) depends = true;
                }
                if (!depends) throw ValidationError(second + " must depend on SSD stage");
            }
            auto bytes = require_int(operation.at("buffer_bytes"), "buffer_bytes", true);
            auto limit = resources.at(path == "AGGREGATE" ? "hbm_capacity" : "ddr_capacity") * MiB;
            if (bytes > limit) throw ValidationError("operation buffer exceeds pool");
            if (staged_physical > bytes) {
                throw ValidationError("buffer_bytes is smaller than staged SSD bytes");
            }
        }
        if (path == "AGGREGATE" && merge_intervals(physical_ssd_required).size() != 1) {
            throw ValidationError("AGGREGATE ranges must be contiguous on one device");
        }
        require_intervals_covered(physical_ssd_required, physical_ssd_provided,
                                  "SSD transfers do not cover the logical range");
        if (path != "DIRECT") {
            for (const auto& [key, required] : logical_iovs) {
                const auto provided = copy_iovs.find(key);
                if (provided == copy_iovs.end()) throw ValidationError("copy transfers do not cover iov");
                require_exact_cover(required, provided->second,
                                    "copy vectors do not match logical iov coverage");
            }
            for (const auto& [key, provided] : copy_iovs) {
                (void)provided;
                if (!logical_iovs.count(key)) throw ValidationError("copy vector references an uncovered iov");
            }
        }
    }

    for (const Json& operation : operations) {
        for (const Json& transfer : operation.at("transfers").as_array()) {
            auto id = transfer.at("transfer_id").as_int();
            for (const Json& raw_dep : transfer.at("depends_on").as_array()) {
                auto dep = require_int(raw_dep, "dependency");
                if (!transfer_ids.count(dep)) throw ValidationError("unknown dependency");
                edges[dep].push_back(id);
                ++indegree[id];
            }
        }
    }
    std::vector<std::int64_t> queue;
    for (const auto& [id, degree] : indegree) if (degree == 0) queue.push_back(id);
    std::size_t cursor = 0;
    while (cursor < queue.size()) {
        auto id = queue[cursor++];
        for (auto target : edges[id]) if (--indegree[target] == 0) queue.push_back(target);
    }
    if (queue.size() != transfer_ids.size()) throw ValidationError("dependency graph has a cycle");

    for (const Iocb& iocb : submit.iocbs) {
        auto intervals = coverage[iocb.iocb_id];
        std::sort(intervals.begin(), intervals.end());
        std::int64_t expected = 0;
        for (const auto& [begin, end] : intervals) {
            if (begin != expected) throw ValidationError("iocb coverage contains gap or overlap");
            expected = end;
        }
        if (expected != iocb.length) throw ValidationError("iocb is not fully covered");
    }
}

namespace {

std::vector<std::string> normalized_events(const Json& events) {
    std::vector<std::string> result;
    for (const Json& event : events.as_array()) {
        if (!event.contains("event")) continue;
        std::string kind = event.at("event").as_string();
        if (kind == "PROMOTE_START") {
            std::ostringstream line;
            line << kind << '|' << event.at("promote_id").as_int() << '|'
                 << event.at("time_us").as_int() << '|' << event.at("path").as_string();
            result.push_back(line.str());
        } else if (kind == "IOCB_COMPLETE" || kind == "PROMOTE_COMPLETE") {
            for (const Json& item : event.at("completions").as_array()) {
                std::ostringstream line;
                line << kind << '|';
                line << (item.contains("submit_id") ? item.at("submit_id").as_int()
                                                     : item.at("source_submit_id").as_int());
                line << '|';
                line << (item.contains("iocb_id") ? item.at("iocb_id").as_int()
                                                   : item.at("source_iocb_id").as_int());
                line << '|' << (item.contains("promote_id") ? item.at("promote_id").as_int() : -1);
                line << '|' << item.at("complete_us").as_int();
                line << '|' << (item.contains("cached") ? item.at("cached").as_bool() : false);
                result.push_back(line.str());
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

}  // namespace

void compare_completion_events(const Json& expected, const Json& claimed) {
    if (normalized_events(expected) != normalized_events(claimed)) {
        throw ValidationError("completion event mismatch");
    }
}

GreedyBaseline::GreedyBaseline(std::map<std::string, std::int64_t> resources,
                               bool enable_promote, bool promote_via_ssd)
    : resources_(std::move(resources)), enable_promote_(enable_promote),
      promote_via_ssd_(promote_via_ssd) {}

Json GreedyBaseline::on_submit(const Submit& submit, const Json& runtime_state) {
    Json operations = Json::array();
    std::int64_t now = runtime_state.at("current_time_us").as_int();
    for (const Iocb& iocb : submit.iocbs) {
        ++seen_[iocb.cache_key()];
        std::int64_t cursor = 0;
        for (const Iov& iov : iocb.iovs) {
            std::int64_t ssd_start = iocb.ssd_offset + cursor;
            bool aligned = ssd_start % 512 == 0 && iov.hbm_addr % 512 == 0 && iov.length % 512 == 0;
            auto [physical_offset, physical_length] = physical_span(ssd_start, iov.length);
            long double amplification = static_cast<long double>(physical_length) / iov.length;
            std::string path = aligned ? "DIRECT"
                : amplification <= 1.5L && physical_length <= resources_.at("hbm_capacity") * MiB
                    ? "AGGREGATE" : "FALLBACK";

            std::int64_t operation_id = next_operation_++;
            std::int64_t first_id = next_transfer_++;
            std::int64_t group = next_group_++;
            Json ranges = array({object({{"iocb_id", iocb.iocb_id},
                                         {"iocb_offset", cursor}, {"length", iov.length}})});
            Json transfers = Json::array();
            if (path == "DIRECT") {
                transfers.push_back(object({
                    {"transfer_id", first_id}, {"type", "DIRECT"}, {"device_id", iocb.device_id},
                    {"physical_ssd_offset", ssd_start}, {"physical_length", iov.length},
                    {"group_id", group}, {"not_before_us", now}, {"depends_on", Json::array()},
                    {"priority", 100},
                }));
                operations.push_back(object({{"operation_id", operation_id}, {"path", path},
                                             {"ranges", ranges}, {"transfers", transfers}}));
            } else {
                std::int64_t second_id = next_transfer_++;
                std::string first_type = path == "AGGREGATE" ? "SSD_TO_HBM_BUFFER" : "SSD_TO_DDR";
                std::string second_type = path == "AGGREGATE" ? "D2D" : "H2D";
                transfers.push_back(object({
                    {"transfer_id", first_id}, {"type", first_type}, {"device_id", iocb.device_id},
                    {"physical_ssd_offset", physical_offset}, {"physical_length", physical_length},
                    {"group_id", group}, {"not_before_us", now}, {"depends_on", Json::array()},
                    {"priority", 100},
                }));
                Json vectors = array({object({
                    {"iocb_id", iocb.iocb_id}, {"iov_id", iov.iov_id},
                    {"iov_offset", 0}, {"length", iov.length},
                })});
                transfers.push_back(object({
                    {"transfer_id", second_id}, {"type", second_type}, {"device_id", iocb.device_id},
                    {"bytes", iov.length}, {"vectors", vectors}, {"group_id", group},
                    {"not_before_us", now}, {"depends_on", array({Json(first_id)})}, {"priority", 100},
                }));
                operations.push_back(object({
                    {"operation_id", operation_id}, {"path", path}, {"ranges", ranges},
                    {"buffer_bytes", physical_length}, {"transfers", transfers},
                }));
            }
            cursor += iov.length;
        }
    }
    return object({{"event", "SCHEDULE_DECISION"}, {"submit_id", submit.submit_id},
                   {"operations", operations}, {"next_wakeup_us", Json(nullptr)}});
}

std::optional<Json> GreedyBaseline::on_iocb_complete(const Iocb& iocb,
                                                      const Json& runtime_state) {
    (void)runtime_state;
    auto key = iocb.cache_key();
    if (!enable_promote_ || seen_[key] < 2 || promoted_[key]) return std::nullopt;
    promoted_[key] = true;
    std::int64_t promote_id = next_promote_++;
    std::int64_t transfer_id = next_transfer_++;
    Json transfer = object({
        {"transfer_id", transfer_id}, {"device_id", iocb.device_id}, {"priority", 10},
    });
    std::string path = "PROMOTE_HBM";
    if (promote_via_ssd_) {
        auto [offset, length] = physical_span(iocb.ssd_offset, iocb.length);
        path = "PROMOTE_SSD";
        transfer["type"] = "SSD_TO_MOONCAKE";
        transfer["physical_ssd_offset"] = offset;
        transfer["physical_length"] = length;
    } else {
        transfer["type"] = "D2H";
        transfer["bytes"] = iocb.length;
        transfer["vectors"] = static_cast<std::int64_t>(iocb.iovs.size());
    }
    return object({
        {"promote_id", promote_id}, {"source_submit_id", iocb.submit_id},
        {"source_iocb_id", iocb.iocb_id}, {"path", path}, {"transfer", transfer},
    });
}

ReplayPolicy::ReplayPolicy(const Json& solution) {
    for (const Json& decision : solution.at("schedule_decisions").as_array()) {
        decisions_[decision.at("submit_id").as_int()] = decision;
    }
    for (const Json& promotion : solution.at("promotion_decisions").as_array()) {
        promotions_[{promotion.at("source_submit_id").as_int(),
                     promotion.at("source_iocb_id").as_int()}] = promotion;
    }
}

Json ReplayPolicy::on_submit(const Submit& submit, const Json& runtime_state) {
    (void)runtime_state;
    auto it = decisions_.find(submit.submit_id);
    if (it == decisions_.end()) throw ValidationError("missing decision for submit");
    return it->second;
}

std::optional<Json> ReplayPolicy::on_iocb_complete(const Iocb& iocb,
                                                    const Json& runtime_state) {
    (void)runtime_state;
    auto it = promotions_.find({iocb.submit_id, iocb.iocb_id});
    if (it == promotions_.end()) return std::nullopt;
    return it->second;
}

namespace {

class Engine {
public:
    Engine(const Trace& trace, SchedulerPolicy& policy)
        : trace_(trace), policy_(policy), resources_(trace.instance.resources),
          costs_(trace.instance.fixed_parameters), time_(trace.events.front().time_us),
          first_arrival_(trace.events.front().time_us) {
        for (const auto& event : trace.events) {
            for (const auto& iocb : event.submit.iocbs) valid_bytes_ += iocb.length;
        }
    }

    Json run();

private:
    using IocbKey = std::pair<std::int64_t, std::int64_t>;
    using OperationKey = std::pair<std::int64_t, std::int64_t>;

    struct Task {
        std::int64_t transfer_id{};
        std::string kind;
        std::int64_t device_id{};
        std::int64_t bytes_total{};
        long double remaining{};
        long double not_before{};
        std::set<std::int64_t> dependencies;
        int unresolved_deps{0};
        int priority{};
        std::optional<std::int64_t> group_id;
        std::optional<OperationKey> operation_key;
        std::vector<IocbKey> iocb_keys;
        std::optional<std::int64_t> promotion_id;
        bool started{false};
        bool completed{false};
        long double actual_start{};
        int queue{0};
        bool future_tracked{false};
        std::int64_t buffer_need{0};
        std::size_t live_index{0};
    };

    struct OperationRuntime {
        OperationKey key;
        std::string path;
        std::set<std::int64_t> transfer_ids;
        std::set<IocbKey> iocb_keys;
        std::string buffer_kind;
        std::int64_t buffer_bytes{};
        bool buffer_allocated{false};
        bool completed{false};
    };

    struct IocbRuntime {
        Iocb iocb;
        std::set<OperationKey> operation_keys;
        std::optional<std::int64_t> cache_hit_task;
        bool requires_iocb_slot{false};
        bool slot_acquired{false};
        bool completed{false};
        std::int64_t arrival_us{0};
        std::vector<std::int64_t> task_ids;
    };

    struct PromotionRuntime {
        std::int64_t promote_id{};
        IocbKey source_key;
        std::string path;
        std::int64_t transfer_id{};
        bool started{false};
        bool completed{false};
    };

    struct CacheEntry {
        Iocb::CacheKey key;
        std::int64_t size{};
        long double last_access{};
        long double expires_at{};
    };

    const Trace& trace_;
    SchedulerPolicy& policy_;
    const std::map<std::string, std::int64_t>& resources_;
    const std::map<std::string, std::int64_t>& costs_;
    long double time_{};
    std::int64_t first_arrival_{};
    std::size_t arrival_index_{0};
    static constexpr int Q_NONE = 0;
    static constexpr int Q_SLOT = 2;
    static constexpr int Q_HBM = 3;
    static constexpr int Q_DDR = 4;
    static constexpr int Q_COPY = 5;
    static constexpr int Q_PLAIN = 6;

    struct ReadyKey {
        int priority{0};
        std::int64_t id{0};
        bool operator<(const ReadyKey& other) const {
            if (priority != other.priority) return priority > other.priority;
            return id < other.id;
        }
    };

    std::map<std::int64_t, Task> tasks_;
    std::set<ReadyKey> q_slot_;
    std::map<std::int64_t, std::set<ReadyKey>> q_hbm_;
    std::map<std::int64_t, std::set<ReadyKey>> q_ddr_;
    std::set<ReadyKey> q_plain_;
    std::map<std::pair<std::string, std::int64_t>, std::set<ReadyKey>> q_copy_;
    std::multiset<std::pair<long double, std::int64_t>> future_;
    std::map<std::int64_t, std::vector<std::int64_t>> dependents_;
    std::int64_t incomplete_{0};
    std::vector<Task*> active_;
    std::unordered_map<std::int64_t, std::size_t> active_index_;
    std::map<std::pair<std::string, std::int64_t>, std::int64_t> copy_inflight_;
    std::set<std::int64_t> completed_transfers_;
    std::map<OperationKey, OperationRuntime> operations_;
    std::map<IocbKey, IocbRuntime> iocbs_;
    std::map<std::int64_t, PromotionRuntime> promotions_;
    std::map<Iocb::CacheKey, CacheEntry> cache_;
    std::int64_t hbm_used_{0};
    std::int64_t ddr_used_{0};
    std::int64_t iocb_slots_used_{0};
    std::int64_t internal_transfer_id_{-1};
    Json schedule_decisions_{Json::array()};
    Json promotion_decisions_{Json::array()};
    Json events_{Json::array()};
    std::int64_t cpu_cost_{0};
    std::int64_t npu_cost_{0};
    std::int64_t valid_bytes_{0};
    std::int64_t physical_ssd_bytes_{0};
    std::int64_t cache_hits_{0};
    std::int64_t promote_count_{0};
    std::int64_t promote_success_{0};
    std::int64_t evictions_{0};
    std::vector<std::int64_t> latencies_;
    Json live_;
    bool live_ready_{false};
    std::map<std::int64_t, std::size_t> transfer_pos_;
    std::map<IocbKey, std::size_t> queued_pos_;
    std::map<Iocb::CacheKey, std::size_t> cache_pos_;
    std::map<std::int64_t, std::size_t> promote_pos_;
    std::int64_t kvcache_used_{0};

    std::int64_t cost(const std::string& name) const;
    const Json& runtime_state();
    void ensure_live();
    void note_transfer_started(Task& task);
    void note_transfer_finished(std::int64_t id);
    void note_queued(const IocbKey& key);
    void note_unqueued(const IocbKey& key);
    void note_cache_added(const Iocb::CacheKey& key);
    void note_cache_removed(const Iocb::CacheKey& key);
    void note_promote_added(const PromotionRuntime& promotion);
    void note_promote_removed(std::int64_t promote_id);
    std::int64_t task_size(const Json& transfer) const;
    void add_operation_cost(const std::string& path, const Json::Array& transfers);
    void register_decision(const Submit& submit, const Json& decision);
    void expire_cache();
    void register_cache_hit(const Iocb& iocb, std::int64_t arrival_us);
    void handle_arrival(std::size_t index);
    void register_promotion(const Iocb& iocb, const Json& decision);
    bool resource_available(const Task& task) const;
    bool can_reserve_operation(const OperationRuntime& operation) const;
    bool can_acquire_iocb_slots(const Task& task) const;
    bool needs_new_slot(const Task& task) const;
    bool slots_exhausted() const;
    std::int64_t pool_free(const std::string& kind) const;
    void note_task(std::int64_t id);
    void consider_ready(std::int64_t id);
    void classify_enqueue(Task& task);
    void take_out(Task& task);
    void launch_task(Task& task);
    void promote_future();
    void start_ready_tasks();
    struct RatedTask {
        Task* task;
        long double rate;
    };
    std::vector<RatedTask> rates();
    void sync_remaining();
    void release_operation(OperationRuntime& operation);
    bool cache_insert(const Iocb& iocb);
    void complete_iocb(IocbRuntime& state);
    void finish_task(Task& task);
    std::optional<long double> next_pending_time() const;
    void advance();
};

std::int64_t Engine::cost(const std::string& name) const {
    auto it = costs_.find(name);
    return it == costs_.end() ? 0 : it->second;
}

void Engine::ensure_live() {
    if (live_ready_) return;
    live_ = object({
        {"current_time_us", ceil_us(time_)}, {"foreground_iocb_inflight", iocb_slots_used_},
        {"transfers_inflight", Json::array()}, {"aggregate_hbm_used", hbm_used_},
        {"fallback_ddr_used", ddr_used_}, {"kvcache_pool_used", kvcache_used_},
        {"cached_ranges", Json::array()}, {"promote_inflight", Json::array()},
        {"queued_iocbs", Json::array()},
    });
    live_ready_ = true;
}

const Json& Engine::runtime_state() {
    ensure_live();
    live_["current_time_us"] = ceil_us(time_);
    live_["foreground_iocb_inflight"] = iocb_slots_used_;
    live_["aggregate_hbm_used"] = hbm_used_;
    live_["fallback_ddr_used"] = ddr_used_;
    live_["kvcache_pool_used"] = kvcache_used_;
    return live_;
}

void Engine::note_transfer_started(Task& task) {
    ensure_live();
    Json item = object({
        {"transfer_id", task.transfer_id}, {"group_id", task.group_id ? Json(*task.group_id) : Json(nullptr)},
        {"type", task.kind}, {"device_id", task.device_id}, {"size", task.bytes_total},
        {"start_us", ceil_us(task.actual_start)}, {"remaining_bytes", ceil_us(task.remaining)},
    });
    auto& inflight = live_.at("transfers_inflight").as_array();
    task.live_index = inflight.size();
    transfer_pos_[task.transfer_id] = inflight.size();
    inflight.push_back(std::move(item));
}

void Engine::note_transfer_finished(std::int64_t id) {
    auto found = transfer_pos_.find(id);
    if (found == transfer_pos_.end() || !live_ready_) return;
    auto& inflight = live_.at("transfers_inflight").as_array();
    const std::size_t index = found->second;
    transfer_pos_.erase(found);
    if (index + 1 != inflight.size()) {
        inflight[index] = std::move(inflight.back());
        const std::int64_t moved = inflight[index].at("transfer_id").as_int();
        transfer_pos_[moved] = index;
        tasks_.at(moved).live_index = index;
    }
    inflight.pop_back();
}

void Engine::note_queued(const IocbKey& key) {
    if (queued_pos_.count(key)) return;
    ensure_live();
    auto& queued = live_.at("queued_iocbs").as_array();
    queued_pos_[key] = queued.size();
    queued.push_back(object({{"submit_id", key.first}, {"iocb_id", key.second}}));
}

void Engine::note_unqueued(const IocbKey& key) {
    auto found = queued_pos_.find(key);
    if (found == queued_pos_.end() || !live_ready_) return;
    auto& queued = live_.at("queued_iocbs").as_array();
    const std::size_t index = found->second;
    queued_pos_.erase(found);
    if (index + 1 != queued.size()) {
        queued[index] = std::move(queued.back());
        queued_pos_[{queued[index].at("submit_id").as_int(), queued[index].at("iocb_id").as_int()}] = index;
    }
    queued.pop_back();
}

void Engine::note_cache_added(const Iocb::CacheKey& key) {
    ensure_live();
    auto& cached = live_.at("cached_ranges").as_array();
    cache_pos_[key] = cached.size();
    cached.push_back(array({Json(std::get<0>(key)), Json(std::get<1>(key)), Json(std::get<2>(key))}));
}

void Engine::note_cache_removed(const Iocb::CacheKey& key) {
    auto found = cache_pos_.find(key);
    if (found == cache_pos_.end() || !live_ready_) return;
    auto& cached = live_.at("cached_ranges").as_array();
    const std::size_t index = found->second;
    cache_pos_.erase(found);
    if (index + 1 != cached.size()) {
        cached[index] = std::move(cached.back());
        const Json& item = cached[index];
        cache_pos_[{item[0].as_int(), item[1].as_int(), item[2].as_int()}] = index;
    }
    cached.pop_back();
}

void Engine::note_promote_added(const PromotionRuntime& promotion) {
    ensure_live();
    auto& inflight = live_.at("promote_inflight").as_array();
    promote_pos_[promotion.promote_id] = inflight.size();
    inflight.push_back(object({
        {"promote_id", promotion.promote_id}, {"path", promotion.path},
        {"source_submit_id", promotion.source_key.first},
        {"source_iocb_id", promotion.source_key.second},
    }));
}

void Engine::note_promote_removed(std::int64_t promote_id) {
    auto found = promote_pos_.find(promote_id);
    if (found == promote_pos_.end() || !live_ready_) return;
    auto& inflight = live_.at("promote_inflight").as_array();
    const std::size_t index = found->second;
    promote_pos_.erase(found);
    if (index + 1 != inflight.size()) {
        inflight[index] = std::move(inflight.back());
        promote_pos_[inflight[index].at("promote_id").as_int()] = index;
    }
    inflight.pop_back();
}

std::int64_t Engine::task_size(const Json& transfer) const {
    std::string kind = transfer.at("type").as_string();
    if (is_ssd_type(kind)) return require_int(transfer.at("physical_length"), "physical_length", true);
    if (transfer.contains("bytes")) return require_int(transfer.at("bytes"), "bytes", true);
    if (transfer.contains("vectors") && transfer.at("vectors").is_array()) {
        std::int64_t total = 0;
        for (const Json& item : transfer.at("vectors").as_array()) total += item.at("length").as_int();
        if (total > 0) return total;
    }
    throw ValidationError("copy transfer has invalid byte count");
}

void Engine::add_operation_cost(const std::string& path, const Json::Array& transfers) {
    int commands = 0, h2d = 0, d2h = 0, d2d = 0;
    for (const Json& transfer : transfers) {
        std::string kind = transfer.at("type").as_string();
        commands += is_ssd_type(kind);
        h2d += kind == "H2D";
        d2h += kind == "D2H";
        d2d += kind == "D2D";
    }
    if (path == "DIRECT" || path == "AGGREGATE") {
        cpu_cost_ += cost("CPU_NDS_FIXED_COST") + commands * cost("CPU_NDS_PER_CMD_COST");
    } else if (path == "FALLBACK") {
        cpu_cost_ += cost("CPU_FALLBACK_FIXED_COST") + commands * cost("CPU_FALLBACK_PER_CMD_COST");
    } else {
        cpu_cost_ += cost("CPU_PROMOTE_FIXED_COST") + commands * cost("CPU_PROMOTE_PER_CMD_COST");
    }
    cpu_cost_ += h2d * cost("CPU_H2D_CALL_COST") + d2h * cost("CPU_D2H_CALL_COST")
               + d2d * cost("CPU_D2D_CALL_COST");
    npu_cost_ += h2d * cost("NPU_H2D_CALL_COST") + d2h * cost("NPU_D2H_CALL_COST")
               + d2d * cost("NPU_D2D_CALL_COST");
}

void Engine::register_decision(const Submit& submit, const Json& decision) {
    validate_decision(submit, decision, resources_);
    schedule_decisions_.push_back(decision);
    for (const Iocb& iocb : submit.iocbs) {
        IocbKey key{iocb.submit_id, iocb.iocb_id};
        IocbRuntime state{iocb, {}, std::nullopt, false, false, false, 0, {}};
        state.arrival_us = submit.arrival_us;
        iocbs_[key] = state;
    }
    for (const Json& operation : decision.at("operations").as_array()) {
        OperationKey op_key{submit.submit_id, operation.at("operation_id").as_int()};
        if (operations_.count(op_key)) throw ValidationError("duplicate operation key");
        OperationRuntime runtime;
        runtime.key = op_key;
        runtime.path = operation.at("path").as_string();
        runtime.buffer_kind = runtime.path == "AGGREGATE" ? "HBM" : runtime.path == "FALLBACK" ? "DDR" : "";
        runtime.buffer_bytes = operation.contains("buffer_bytes") ? operation.at("buffer_bytes").as_int() : 0;
        for (const Json& range : operation.at("ranges").as_array()) {
            IocbKey owner{submit.submit_id, range.at("iocb_id").as_int()};
            runtime.iocb_keys.insert(owner);
            iocbs_.at(owner).operation_keys.insert(op_key);
            if (runtime.path == "DIRECT" || runtime.path == "AGGREGATE") {
                iocbs_.at(owner).requires_iocb_slot = true;
                note_queued(owner);
            }
        }
        for (const Json& transfer : operation.at("transfers").as_array()) {
            runtime.transfer_ids.insert(transfer.at("transfer_id").as_int());
        }
        operations_[op_key] = runtime;

        for (const Json& raw : operation.at("transfers").as_array()) {
            std::int64_t id = raw.at("transfer_id").as_int();
            if (tasks_.count(id)) throw ValidationError("duplicate global transfer_id");
            Task task;
            task.transfer_id = id;
            task.kind = raw.at("type").as_string();
            task.device_id = raw.contains("device_id") ? raw.at("device_id").as_int()
                : iocbs_.at(*runtime.iocb_keys.begin()).iocb.device_id;
            task.bytes_total = task_size(raw);
            task.remaining = task.bytes_total;
            task.not_before = optional_int(raw, "not_before_us", submit.arrival_us);
            if (task.not_before < submit.arrival_us) throw ValidationError("transfer begins before arrival");
            for (const Json& dep : raw.at("depends_on").as_array()) task.dependencies.insert(dep.as_int());
            task.priority = static_cast<int>(optional_int(raw, "priority", 0));
            if (raw.contains("group_id") && !raw.at("group_id").is_null()) task.group_id = raw.at("group_id").as_int();
            task.operation_key = op_key;
            task.iocb_keys.assign(runtime.iocb_keys.begin(), runtime.iocb_keys.end());
            tasks_[id] = task;
            note_task(id);
            if (is_ssd_type(task.kind)) physical_ssd_bytes_ += task.bytes_total;
        }
        add_operation_cost(runtime.path, operation.at("transfers").as_array());
    }
}

void Engine::expire_cache() {
    std::vector<Iocb::CacheKey> expired;
    for (const auto& [key, entry] : cache_) if (entry.expires_at <= time_ + Epsilon) expired.push_back(key);
    for (const auto& key : expired) {
        kvcache_used_ -= cache_.at(key).size;
        note_cache_removed(key);
        cache_.erase(key);
        ++evictions_;
    }
}

void Engine::register_cache_hit(const Iocb& iocb, std::int64_t arrival_us) {
    IocbKey key{iocb.submit_id, iocb.iocb_id};
    std::int64_t id = internal_transfer_id_--;
    IocbRuntime state{iocb, {}, std::nullopt, false, false, false, 0, {}};
    state.cache_hit_task = id;
    state.arrival_us = arrival_us;
    iocbs_[key] = state;
    Task task;
    task.transfer_id = id;
    task.kind = "H2D";
    task.device_id = iocb.device_id;
    task.bytes_total = iocb.length;
    task.remaining = iocb.length;
    task.not_before = time_;
    task.priority = 1000000;
    task.iocb_keys.push_back(key);
    tasks_[id] = task;
    note_task(id);
    ++cache_hits_;
    cpu_cost_ += cost("CPU_H2D_CALL_COST");
    npu_cost_ += cost("NPU_H2D_CALL_COST");
    CacheEntry& entry = cache_.at(iocb.cache_key());
    entry.last_access = time_;
}

void Engine::handle_arrival(std::size_t index) {
    const TraceEvent& event = trace_.events.at(index);
    expire_cache();
    Submit visible{event.submit.submit_id, event.submit.arrival_us, {}};
    for (const Iocb& iocb : event.submit.iocbs) {
        if (cache_.count(iocb.cache_key())) register_cache_hit(iocb, event.submit.arrival_us);
        else visible.iocbs.push_back(iocb);
    }
    if (!visible.iocbs.empty()) register_decision(visible, policy_.on_submit(visible, runtime_state()));
}

void Engine::register_promotion(const Iocb& iocb, const Json& decision) {
    std::int64_t promote_id = require_int(decision.at("promote_id"), "promote_id");
    if (promotions_.count(promote_id)) throw ValidationError("duplicate promote_id");
    if (decision.at("source_submit_id").as_int() != iocb.submit_id ||
        decision.at("source_iocb_id").as_int() != iocb.iocb_id) {
        throw ValidationError("promotion source mismatch");
    }
    std::string path = decision.at("path").as_string();
    if (path != "PROMOTE_SSD" && path != "PROMOTE_HBM") throw ValidationError("invalid promotion path");
    Json raw = decision.at("transfer");
    std::string expected = path == "PROMOTE_SSD" ? "SSD_TO_MOONCAKE" : "D2H";
    if (raw.at("type").as_string() != expected) throw ValidationError("promotion transfer type mismatch");
    if (path == "PROMOTE_SSD") {
        auto [offset, length] = physical_span(iocb.ssd_offset, iocb.length);
        if (!raw.contains("physical_ssd_offset")) raw["physical_ssd_offset"] = offset;
        if (!raw.contains("physical_length")) raw["physical_length"] = length;
        if (require_int(raw.at("physical_ssd_offset"), "physical_ssd_offset") != offset ||
            require_int(raw.at("physical_length"), "physical_length", true) != length) {
            throw ValidationError("PROMOTE_SSD must cover the aligned SSD span exactly");
        }
    } else {
        if (!raw.contains("bytes")) raw["bytes"] = iocb.length;
        if (require_int(raw.at("bytes"), "bytes", true) != iocb.length) {
            throw ValidationError("PROMOTE_HBM bytes must equal iocb.length");
        }
    }
    if (raw.contains("device_id") && raw.at("device_id").as_int() != iocb.device_id) {
        throw ValidationError("promotion device_id mismatch");
    }
    std::int64_t id = require_int(raw.at("transfer_id"), "transfer_id");
    if (tasks_.count(id)) throw ValidationError("duplicate global transfer_id");
    Task task;
    task.transfer_id = id;
    task.kind = expected;
    task.device_id = iocb.device_id;
    task.bytes_total = task_size(raw);
    task.remaining = task.bytes_total;
    task.not_before = time_;
    if (decision.contains("not_before_us") && !decision.at("not_before_us").is_null()) {
        const std::int64_t not_before = require_int(decision.at("not_before_us"), "not_before_us");
        if (not_before < ceil_us(time_)) throw ValidationError("promotion not_before_us is in the past");
        task.not_before = not_before;
    }
    task.priority = static_cast<int>(optional_int(raw, "priority", 0));
    task.promotion_id = promote_id;
    tasks_[id] = task;
    note_task(id);
    promotions_[promote_id] = PromotionRuntime{promote_id, {iocb.submit_id, iocb.iocb_id}, path, id};
    note_promote_added(promotions_.at(promote_id));
    promotion_decisions_.push_back(decision);
    ++promote_count_;
    if (is_ssd_type(expected)) physical_ssd_bytes_ += task.bytes_total;
    add_operation_cost(path, Json::Array{raw});
}

bool Engine::resource_available(const Task& task) const {
    if (is_ssd_type(task.kind)) return true;
    auto it = copy_inflight_.find({task.kind, task.device_id});
    std::int64_t active_count = it == copy_inflight_.end() ? 0 : it->second;
    std::string cap = task.kind == "H2D" ? "h2d_copy_concurrency"
        : task.kind == "D2H" ? "d2h_copy_concurrency" : "hbm_copy_concurrency";
    return active_count < resources_.at(cap);
}

bool Engine::can_reserve_operation(const OperationRuntime& operation) const {
    if (operation.buffer_allocated || operation.buffer_kind.empty()) return true;
    if (operation.buffer_kind == "HBM") {
        return hbm_used_ + operation.buffer_bytes <= resources_.at("hbm_capacity") * MiB;
    }
    return ddr_used_ + operation.buffer_bytes <= resources_.at("ddr_capacity") * MiB;
}

bool Engine::can_acquire_iocb_slots(const Task& task) const {
    std::int64_t needed = 0;
    for (const auto& key : task.iocb_keys) {
        const auto& state = iocbs_.at(key);
        if (state.requires_iocb_slot && !state.slot_acquired) ++needed;
    }
    return iocb_slots_used_ + needed <= resources_.at("iocb_capacity");
}

bool Engine::needs_new_slot(const Task& task) const {
    for (const auto& key : task.iocb_keys) {
        const IocbRuntime& state = iocbs_.at(key);
        if (state.requires_iocb_slot && !state.slot_acquired) return true;
    }
    return false;
}

bool Engine::slots_exhausted() const {
    return iocb_slots_used_ >= resources_.at("iocb_capacity");
}

std::int64_t Engine::pool_free(const std::string& kind) const {
    if (kind == "HBM") return resources_.at("hbm_capacity") * MiB - hbm_used_;
    if (kind == "DDR") return resources_.at("ddr_capacity") * MiB - ddr_used_;
    return 0;
}

void Engine::note_task(std::int64_t id) {
    Task& task = tasks_.at(id);
    task.unresolved_deps = static_cast<int>(task.dependencies.size());
    for (std::int64_t dep : task.dependencies) dependents_[dep].push_back(id);
    for (const auto& key : task.iocb_keys) iocbs_.at(key).task_ids.push_back(id);
    ++incomplete_;
    consider_ready(id);
}

void Engine::consider_ready(std::int64_t id) {
    Task& task = tasks_.at(id);
    if (task.started || task.completed || task.queue != Q_NONE) return;
    if (task.not_before > time_ + Epsilon) {
        if (!task.future_tracked) {
            future_.insert({task.not_before, id});
            task.future_tracked = true;
        }
        return;
    }
    if (task.unresolved_deps > 0) return;
    classify_enqueue(task);
}

void Engine::classify_enqueue(Task& task) {
    ReadyKey key{task.priority, task.transfer_id};
    if (needs_new_slot(task)) {
        q_slot_.insert(key);
        task.queue = Q_SLOT;
        return;
    }
    OperationRuntime* operation = task.operation_key ? &operations_.at(*task.operation_key) : nullptr;
    if (operation && !operation->buffer_allocated && operation->buffer_kind == "HBM") {
        q_hbm_[operation->buffer_bytes].insert(key);
        task.buffer_need = operation->buffer_bytes;
        task.queue = Q_HBM;
        return;
    }
    if (operation && !operation->buffer_allocated && operation->buffer_kind == "DDR") {
        q_ddr_[operation->buffer_bytes].insert(key);
        task.buffer_need = operation->buffer_bytes;
        task.queue = Q_DDR;
        return;
    }
    if (!is_ssd_type(task.kind)) {
        q_copy_[{task.kind, task.device_id}].insert(key);
        task.queue = Q_COPY;
        return;
    }
    q_plain_.insert(key);
    task.queue = Q_PLAIN;
}

void Engine::take_out(Task& task) {
    ReadyKey key{task.priority, task.transfer_id};
    if (task.queue == Q_SLOT) q_slot_.erase(key);
    else if (task.queue == Q_HBM || task.queue == Q_DDR) {
        auto& buckets = task.queue == Q_HBM ? q_hbm_ : q_ddr_;
        auto bucket = buckets.find(task.buffer_need);
        if (bucket != buckets.end()) {
            bucket->second.erase(key);
            if (bucket->second.empty()) buckets.erase(bucket);
        }
    } else if (task.queue == Q_COPY) {
        q_copy_[{task.kind, task.device_id}].erase(key);
    } else if (task.queue == Q_PLAIN) {
        q_plain_.erase(key);
    }
    task.queue = Q_NONE;
}

void Engine::launch_task(Task& task) {
    OperationRuntime* operation = task.operation_key ? &operations_.at(*task.operation_key) : nullptr;
    if (operation && !operation->buffer_allocated && !operation->buffer_kind.empty()) {
        operation->buffer_allocated = true;
        if (operation->buffer_kind == "HBM") hbm_used_ += operation->buffer_bytes;
        else ddr_used_ += operation->buffer_bytes;
    }
    for (const auto& key : task.iocb_keys) {
        IocbRuntime& state = iocbs_.at(key);
                if (state.requires_iocb_slot && !state.slot_acquired) {
                    state.slot_acquired = true;
                    ++iocb_slots_used_;
                    note_unqueued(key);
                    for (std::int64_t other_id : state.task_ids) {
                Task& other = tasks_.at(other_id);
                if (other.queue == Q_SLOT && !needs_new_slot(other)) {
                    take_out(other);
                    classify_enqueue(other);
                }
            }
        }
    }
    task.started = true;
    task.actual_start = time_;
    active_index_[task.transfer_id] = active_.size();
    active_.push_back(&task);
    if (!is_ssd_type(task.kind)) ++copy_inflight_[{task.kind, task.device_id}];
    if (task.promotion_id) {
        auto& promotion = promotions_.at(*task.promotion_id);
        if (!promotion.started) {
            promotion.started = true;
            events_.push_back(object({
                {"event", "PROMOTE_START"}, {"time_us", ceil_us(time_)},
                {"promote_id", promotion.promote_id},
                {"source_submit_id", promotion.source_key.first},
                {"source_iocb_id", promotion.source_key.second}, {"path", promotion.path},
            }));
        }
    }
    note_transfer_started(task);
}

void Engine::promote_future() {
    while (!future_.empty() && future_.begin()->first <= time_ + Epsilon) {
        std::int64_t id = future_.begin()->second;
        future_.erase(future_.begin());
        Task& task = tasks_.at(id);
        task.future_tracked = false;
        if (!task.started && !task.completed && task.unresolved_deps == 0) classify_enqueue(task);
    }
}

void Engine::start_ready_tasks() {
    promote_future();
    while (true) {
        bool started = false;
        std::vector<std::int64_t> deferred;
        bool ignore_slot = false;
        std::set<std::pair<std::string, std::int64_t>> ignore_copy;
        while (true) {
            struct Candidate {
                ReadyKey key;
                int queue{0};
                std::pair<std::string, std::int64_t> copy_key;
            };
            std::optional<Candidate> best;
            auto offer = [&](const std::set<ReadyKey>& queue, int kind,
                             const std::pair<std::string, std::int64_t>& copy_key) {
                if (queue.empty()) return;
                Candidate candidate{*queue.begin(), kind, copy_key};
                if (!best || candidate.key < best->key) best = candidate;
            };
            auto offer_key = [&](const ReadyKey& key, int kind) {
                Candidate candidate{key, kind, {}};
                if (!best || candidate.key < best->key) best = candidate;
            };
            auto offer_fitting = [&](const std::map<std::int64_t, std::set<ReadyKey>>& buckets,
                                     std::int64_t free, int kind) {
                std::optional<ReadyKey> choice;
                for (const auto& [size, queue] : buckets) {
                    if (size > free) break;
                    if (queue.empty()) continue;
                    if (!choice || *queue.begin() < *choice) choice = *queue.begin();
                }
                if (choice) offer_key(*choice, kind);
            };
            if (!ignore_slot) offer(q_slot_, Q_SLOT, {});
            offer_fitting(q_hbm_, pool_free("HBM"), Q_HBM);
            offer_fitting(q_ddr_, pool_free("DDR"), Q_DDR);
            offer(q_plain_, Q_PLAIN, {});
            for (const auto& [copy_key, queue] : q_copy_) {
                if (!ignore_copy.count(copy_key)) offer(queue, Q_COPY, copy_key);
            }
            if (!best) break;
            Task& task = tasks_.at(best->key.id);
            OperationRuntime* operation = task.operation_key ? &operations_.at(*task.operation_key) : nullptr;
            const bool copy_blocked = best->queue == Q_COPY && !resource_available(task);
            const bool can_start = !copy_blocked && resource_available(task) &&
                (operation == nullptr || can_reserve_operation(*operation)) &&
                can_acquire_iocb_slots(task);
            if (!can_start) {
                if (best->queue == Q_SLOT && slots_exhausted()) {
                    ignore_slot = true;
                    continue;
                }
                if (copy_blocked) {
                    ignore_copy.insert(best->copy_key);
                    continue;
                }
                take_out(task);
                deferred.push_back(task.transfer_id);
                continue;
            }
            take_out(task);
            launch_task(task);
            started = true;
        }
        for (std::int64_t id : deferred) classify_enqueue(tasks_.at(id));
        if (!started) break;
    }
}

std::vector<Engine::RatedTask> Engine::rates() {
    using ResourceKey = std::pair<std::string, std::int64_t>;
    std::map<ResourceKey, std::vector<Task*>> groups;
    for (Task* task : active_) {
        ResourceKey key = is_ssd_type(task->kind) ? ResourceKey{"SSD", 0}
                                                   : ResourceKey{task->kind, task->device_id};
        groups[key].push_back(task);
    }
    std::vector<RatedTask> result;
    result.reserve(active_.size());
    for (const auto& [key, tasks] : groups) {
        int q = static_cast<int>(tasks.size());
        if (key.first == "SSD") {
            for (Task* task : tasks) {
                const Profile& profile = (task->kind == "DIRECT" || task->kind == "SSD_TO_HBM_BUFFER")
                    ? DirectProfile : SsdToDdrProfile;
                long double rate = percent_rate(resources_.at("nvme_bw"),
                    bucket_value(profile, task->bytes_total, q), q);
                if (rate <= 0) throw std::runtime_error("non-positive rate");
                result.push_back({task, rate});
            }
        } else if (key.first == "H2D" || key.first == "D2H") {
            std::int64_t base = resources_.at(key.first == "H2D" ? "h2d_bw" : "d2h_bw");
            for (Task* task : tasks) {
                long double rate = percent_rate(base, bucket_value(H2dD2hProfile, task->bytes_total, q), q);
                if (rate <= 0) throw std::runtime_error("non-positive rate");
                result.push_back({task, rate});
            }
        } else {
            std::vector<std::pair<Task*, long double>> raw;
            long double total = 0;
            for (Task* task : tasks) {
                auto size_it = floor_iterator(D2dProfile, task->bytes_total);
                long double group_mb_s = size_it->first == 512 ? 20.0L * q
                    : size_it->first == 4 * KiB ? 130.0L * q
                    : bucket_value(D2dProfile, task->bytes_total, q);
                long double rate = bytes_per_us(static_cast<std::int64_t>(group_mb_s)) / q;
                raw.push_back({task, rate});
                total += rate;
            }
            long double scale = total > 0 ? std::min(1.0L, bytes_per_us(resources_.at("d2d_bw")) / total) : 1.0L;
            for (const auto& [task, rate] : raw) {
                long double scaled = rate * scale;
                if (scaled <= 0) throw std::runtime_error("non-positive rate");
                result.push_back({task, scaled});
            }
        }
    }
    return result;
}

void Engine::sync_remaining() {
    if (!live_ready_) return;
    auto& inflight = live_.at("transfers_inflight").as_array();
    for (Task* task : active_) {
        inflight[task->live_index]["remaining_bytes"] = ceil_us(task->remaining);
    }
}

void Engine::release_operation(OperationRuntime& operation) {
    if (!operation.buffer_allocated) return;
    if (operation.buffer_kind == "HBM") hbm_used_ -= operation.buffer_bytes;
    else if (operation.buffer_kind == "DDR") ddr_used_ -= operation.buffer_bytes;
    operation.buffer_allocated = false;
}

bool Engine::cache_insert(const Iocb& iocb) {
    std::int64_t capacity = resources_.at("kvcache_pool_capacity") * MiB;
    if (iocb.length > capacity) return false;
    expire_cache();
    const auto key = iocb.cache_key();
    while (!cache_.empty() && kvcache_used_ + iocb.length > capacity) {
        auto victim = std::min_element(cache_.begin(), cache_.end(),
            [](const auto& a, const auto& b) {
                return a.second.last_access != b.second.last_access
                    ? a.second.last_access < b.second.last_access : a.first < b.first;
            });
        kvcache_used_ -= victim->second.size;
        note_cache_removed(victim->first);
        cache_.erase(victim);
        ++evictions_;
    }
    if (auto existing = cache_.find(key); existing != cache_.end()) {
        kvcache_used_ -= existing->second.size;
        note_cache_removed(key);
        cache_.erase(existing);
    }
    const std::int64_t inserted_at = ceil_us(time_);
    cache_[key] = CacheEntry{
        key, iocb.length, time_,
        static_cast<long double>(inserted_at) + static_cast<long double>(cost("promote_ttl_us"))};
    kvcache_used_ += iocb.length;
    note_cache_added(key);
    return true;
}

void Engine::complete_iocb(IocbRuntime& state) {
    if (state.completed) return;
    state.completed = true;
    if (state.slot_acquired) { state.slot_acquired = false; --iocb_slots_used_; }
    std::int64_t complete = std::max(state.arrival_us, ceil_us(time_));
    latencies_.push_back(complete - state.arrival_us);
    expire_cache();
    events_.push_back(object({
        {"event", "IOCB_COMPLETE"}, {"time_us", complete},
        {"completions", array({object({{"submit_id", state.iocb.submit_id},
                                        {"iocb_id", state.iocb.iocb_id},
                                        {"complete_us", complete}})})},
    }));
    note_unqueued({state.iocb.submit_id, state.iocb.iocb_id});
    auto promotion = policy_.on_iocb_complete(state.iocb, runtime_state());
    if (promotion) register_promotion(state.iocb, *promotion);
}

void Engine::finish_task(Task& task) {
    task.remaining = 0;
    task.completed = true;
    if (auto found = active_index_.find(task.transfer_id); found != active_index_.end()) {
        const std::size_t index = found->second;
        active_index_.erase(found);
        if (index + 1 != active_.size()) {
            active_[index] = active_.back();
            active_index_[active_[index]->transfer_id] = index;
        }
        active_.pop_back();
    }
    note_transfer_finished(task.transfer_id);
    if (!is_ssd_type(task.kind)) {
        auto key = std::make_pair(task.kind, task.device_id);
        auto it = copy_inflight_.find(key);
        if (it == copy_inflight_.end() || it->second <= 0) {
            throw std::runtime_error("copy inflight accounting underflow");
        }
        if (--it->second == 0) copy_inflight_.erase(it);
    }
    completed_transfers_.insert(task.transfer_id);
    --incomplete_;
    if (auto waiting = dependents_.find(task.transfer_id); waiting != dependents_.end()) {
        for (std::int64_t child : waiting->second) {
            Task& dependent = tasks_.at(child);
            if (dependent.unresolved_deps > 0) --dependent.unresolved_deps;
            if (dependent.unresolved_deps == 0) consider_ready(child);
        }
        dependents_.erase(waiting);
    }
    if (task.operation_key) {
        OperationRuntime& operation = operations_.at(*task.operation_key);
        bool all_done = std::all_of(operation.transfer_ids.begin(), operation.transfer_ids.end(),
            [&](std::int64_t id) { return completed_transfers_.count(id) != 0; });
        if (all_done) {
            operation.completed = true;
            release_operation(operation);
            for (const auto& key : operation.iocb_keys) {
                IocbRuntime& state = iocbs_.at(key);
                bool iocb_done = std::all_of(state.operation_keys.begin(), state.operation_keys.end(),
                    [&](const OperationKey& op) { return operations_.at(op).completed; });
                if (iocb_done) complete_iocb(state);
            }
        }
    } else if (task.promotion_id) {
        PromotionRuntime& promotion = promotions_.at(*task.promotion_id);
        promotion.completed = true;
        note_promote_removed(promotion.promote_id);
        IocbRuntime& source = iocbs_.at(promotion.source_key);
        bool cached = cache_insert(source.iocb);
        if (cached) ++promote_success_;
        std::int64_t complete = ceil_us(time_);
        events_.push_back(object({
            {"event", "PROMOTE_COMPLETE"}, {"time_us", complete},
            {"completions", array({object({
                {"promote_id", promotion.promote_id},
                {"source_submit_id", promotion.source_key.first},
                {"source_iocb_id", promotion.source_key.second}, {"path", promotion.path},
                {"complete_us", complete}, {"cached", cached},
            })})},
        }));
    } else {
        for (const auto& key : task.iocb_keys) complete_iocb(iocbs_.at(key));
    }
}

std::optional<long double> Engine::next_pending_time() const {
    if (future_.empty()) return std::nullopt;
    return future_.begin()->first;
}

void Engine::advance() {
    auto current_rates = rates();
    long double next = std::numeric_limits<long double>::infinity();
    for (const RatedTask& item : current_rates) {
        next = std::min(next, time_ + item.task->remaining / item.rate);
    }
    if (arrival_index_ < trace_.events.size() && trace_.events[arrival_index_].time_us > time_ + Epsilon) {
        next = std::min(next, static_cast<long double>(trace_.events[arrival_index_].time_us));
    }
    if (auto pending = next_pending_time()) next = std::min(next, *pending);
    if (!std::isfinite(next)) {
        std::vector<std::int64_t> pending;
        for (const auto& [id, task] : tasks_) if (!task.completed) pending.push_back(id);
        if (!pending.empty()) throw std::runtime_error("event loop deadlock");
        return;
    }
    long double delta = next - time_;
    for (const RatedTask& item : current_rates) {
        item.task->remaining = std::max(0.0L, item.task->remaining - item.rate * delta);
        if (item.task->remaining <= RemainingEpsilon) item.task->remaining = 0;
    }
    time_ = next;
    sync_remaining();
    std::vector<Task*> finished;
    for (Task* task : active_) if (task->remaining <= RemainingEpsilon) finished.push_back(task);
    std::sort(finished.begin(), finished.end(), [](const Task* lhs, const Task* rhs) {
        return lhs->transfer_id < rhs->transfer_id;
    });
    for (Task* task : finished) finish_task(*task);
}

Json Engine::run() {
    ensure_live();
    std::uint64_t event_steps = 0;
    while (true) {
        if (++event_steps > 2000000ULL) {
            throw std::runtime_error(
                "event loop exceeded safety limit at time_us=" +
                std::to_string(static_cast<double>(time_)) +
                ", active=" + std::to_string(active_.size()));
        }
        while (arrival_index_ < trace_.events.size() &&
               std::fabs(static_cast<long double>(trace_.events[arrival_index_].time_us) - time_) <= Epsilon) {
            handle_arrival(arrival_index_++);
        }
        start_ready_tasks();
        if (arrival_index_ >= trace_.events.size() && incomplete_ == 0) break;
        advance();
    }
    std::int64_t trace_time = ceil_us(time_) - first_arrival_;
    std::int64_t total_cost = cost("time_cost_per_us") * trace_time + cpu_cost_ + npu_cost_;
    double bandwidth = trace_time > 0
        ? round6(static_cast<long double>(valid_bytes_) * 1000000.0L / trace_time / MiB) : 0.0;
    double amplification = valid_bytes_ > 0
        ? round6(static_cast<long double>(physical_ssd_bytes_) / valid_bytes_) : 0.0;
    auto percentile = [](std::vector<std::int64_t> values, int percent) {
        if (values.empty()) return static_cast<std::int64_t>(0);
        std::sort(values.begin(), values.end());
        const double rank = std::ceil(static_cast<double>(percent) *
                                       static_cast<double>(values.size()) / 100.0);
        std::size_t index = static_cast<std::size_t>(std::max(1.0, rank)) - 1;
        if (index >= values.size()) index = values.size() - 1;
        return values[index];
    };
    const double promote_hit_rate = promote_count_ > 0
        ? round6(static_cast<long double>(promote_success_) / promote_count_) : 0.0;
    Json metrics = object({
        {"trace_time_us", trace_time}, {"valid_bytes", valid_bytes_},
        {"physical_ssd_bytes", physical_ssd_bytes_}, {"read_amplification", amplification},
        {"effective_bandwidth_mb_s", bandwidth}, {"cpu_cost", cpu_cost_},
        {"npu_cost", npu_cost_}, {"total_cost", total_cost}, {"cache_hits", cache_hits_},
        {"promote_count", promote_count_}, {"promote_success", promote_success_},
        {"promote_hit_rate", promote_hit_rate}, {"evictions", evictions_},
        {"latency_p50_us", percentile(latencies_, 50)},
        {"latency_p95_us", percentile(latencies_, 95)},
        {"latency_p99_us", percentile(latencies_, 99)},
    });
    return object({{"schedule_decisions", schedule_decisions_},
                   {"promotion_decisions", promotion_decisions_},
                   {"events", events_}, {"metrics", metrics}});
}

}  // namespace

Simulator::Simulator(Trace trace, SchedulerPolicy& policy)
    : trace_(std::move(trace)), policy_(policy) {}

Json Simulator::run() { return Engine(trace_, policy_).run(); }

}  // namespace nds
