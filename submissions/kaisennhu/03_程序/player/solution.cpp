#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

#include "nds/player_api.hpp"

// Interval DP, bounded observed-window planning, and read-funded cache admission.
// No files, network or future trace access.
namespace {
using I = std::int64_t;
using nds::Json;
constexpr I MiB = 1024 * 1024;
constexpr std::size_t Window = 128, HistoryLimit = 131072;
Json obj(std::initializer_list<std::pair<const std::string, Json>> f) {
    return Json(Json::Object(f));
}
Json arr(std::initializer_list<Json> f) { return Json(Json::Array(f)); }
// Match the public simulator: MB means 2^20 bytes. Estimates are NOT completion times.
using Profile = std::map<I, std::map<int, int>>;
const Profile direct_profile = {{512, {{1, 2}}},
                                {4096, {{1, 20}}},
                                {32768, {{1, 10}, {4, 30}, {16, 50}, {64, 75}, {1024, 90}}},
                                {65536, {{1, 15}, {4, 40}, {16, 60}, {64, 100}}},
                                {MiB, {{1, 55}, {4, 90}, {16, 100}}},
                                {2 * MiB, {{1, 70}, {4, 100}}},
                                {8 * MiB, {{1, 95}, {2, 100}}}};
const Profile ddr_profile = {{512, {{1, 2}}},
                             {4096, {{1, 15}}},
                             {32768, {{1, 10}, {4, 28}, {16, 48}, {64, 75}, {1024, 85}}},
                             {65536, {{1, 15}, {4, 40}, {16, 60}, {64, 90}}},
                             {MiB, {{1, 50}, {4, 80}, {16, 90}}},
                             {2 * MiB, {{1, 60}, {4, 90}}},
                             {8 * MiB, {{1, 85}, {2, 90}}}};
const Profile copy_profile = {{512, {{1, 1}, {4, 2}, {16, 3}, {256, 4}}},
                              {4096, {{1, 1}, {4, 2}, {256, 3}}},
                              {32768, {{1, 6}, {4, 14}, {16, 20}, {64, 22}, {1024, 24}}},
                              {65536, {{1, 12}, {4, 26}, {16, 37}, {256, 45}}},
                              {MiB, {{1, 50}, {4, 90}, {16, 100}}},
                              {2 * MiB, {{1, 75}, {4, 100}}},
                              {8 * MiB, {{1, 80}}}};
const Profile d2d_profile = {
    {32768, {{1, 1500}, {4, 5000}, {16, 20000}, {64, 65000}, {256, 200000}, {1024, 450000}}},
    {65536, {{1, 4000}, {4, 14000}, {16, 40000}, {64, 150000}, {256, 360000}, {1024, 520000}}}};
template <class M>
auto floor_entry(const M& m, typename M::key_type k) {
    auto it = m.upper_bound(k);
    return it == m.begin() ? it : std::prev(it);
}
double bucket(const Profile& p, I size, int q) {
    return floor_entry(floor_entry(p, size)->second, std::max(1, q))->second;
}
double transfer_us(I size, I bw, const Profile& p, int q) {
    return double(size) * 1000000.0 / (double(bw) * MiB * bucket(p, size, q) / 100.0);
}
struct IovIndex {
    std::vector<I> ends;
    explicit IovIndex(const nds::Iocb& io) {
        ends.reserve(io.iovs.size());
        I end = 0;
        for (const auto& v : io.iovs) { end += v.length; ends.push_back(end); }
    }
    std::size_t first(I off) const {
        return std::upper_bound(ends.begin(), ends.end(), off) - ends.begin();
    }
};
struct Piece {
    const nds::Iocb* io;
    const IovIndex* index;
    I offset, length;
    bool aligned, whole;
    I begin() const { return io->ssd_offset + offset; }
    I end() const { return begin() + length; }
};
// A local evaluator receives only explicitly constructed, already observed work.
class FixedPlan final : public nds::SchedulerPolicy {
    Json plan_;
    std::map<I, Json> prefix_;

