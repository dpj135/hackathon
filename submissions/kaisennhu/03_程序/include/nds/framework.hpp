#pragma once

#include "nds/json.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace nds {

class ValidationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Iov {
    std::int64_t iov_id{};
    std::int64_t hbm_addr{};
    std::int64_t length{};
};

struct Iocb {
    std::int64_t submit_id{};
    std::int64_t iocb_id{};
    std::string opcode;
    std::int64_t device_id{};
    std::int64_t ssd_offset{};
    std::int64_t length{};
    std::vector<Iov> iovs;

    using CacheKey = std::tuple<std::int64_t, std::int64_t, std::int64_t>;
    CacheKey cache_key() const { return {device_id, ssd_offset, length}; }
};

struct Submit {
    std::int64_t submit_id{};
    std::int64_t arrival_us{};
    std::vector<Iocb> iocbs;
};

struct TraceEvent {
    std::int64_t time_us{};
    Submit submit;
};

struct Instance {
    std::string instance_id;
    std::map<std::string, std::int64_t> resources;
    std::map<std::string, std::int64_t> runtime_limits;
    std::map<std::string, std::int64_t> fixed_parameters;
};

struct Trace {
    Instance instance;
    std::vector<TraceEvent> events;
};

Trace parse_trace(const Json& root);
Trace load_trace(const std::string& path);

std::int64_t align_down(std::int64_t value, std::int64_t alignment = 512);
std::int64_t align_up(std::int64_t value, std::int64_t alignment = 512);
std::pair<std::int64_t, std::int64_t> physical_span(std::int64_t offset, std::int64_t length);
void validate_decision(const Submit& submit, const Json& decision,
                       const std::map<std::string, std::int64_t>& resources);
void compare_completion_events(const Json& expected, const Json& claimed);

class SchedulerPolicy {
public:
    virtual ~SchedulerPolicy() = default;
    virtual Json on_submit(const Submit& submit, const Json& runtime_state) = 0;
    virtual std::optional<Json> on_iocb_complete(const Iocb& iocb,
                                                 const Json& runtime_state) = 0;
};

class GreedyBaseline final : public SchedulerPolicy {
public:
    explicit GreedyBaseline(std::map<std::string, std::int64_t> resources,
                            bool enable_promote = true, bool promote_via_ssd = false);
    Json on_submit(const Submit& submit, const Json& runtime_state) override;
    std::optional<Json> on_iocb_complete(const Iocb& iocb,
                                         const Json& runtime_state) override;

private:
    std::map<std::string, std::int64_t> resources_;
    bool enable_promote_;
    bool promote_via_ssd_;
    std::int64_t next_operation_{1};
    std::int64_t next_transfer_{1};
    std::int64_t next_group_{1};
    std::int64_t next_promote_{1};
    std::map<Iocb::CacheKey, int> seen_;
    std::map<Iocb::CacheKey, bool> promoted_;
};

class ReplayPolicy final : public SchedulerPolicy {
public:
    explicit ReplayPolicy(const Json& solution);
    Json on_submit(const Submit& submit, const Json& runtime_state) override;
    std::optional<Json> on_iocb_complete(const Iocb& iocb,
                                         const Json& runtime_state) override;

private:
    std::map<std::int64_t, Json> decisions_;
    std::map<std::pair<std::int64_t, std::int64_t>, Json> promotions_;
};

class Simulator {
public:
    Simulator(Trace trace, SchedulerPolicy& policy);
    Json run();

private:
    struct Impl;
    Trace trace_;
    SchedulerPolicy& policy_;
};

}  // namespace nds