   public:
    explicit FixedPlan(Json p, std::map<I, Json> prefix = {})
        : plan_(std::move(p)), prefix_(std::move(prefix)) {}
    Json on_submit(const nds::Submit& s, const Json&) override {
        auto it = prefix_.find(s.submit_id);
        return it == prefix_.end() ? plan_ : it->second;
    }
    std::optional<Json> on_iocb_complete(const nds::Iocb&, const Json&) override {
        return std::nullopt;
    }
};
bool direct_ok(const nds::Iocb& io, const IovIndex& index, I off, I len) {
    if ((io.ssd_offset + off) % 512 || len % 512) return false;
    auto k = index.first(off);
    I cursor = k ? index.ends[k - 1] : 0;
    for (; k < io.iovs.size() && cursor < off + len; ++k) {
        const auto& v = io.iovs[k];
        I a = std::max(off, cursor), b = std::min(off + len, cursor + v.length);
        if (a < b && ((v.hbm_addr + a - cursor) % 512 || (b - a) % 512)) return false;
        cursor += v.length;
    }
    return true;
}
class AdaptivePolicy final : public nds::SchedulerPolicy {
    std::map<std::string, I> r_, c_;
    I next_op_ = 1, next_transfer_ = 1, next_promote_ = 1;
    struct History {
        I seen = 0, last_arrival = 0, last_byte = 0, promote_id = 0, generation = 0;
        I observed = 0, accounted = 0;
        double gap = 0, jitter = 0, credit = 0, last_gain = 0, paid_cost = 0;
    };
    std::map<nds::Iocb::CacheKey, History> history_;
    std::deque<std::pair<nds::Iocb::CacheKey, I>> history_queue_;
    I byte_clock_ = 0, generation_ = 0;
    std::map<std::pair<I, I>, I> arrivals_;
    struct Work { double read_us = 0, copy_us = 0, commands = 0, card_share = 1; int hit_q = 1; };
    std::map<std::pair<I, I>, Work> work_;
    std::map<std::vector<I>, I> batch_costs_;
    std::map<I, I> promotion_cards_;
    struct ObservedBatch { nds::Submit submit; Json decision; };
    std::deque<ObservedBatch> recent_;
    double cost(const char* key) const { return double(c_.at(key)); }
    I pool(const char* name) const { return r_.at(name) * MiB; }
    // Additive resource-work surrogate, not an exact objective under shared BW.
    double estimate(const std::string& path, I physical, I valid, int qs, int qc,
                    const Json& state) const {
        double cpu = 0, npu = 0, t = 0;
        if (path == "DIRECT" || path == "AGGREGATE") {
            cpu = cost("CPU_NDS_FIXED_COST") + cost("CPU_NDS_PER_CMD_COST");
            t = transfer_us(physical, r_.at("nvme_bw"), direct_profile, qs);
        } else {
            cpu = cost("CPU_FALLBACK_FIXED_COST") + cost("CPU_FALLBACK_PER_CMD_COST");
            t = transfer_us(physical, r_.at("nvme_bw"), ddr_profile, qs);
        }
        if (path == "AGGREGATE") {
            qc = std::max(1, std::min<int>(qc, int(r_.at("hbm_copy_concurrency"))));
            double bw = valid < 4096    ? 20.0 * qc
                        : valid < 32768 ? 130.0 * qc
                                        : bucket(d2d_profile, valid, qc);
            bw = std::min(bw, double(r_.at("d2d_bw")));
            t += double(valid) * 1000000.0 / (bw * MiB);
            cpu += cost("CPU_D2D_CALL_COST");
            npu += cost("NPU_D2D_CALL_COST");
            t *= 1.0 + 0.5 * state.at("aggregate_hbm_used").as_int() /
                           std::max(1.0, double(pool("hbm_capacity")));
        } else if (path == "FALLBACK") {
            qc = std::max(1, std::min<int>(qc, int(r_.at("h2d_copy_concurrency"))));
            t += transfer_us(valid, r_.at("h2d_bw"), copy_profile, qc);
            cpu += cost("CPU_H2D_CALL_COST");
            npu += cost("NPU_H2D_CALL_COST");
            t *= 1.0 + 0.5 * state.at("fallback_ddr_used").as_int() /
                           std::max(1.0, double(pool("ddr_capacity")));
        }
        return cost("time_cost_per_us") * t + cpu + npu;
    }
    Json operation(const std::vector<Piece>& p, std::size_t begin, std::size_t end,
                   const std::string& path, I now) {
        I lo = p[begin].begin(), hi = lo, bytes = 0;
        Json ranges = Json::array(), vectors = Json::array();
        for (std::size_t k = begin; k < end; ++k) {
            const auto& x = p[k];
            hi = std::max(hi, x.end());
            bytes += x.length;
            ranges.push_back(
                obj({{"iocb_id", x.io->iocb_id}, {"iocb_offset", x.offset}, {"length", x.length}}));
            auto vi = x.index->first(x.offset);
            I cursor = vi ? x.index->ends[vi - 1] : 0;
            for (; vi < x.io->iovs.size() && cursor < x.offset + x.length; ++vi) {
                const auto& v = x.io->iovs[vi];
                I a = std::max(x.offset, cursor),
                  b = std::min(x.offset + x.length, cursor + v.length);
                if (a < b)
                    vectors.push_back(obj({{"iocb_id", x.io->iocb_id},
                                           {"iov_id", v.iov_id},
                                           {"iov_offset", a - cursor},
                                           {"length", b - a}}));
                cursor += v.length;
            }
        }
        auto [physical, length] = nds::physical_span(lo, hi - lo);
        I id = next_op_++, first = next_transfer_++, device = p[begin].io->device_id;
        Json transfers = arr({obj({{"transfer_id", first},
                                   {"type", path == "DIRECT"      ? "DIRECT"
                                            : path == "AGGREGATE" ? "SSD_TO_HBM_BUFFER"
                                                                  : "SSD_TO_DDR"},
                                   {"device_id", device},
                                   {"physical_ssd_offset", physical},
                                   {"physical_length", length},
                                   {"group_id", id},
                                   {"not_before_us", now},
                                   {"depends_on", Json::array()},
                                   {"priority", 100}})});
        if (path != "DIRECT")
            transfers.push_back(obj({{"transfer_id", next_transfer_++},
                                     {"type", path == "AGGREGATE" ? "D2D" : "H2D"},
                                     {"device_id", device},
                                     {"bytes", bytes},
                                     {"vectors", std::move(vectors)},
                                     {"group_id", id},
                                     {"not_before_us", now},
                                     {"depends_on", arr({Json(first)})},
                                     {"priority", 100}}));
        Json result = obj({{"operation_id", id},
                           {"path", path},
                           {"ranges", std::move(ranges)},
                           {"transfers", std::move(transfers)}});
        if (path != "DIRECT") result["buffer_bytes"] = length;
        return result;
    }

   public:
    AdaptivePolicy(std::map<std::string, I> r, std::map<std::string, I> c)
        : r_(std::move(r)), c_(std::move(c)) {}
    Json on_submit(const nds::Submit& submit, const Json& state) override {
        I now = state.at("current_time_us").as_int();
        for (auto it = recent_.begin(); it != recent_.end();) {
            bool pending = std::any_of(it->submit.iocbs.begin(), it->submit.iocbs.end(),
                [&](const nds::Iocb& io) { return arrivals_.count({io.submit_id, io.iocb_id}) != 0; });
            if (!pending) it = recent_.erase(it); else ++it;
        }
        std::map<I, I> live_cards;
        for (const auto& p : state.at("promote_inflight").as_array()) {
            I id = p.at("promote_id").as_int();
            auto known = promotion_cards_.find(id);
            if (known != promotion_cards_.end()) live_cards.emplace(*known);
        }
        promotion_cards_.swap(live_cards);
        std::vector<IovIndex> indexes;
        indexes.reserve(submit.iocbs.size());  // Piece pointers must remain stable.
        std::map<I, int> per_card;
        I max_pool = std::max(pool("hbm_capacity"), pool("ddr_capacity"));
        for (const auto& io : submit.iocbs) {
            indexes.emplace_back(io);
            arrivals_[{io.submit_id, io.iocb_id}] = submit.arrival_us;
            auto& h = history_[io.cache_key()];
            bool observed_again = h.seen && submit.arrival_us > h.last_arrival;
            if (h.seen && submit.arrival_us > h.last_arrival) {
                double gap = double(submit.arrival_us - h.last_arrival);
                if (h.gap == 0) h.gap = gap;
                else {
                    h.jitter = 0.5 * h.jitter + 0.5 * std::abs(gap - h.gap);
                    h.gap = 0.5 * h.gap + 0.5 * gap;
                }
            }
            // Reuse-supported byte distance excludes one-shot scan traffic. It is
            // still an upper estimate because recurring keys can be counted again.
            I distance = byte_clock_ - h.last_byte;
            if (h.seen && (distance > pool("kvcache_pool_capacity") ||
                           submit.arrival_us - h.last_arrival >= cost("promote_ttl_us"))) {
                h.seen = 0;
                h.credit = 0;
                h.observed = 0;
                h.accounted = 0;
            }
            if (h.seen == 0 || observed_again) ++h.observed;
            ++h.seen;
            h.last_arrival = submit.arrival_us;
            h.last_byte = byte_clock_;
            h.generation = ++generation_;
            if (observed_again && h.seen > 1 && io.length <= pool("kvcache_pool_capacity"))
                byte_clock_ += io.length;
            history_queue_.push_back({io.cache_key(), h.generation});
            while (history_queue_.size() > HistoryLimit) {
                auto [key, gen] = history_queue_.front();
                history_queue_.pop_front();
                auto found = history_.find(key);
                if (found != history_.end() && found->second.generation == gen)
                    history_.erase(found);
            }
            ++per_card[io.device_id];
        }
        auto build_decision = [&](I block_limit) {
            std::vector<Piece> pieces;
            for (std::size_t k = 0; k < submit.iocbs.size(); ++k) {
                const auto& io = submit.iocbs[k];
                const auto& index = indexes[k];
                for (I offset = 0; offset < io.length;) {
                    I start = io.ssd_offset + offset, length = std::min<I>(block_limit, io.length - offset);
                    if (!direct_ok(io, index, offset, length)) {
                        if (max_pool < 512)
                            throw nds::ValidationError("no temporary pool for unaligned IO");
                        length = std::min(length, max_pool - start % 512);
                        if (offset + length < io.length) length -= (start + length) % 512;
                        if (length <= 0) throw nds::ValidationError("temporary pool too small");
                    }
                    pieces.push_back({&io, &index, offset, length, direct_ok(io, index, offset, length),
                                      offset == 0 && length == io.length});
                    offset += length;
                }
            }
            std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) {
                return std::make_tuple(a.io->device_id, a.begin(), a.io->iocb_id, a.offset) <
                       std::make_tuple(b.io->device_id, b.begin(), b.io->iocb_id, b.offset);
            });
            int active_ssd = 0;
            std::map<std::pair<std::string, I>, int> active_copy;
            for (const auto& t : state.at("transfers_inflight").as_array()) {
                auto type = t.at("type").as_string();
                if (type == "DIRECT" || type.find("SSD_TO_") == 0)
                    ++active_ssd;
                else
                    ++active_copy[{type, t.at("device_id").as_int()}];
            }
            int qs = std::max(1, active_ssd + int(pieces.size()));
            auto n = pieces.size();
            std::vector<double> dp(n + 1, std::numeric_limits<double>::infinity());
            std::vector<std::size_t> prev(n + 1);
            std::vector<std::string> paths(n + 1);
            dp[0] = 0;
            for (std::size_t i = 0; i < n; ++i) {
                I hi = pieces[i].begin(), valid = 0;
                bool aligned = true, whole = true;
                std::set<I> owners;
                I card = pieces[i].io->device_id;
                std::size_t window = Window;
                for (std::size_t j = i; j < n && j < i + window; ++j) {
                    const auto& p = pieces[j];
                    if (p.io->device_id != pieces[i].io->device_id) break;
                    if (j > i && nds::align_down(p.begin()) > nds::align_up(hi)) break;
                    // Split iocbs never share an operation with a different owner.
                    // This prevents cross-operation circular waits for NDS slots.
                    if (j > i && ((!whole || !p.whole) && p.io != pieces[i].io)) break;
                    whole = whole && p.whole;
                    if (j > i && p.begin() != hi) aligned = false;
                    aligned = aligned && p.aligned;
                    hi = std::max(hi, p.end());
                    valid += p.length;
                    owners.insert(p.io->iocb_id);
                    I physical = nds::align_up(hi) - nds::align_down(pieces[i].begin());
                    if (j > i && physical > block_limit) break;
                    for (const std::string path : {"DIRECT", "AGGREGATE", "FALLBACK"}) {
                        if (path != "FALLBACK" && owners.size() > std::size_t(r_.at("iocb_capacity")))
                            continue;
                        if (path == "DIRECT" && !aligned) continue;
                        if (path == "AGGREGATE" && physical > pool("hbm_capacity")) continue;
                        if (path == "FALLBACK" && physical > pool("ddr_capacity")) continue;
                        int qc = std::max(1, active_copy[{path == "AGGREGATE" ? "D2D" : "H2D", card}] + per_card[card]);
                        double v = dp[i] + estimate(path, physical, valid, qs, qc, state);
                        if (v < dp[j + 1]) {
                            dp[j + 1] = v;
                            prev[j + 1] = i;
                            paths[j + 1] = path;
                        }
                    }
                }
            }
            if (!std::isfinite(dp[n]))
                throw nds::ValidationError("no feasible path under supplied resources");
            std::vector<std::tuple<std::size_t, std::size_t, std::string>> plan;
            for (std::size_t end = n; end > 0; end = prev[end])
                plan.emplace_back(prev[end], end, paths[end]);
            Json operations = Json::array();
            for (auto it = plan.rbegin(); it != plan.rend(); ++it) {
                auto [a, b, path] = *it;
                operations.push_back(operation(pieces, a, b, path, now));
            }
            Json decision = obj({{"event", "SCHEDULE_DECISION"},
                                 {"submit_id", submit.submit_id},
                                 {"operations", std::move(operations)},
                                 {"next_wakeup_us", Json(nullptr)}});
            std::size_t total_iovs = 0;
            for (const auto& io : submit.iocbs) total_iovs += io.iovs.size();
            // At most 3*4^5 plans. Exact within this restricted family, only when
            // conservatively idle; cached or previously promoted work disables search.
            if (n <= 6 && total_iovs <= 256 && next_promote_ == 1 &&
                arrivals_.size() == submit.iocbs.size() && state.at("cached_ranges").size() == 0 &&
                state.at("transfers_inflight").size() == 0 &&
                state.at("queued_iocbs").size() == 0 && state.at("promote_inflight").size() == 0 &&
                state.at("foreground_iocb_inflight").as_int() == 0 &&
                std::all_of(pieces.begin(), pieces.end(), [](const Piece& p) { return p.whole; })) {
                nds::Trace local;
                local.instance.resources = r_;
                local.instance.fixed_parameters = c_;
                local.events.push_back({submit.arrival_us, submit});
                auto evaluate = [&](const Json& plan) {
                    FixedPlan policy(plan);
                    nds::Simulator simulator(local, policy);
                    return simulator.run().at("metrics").at("total_cost").as_int();
                };
                I best = evaluate(decision);
                const I op_base = next_op_, transfer_base = next_transfer_;
                auto search = [&](auto&& self, std::size_t start, Json ops) -> void {
                    if (start == n) {
                        Json candidate = obj({{"event", "SCHEDULE_DECISION"},
                                              {"submit_id", submit.submit_id},
                                              {"operations", std::move(ops)},
                                              {"next_wakeup_us", Json(nullptr)}});
                        I value = evaluate(candidate);
                        if (value < best) {
                            best = value;
                            decision = std::move(candidate);
                        }
                        return;
                    }
                    I hi = pieces[start].begin();
                    bool direct = true;
                    for (std::size_t end = start; end < n; ++end) {
                        const auto& p = pieces[end];
                        if (p.io->device_id != pieces[start].io->device_id) break;
                        if (end > start && nds::align_down(p.begin()) > nds::align_up(hi)) break;
                        if (end > start && p.begin() != hi) direct = false;
                        direct = direct && p.aligned;
                        hi = std::max(hi, p.end());
                        I physical = nds::align_up(hi) - nds::align_down(pieces[start].begin());
                        if (end > start && physical > block_limit) break;
                        for (const std::string path : {"DIRECT", "AGGREGATE", "FALLBACK"}) {
                            if (path != "FALLBACK" &&
                                end - start + 1 > std::size_t(r_.at("iocb_capacity")))
                                continue;
                            if (path == "DIRECT" && !direct) continue;
                            if (path == "AGGREGATE" && physical > pool("hbm_capacity")) continue;
                            if (path == "FALLBACK" && physical > pool("ddr_capacity")) continue;
                            next_op_ = op_base + I(ops.size());
                            next_transfer_ = transfer_base + 2 * I(ops.size());
                            auto next = ops;
                            next.push_back(operation(pieces, start, end + 1, path, now));
                            self(self, end + 1, std::move(next));
                        }
                    }
                };
                search(search, 0, Json::array());
                next_op_ = op_base + I(n);
                next_transfer_ = transfer_base + 2 * I(n);
            }
            return decision;
        };
        Json decision = build_decision(2 * MiB);
        std::size_t iov_count = 0;
        for (const auto& io : submit.iocbs) iov_count += io.iovs.size();
        // At most six plans, with counterfactual path choices on a bounded
        // observed window. Only the current decision is issued. Older omitted
        // work and cache-hit/background traffic make this a surrogate for J.
        if (submit.iocbs.size() <= 1024 && iov_count <= 2048 &&
            decision.at("operations").size() <= 2048) {
            std::map<I, Json> prefix;
            nds::Trace local;
            local.instance.resources = r_; local.instance.fixed_parameters = c_;
            std::size_t prefix_iovs = iov_count;
            std::size_t prefix_ops = decision.at("operations").size();
            bool busy = state.at("queued_iocbs").size() || state.at("transfers_inflight").size();
            if (busy) for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
                std::size_t count = 0;
                for (const auto& io : it->submit.iocbs) count += io.iovs.size();
                if (prefix_iovs + count > 32768 ||
                    prefix_ops + it->decision.at("operations").size() > 512) break;
                prefix_iovs += count;
                prefix_ops += it->decision.at("operations").size();
                local.events.insert(local.events.begin(), {it->submit.arrival_us, it->submit});
                prefix[it->submit.submit_id] = it->decision;
            }
            auto visible = submit; visible.arrival_us = now;
            local.events.push_back({now, std::move(visible)});
            std::map<I, I> ordinal;
            for (const auto& io : submit.iocbs) ordinal[io.iocb_id] = I(ordinal.size());
            auto rewrite_path = [&](Json& plan, const std::string& forced) {
                if (forced.empty()) return;
                for (auto& op : plan["operations"].as_array()) {
                    auto path = op.at("path").as_string();
                    if (path == "DIRECT" || path == forced) continue;
                    std::set<I> owners;
                    for (const auto& range : op.at("ranges").as_array()) owners.insert(range.at("iocb_id").as_int());
                    if (forced == "AGGREGATE" && owners.size() > std::size_t(r_.at("iocb_capacity"))) continue;
                    if (op.at("buffer_bytes").as_int() > pool(forced == "AGGREGATE" ? "hbm_capacity" : "ddr_capacity")) continue;
                    op["path"] = forced;
                    op["transfers"][0]["type"] = forced == "AGGREGATE" ? "SSD_TO_HBM_BUFFER" : "SSD_TO_DDR";
                    op["transfers"][1]["type"] = forced == "AGGREGATE" ? "D2D" : "H2D";
                }
            };
            auto evaluate = [&](const Json& candidate, const std::string& mode) {
                std::vector<I> key;
                key.push_back(I(submit.iocbs.size()));
                for (const auto& io : submit.iocbs) {
                    key.push_back(io.device_id); key.push_back(io.length);
                }
                key.push_back(I(candidate.at("operations").size()));
                for (const auto& op : candidate.at("operations").as_array()) {
                    const auto& transfers = op.at("transfers");
                    auto path = op.at("path").as_string();
                    key.push_back(path == "DIRECT" ? 0 : path == "AGGREGATE" ? 1 : 2);
                    key.push_back(transfers[0].at("device_id").as_int());
                    key.push_back(transfers[0].at("physical_length").as_int());
                    key.push_back(I(op.at("ranges").size()));
                    for (const auto& range : op.at("ranges").as_array()) {
                        key.push_back(ordinal.at(range.at("iocb_id").as_int()));
                        key.push_back(range.at("iocb_offset").as_int());
                        key.push_back(range.at("length").as_int());
                    }
                }
                auto cached = batch_costs_.find(key);
                if (prefix.empty() && cached != batch_costs_.end()) return cached->second;
                auto scenario = prefix;
                for (auto& entry : scenario) rewrite_path(entry.second, mode);
                FixedPlan policy(candidate, std::move(scenario));
                nds::Simulator simulator(local, policy);
                I value = simulator.run().at("metrics").at("total_cost").as_int();
                if (prefix.empty() && key.size() <= 32768) {
                    if (batch_costs_.size() >= 64) batch_costs_.erase(batch_costs_.begin());
                    batch_costs_.emplace(std::move(key), value);
                }
                return value;
            };
            I best = evaluate(decision, "");
            for (I block : {2 * MiB, 8 * MiB}) {
                Json base = block == 2 * MiB ? decision : build_decision(block);
                for (const std::string forced : {"", "AGGREGATE", "FALLBACK"}) {
                    Json candidate = base;
                    rewrite_path(candidate, forced);
                    I value = evaluate(candidate, forced);
                    if (value < best) { best = value; decision = std::move(candidate); }
                }
            }
        }
        int active_ssd = 0;
        for (const auto& t : state.at("transfers_inflight").as_array()) {
            auto type = t.at("type").as_string();
            if (type == "DIRECT" || type.find("SSD_TO_") == 0) ++active_ssd;
        }
        if (iov_count <= 2048 && submit.iocbs.size() <= 1024 && decision.at("operations").size() <= 2048) {
            auto observed = submit; observed.arrival_us = now;
            recent_.push_back({std::move(observed), decision});
            while (recent_.size() > 32) recent_.pop_front();
        } else {
            recent_.clear();
        }
        // Account only the final selected plan. Shared commands are charged once,
        // then apportioned by useful bytes; queueing latency is not service work.
        std::map<I, I> card_bytes;
        std::map<I, int> card_ops;
        I total_bytes = 0;
        for (const auto& io : submit.iocbs) {
            card_bytes[io.device_id] += io.length;
            total_bytes += io.length;
        }
        for (const auto& op : decision.at("operations").as_array())
            ++card_ops[op.at("transfers")[0].at("device_id").as_int()];
        int read_q = std::max(1, active_ssd + int(decision.at("operations").size()));
        for (const auto& op : decision.at("operations").as_array()) {
            const auto& first = op.at("transfers")[0];
            I card = first.at("device_id").as_int();
            I bytes = 0;
            for (const auto& range : op.at("ranges").as_array()) bytes += range.at("length").as_int();
            std::string path = op.at("path").as_string();
            double read = transfer_us(first.at("physical_length").as_int(), r_.at("nvme_bw"),
                                      path == "FALLBACK" ? ddr_profile : direct_profile, read_q);
            double copy = 0, commands = cost("CPU_NDS_FIXED_COST") + cost("CPU_NDS_PER_CMD_COST");
            int copy_q = std::max(1, card_ops[card]);
            if (path == "FALLBACK") {
                copy_q = std::min<int>(copy_q, int(r_.at("h2d_copy_concurrency")));
                copy = transfer_us(bytes, r_.at("h2d_bw"), copy_profile, copy_q);
                commands = cost("CPU_FALLBACK_FIXED_COST") + cost("CPU_FALLBACK_PER_CMD_COST") +
                           cost("CPU_H2D_CALL_COST") + cost("NPU_H2D_CALL_COST");
            } else if (path == "AGGREGATE") {
                copy_q = std::max(1, std::min<int>(copy_q, int(r_.at("hbm_copy_concurrency"))));
                double bw = bytes < 4096 ? 20.0 * copy_q : bytes < 32768 ? 130.0 * copy_q :
                            bucket(d2d_profile, bytes, copy_q);
                copy = double(bytes) * 1000000.0 / (std::min(bw, double(r_.at("d2d_bw"))) * MiB);
                commands += cost("CPU_D2D_CALL_COST") + cost("NPU_D2D_CALL_COST");
            }
            for (const auto& range : op.at("ranges").as_array()) {
                auto& w = work_[{submit.submit_id, range.at("iocb_id").as_int()}];
                double fraction = double(range.at("length").as_int()) / bytes;
                w.read_us += read * fraction;
                w.copy_us += copy * fraction;
                w.commands += commands * fraction;
                w.card_share = double(card_bytes[card]) / total_bytes;
                w.hit_q = std::max(1, std::min<int>(per_card[card], int(r_.at("h2d_copy_concurrency"))));
            }
        }
        return decision;
    }
    std::optional<Json> on_iocb_complete(const nds::Iocb& io, const Json& state) override {
        I now = state.at("current_time_us").as_int();
        auto owner = std::make_pair(io.submit_id, io.iocb_id);
        auto arrival = arrivals_.find(owner);
        auto work = work_.find(owner);
        Work served;
        if (work != work_.end()) { served = work->second; work_.erase(work); }
        // Cache hits have no foreground work owned by this policy.
        bool foreground = arrival != arrivals_.end();
        if (arrival != arrivals_.end()) arrivals_.erase(arrival);
        auto found = history_.find(io.cache_key());
        if (found == history_.end()) return std::nullopt;
        auto& h = found->second;
        if (!foreground) {
            // A completion without policy-owned miss work is a cache hit.
            // Earn at most the previous admission cost for a later reinsertion.
            h.credit = std::min(h.paid_cost, h.credit + std::max(0.0, h.last_gain));
            return std::nullopt;
        }
        if (h.seen < 2 || h.gap <= 0 || io.length > pool("kvcache_pool_capacity")) return std::nullopt;
        if ((h.gap >= cost("promote_ttl_us") || cost("promote_ttl_us") <= 0))
            return std::nullopt;
        // Arrival evidence can become stale while foreground IO waits in a queue.
        // This conservative admission rule uses only already observed demand.
        if ((now - h.last_arrival >= cost("promote_ttl_us") ||
             byte_clock_ - h.last_byte > pool("kvcache_pool_capacity")))
            return std::nullopt;
        if (now - h.last_arrival > std::max(1.0, h.gap + 2.0 * h.jitter)) return std::nullopt;
        for (const auto& key : state.at("cached_ranges").as_array())
            if (key[0].as_int() == io.device_id && key[1].as_int() == io.ssd_offset &&
                key[2].as_int() == io.length)
                return std::nullopt;
        int pending_hbm = 0, pending_ssd = 0;
        for (const auto& p : state.at("promote_inflight").as_array()) {
            if (p.at("promote_id").as_int() == h.promote_id) return std::nullopt;
            if (p.at("path").as_string() == "PROMOTE_HBM") {
                auto card = promotion_cards_.find(p.at("promote_id").as_int());
                if (card != promotion_cards_.end() && card->second == io.device_id) ++pending_hbm;
            }
            else
                ++pending_ssd;
        }
        int active_ssd = 0, active_d2h = 0;
        for (const auto& t : state.at("transfers_inflight").as_array()) {
            auto type = t.at("type").as_string();
            if (type == "DIRECT" || type.find("SSD_TO_") == 0) ++active_ssd;
            if (type == "D2H" && t.at("device_id").as_int() == io.device_id) ++active_d2h;
        }
        auto [offset, physical] = nds::physical_span(io.ssd_offset, io.length);
        int qs = std::max(1, active_ssd + 1);
        int qh = std::max(1, std::min<int>(int(r_.at("d2h_copy_concurrency")), active_d2h + 1));
        double ts = transfer_us(physical, r_.at("nvme_bw"), ddr_profile, qs) * qs;
        double th = transfer_us(io.length, r_.at("d2h_bw"), copy_profile, qh) * qh;
        // Pending work is scoped to the actual source card.
        th *= 1.0 + double(pending_hbm) / std::max<I>(1, r_.at("d2h_copy_concurrency"));
        ts *= 1.0 + double(pending_ssd) / std::max(1, qs);
        double ss = cost("time_cost_per_us") * ts + cost("CPU_PROMOTE_PER_CMD_COST");
        double sh =
            cost("time_cost_per_us") * th + cost("CPU_D2H_CALL_COST") + cost("NPU_D2H_CALL_COST");
        bool via_ssd = ss < sh;
        if (r_.at("d2h_copy_concurrency") == 0) via_ssd = true;
        // Queue delay determines source readiness, but is not extra service
        // work to charge to every new promotion in that same queue.
        double service = via_ssd
            ? cost("time_cost_per_us") * transfer_us(physical, r_.at("nvme_bw"), ddr_profile, qs) + cost("CPU_PROMOTE_PER_CMD_COST")
            : cost("time_cost_per_us") * transfer_us(io.length, r_.at("d2h_bw"), copy_profile, qh) * served.card_share + cost("CPU_D2H_CALL_COST") + cost("NPU_D2H_CALL_COST");
        double promote_cost = cost("CPU_PROMOTE_FIXED_COST") + service;
        double hit = transfer_us(io.length, r_.at("h2d_bw"), copy_profile, served.hit_q);
        // Global SSD work and per-card copy work have different parallel scope.
        // This remains a marginal-work estimate, not a global optimality claim.
        double gain = cost("time_cost_per_us") *
                          (served.read_us + (served.copy_us - hit) * served.card_share) +
                      served.commands - cost("CPU_H2D_CALL_COST") - cost("NPU_H2D_CALL_COST");
        // Fund promotion from estimated work of already observed reads. Reuse
        // must still be fresh and each future hit must have positive net value.
        if (h.observed > h.accounted) {
            double spent = cost("time_cost_per_us") *
                (served.read_us + served.copy_us * served.card_share) + served.commands;
            h.credit += (h.observed - h.accounted) * std::max(0.0, spent);
            h.accounted = h.observed;
        }
        if (gain <= 0 || h.credit < promote_cost) return std::nullopt;
        h.credit = 0;
        h.last_gain = gain;
        h.paid_cost = promote_cost;
        I promote = next_promote_++;
        h.promote_id = promote;
        promotion_cards_[promote] = io.device_id;
        Json transfer = obj({{"transfer_id", next_transfer_++},
                             {"device_id", io.device_id},
                             {"priority", 10}});
        if (via_ssd) {
            transfer["type"] = "SSD_TO_MOONCAKE";
            transfer["physical_ssd_offset"] = offset;
            transfer["physical_length"] = physical;
        } else {
            transfer["type"] = "D2H";
            transfer["bytes"] = io.length;
            transfer["vectors"] = I(io.iovs.size());
        }
        return obj({{"promote_id", promote},
                    {"not_before_us", now},
                    {"source_submit_id", io.submit_id},
                    {"source_iocb_id", io.iocb_id},
                    {"path", via_ssd ? "PROMOTE_SSD" : "PROMOTE_HBM"},
                    {"transfer", std::move(transfer)}});
    }
};
}  // namespace
std::unique_ptr<nds::SchedulerPolicy> nds::create_policy(
    const std::map<std::string, I>& resources, const std::map<std::string, I>& fixed_parameters) {
    return std::make_unique<AdaptivePolicy>(resources, fixed_parameters);
}
