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
constexpr std::size_t CountTransitions = 2000000, CountStates = 131072;
constexpr std::size_t PlanTransfers = 4096;
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
bool ssd_type(const std::string& type) {
    return type == "DIRECT" || type.find("SSD_TO_") == 0;
}
// Samples are hypotheses about the final SSD-command count, not measured q.
std::vector<int> count_samples(int n, int active, I bytes, int seed) {
    std::vector<int> out;
    auto add = [&](I count) {
        if (count >= 1 && count <= n &&
            std::find(out.begin(), out.end(), int(count)) == out.end()) out.push_back(int(count));
    };
    add(seed); add(1); add(n);
    if (n <= 24) { for (int k = 2; k < n; ++k) add(k); return out; }
    for (const auto* profile : {&direct_profile, &ddr_profile})
        for (const auto& [size, qs] : *profile) {
            (void)size;
            for (const auto& [q, value] : qs) { (void)value; add(I(q) - active); }
        }
    for (I k = 2; k < n; k *= 2) add(k);
    for (I size = 8 * 1024; size <= 8 * MiB; size *= 2) {
        add(bytes / size); add((bytes + size - 1) / size);
    }
    return out;
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
        bool cache_admitted = false;
    };
    std::map<nds::Iocb::CacheKey, History> history_;
    std::deque<std::pair<nds::Iocb::CacheKey, I>> history_queue_;
    I byte_clock_ = 0, generation_ = 0;
    std::map<std::pair<I, I>, I> arrivals_;
    struct Work {
        double read_us = 0, copy_us = 0, commands = 0, card_share = 1;
        double batch_us = 0, submit_gap_us = 0;
        int hit_q = 1;
    };
    std::map<std::pair<I, I>, Work> work_;
    std::map<std::vector<I>, I> batch_costs_;
    struct Promotion { nds::Iocb::CacheKey key; I bytes; };
    std::map<I, Promotion> promotions_;
    std::map<nds::Iocb::CacheKey, I> pending_keys_;
    I last_submit_arrival_ = -1, submit_gap_us_ = 0;
    struct ObservedBatch { nds::Submit submit; Json decision; };
    std::deque<ObservedBatch> recent_;
    double cost(const char* key) const { return double(c_.at(key)); }
    I pool(const char* name) const { return r_.at(name) * MiB; }
    void sync_promotions(const Json& state) {
        std::set<I> live;
        for (const auto& p : state.at("promote_inflight").as_array())
            live.insert(p.at("promote_id").as_int());
        for (auto it = promotions_.begin(); it != promotions_.end();) {
            if (live.count(it->first)) ++it;
            else {
                auto known = history_.find(it->second.key);
                if (known != history_.end()) known->second.cache_admitted = true;
                pending_keys_.erase(it->second.key);
                it = promotions_.erase(it);
            }
        }
    }
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
    // Count transfers, not operations. Copy resources are scoped by type AND card.
    // These static-q work values are ranking proxies, never latency upper bounds.
    std::vector<Work> plan_work(const Json& plan, const Json& state) const {
        int qs = 0;
        std::map<std::pair<std::string, I>, int> copies;
        auto count = [&](const Json& t) {
            auto type = t.at("type").as_string();
            if (ssd_type(type)) ++qs;
            else ++copies[{type, t.at("device_id").as_int()}];
        };
        for (const auto& t : state.at("transfers_inflight").as_array()) count(t);
        for (const auto& op : plan.at("operations").as_array())
            for (const auto& t : op.at("transfers").as_array()) count(t);
        std::vector<Work> out;
        for (const auto& op : plan.at("operations").as_array()) {
            Work w;
            bool fallback = op.at("path").as_string() == "FALLBACK";
            w.commands = cost(fallback ? "CPU_FALLBACK_FIXED_COST" : "CPU_NDS_FIXED_COST");
            for (const auto& t : op.at("transfers").as_array()) {
                auto type = t.at("type").as_string();
                if (ssd_type(type)) {
                    w.read_us += transfer_us(t.at("physical_length").as_int(), r_.at("nvme_bw"),
                        fallback ? ddr_profile : direct_profile, std::max(1, qs));
                    w.commands += cost(fallback ? "CPU_FALLBACK_PER_CMD_COST" : "CPU_NDS_PER_CMD_COST");
                } else {
                    I bytes = t.at("bytes").as_int();
                    int q = std::max(1, std::min<int>(copies[{type, t.at("device_id").as_int()}],
                        int(r_.at(type == "D2D" ? "hbm_copy_concurrency" : "h2d_copy_concurrency"))));
                    if (type == "D2D") {
                        double bw = bytes < 4096 ? 20.0 * q : bytes < 32768 ? 130.0 * q :
                            bucket(d2d_profile, bytes, q);
                        w.copy_us += double(bytes) * 1000000.0 / (std::min(bw, double(r_.at("d2d_bw"))) * MiB);
                        w.commands += cost("CPU_D2D_CALL_COST") + cost("NPU_D2D_CALL_COST");
                    } else {
                        w.copy_us += transfer_us(bytes, r_.at("h2d_bw"), copy_profile, q);
                        w.commands += cost("CPU_H2D_CALL_COST") + cost("NPU_H2D_CALL_COST");
                    }
                }
            }
            out.push_back(w);
        }
        return out;
    }
    std::optional<Json> split_reads(const Json& plan, I limit, I now, const Json& state) {
        std::size_t total = 0;
        bool changed = false;
        for (const auto& op : plan.at("operations").as_array())
            for (const auto& t : op.at("transfers").as_array()) {
                I count = t.at("type").as_string() == "DIRECT"
                    ? (t.at("physical_length").as_int() - 1) / limit + 1 : 1;
                if (count > I(PlanTransfers - total)) return std::nullopt;
                total += std::size_t(count); changed = changed || count > 1;
                // This transformation is only for our independent, ready SSD stages.
                if (ssd_type(t.at("type").as_string()) &&
                    (t.at("depends_on").size() || t.at("not_before_us").as_int() > now)) return std::nullopt;
            }
        if (!changed) return std::nullopt;
        Json candidate = plan;
        for (auto& op : candidate["operations"].as_array()) {
            if (op.at("path").as_string() != "DIRECT") continue;
            Json transfers = Json::array();
            for (const auto& t : op.at("transfers").as_array()) {
                if (!ssd_type(t.at("type").as_string())) continue;
                I sectors = t.at("physical_length").as_int() / 512;
                I count = (t.at("physical_length").as_int() - 1) / limit + 1;
                if (count < 1 || count > sectors) return std::nullopt;
                I offset = t.at("physical_ssd_offset").as_int();
                for (I k = 0; k < count; ++k) {
                    I bytes = (sectors / count + (k < sectors % count)) * 512;
                    Json part = t;
                    part["transfer_id"] = next_transfer_++;
                    part["physical_ssd_offset"] = offset; part["physical_length"] = bytes;
                    transfers.push_back(std::move(part));
                    offset += bytes;
                }
            }
            op["transfers"] = std::move(transfers);
        }
        // Do not split an already efficient large read merely to raise other
        // reads' q. Its extra calls survive even if later traffic absorbs the gain.
        const auto original_work = plan_work(plan, state);
        for (int pass = 0; pass < 4; ++pass) {
            bool removed = false;
            auto proposed = plan_work(candidate, state);
            for (std::size_t i = 0; i < proposed.size(); ++i) {
                if (candidate.at("operations")[i].at("transfers").size() <=
                    plan.at("operations")[i].at("transfers").size()) continue;
                if (cost("time_cost_per_us") * proposed[i].read_us + proposed[i].commands >=
                    cost("time_cost_per_us") * original_work[i].read_us + original_work[i].commands) {
                    candidate["operations"][i] = plan.at("operations")[i]; removed = true;
                }
            }
            if (!removed) return candidate;
        }
        // A still-changing estimate is too uncertain for this bounded transform.
        return std::nullopt;
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
        sync_promotions(state);
        if (last_submit_arrival_ >= 0 && submit.arrival_us > last_submit_arrival_)
            submit_gap_us_ = submit.arrival_us - last_submit_arrival_;
        last_submit_arrival_ = submit.arrival_us;
        for (auto it = recent_.begin(); it != recent_.end();) {
            bool pending = std::any_of(it->submit.iocbs.begin(), it->submit.iocbs.end(),
                [&](const nds::Iocb& io) { return arrivals_.count({io.submit_id, io.iocb_id}) != 0; });
            if (!pending) it = recent_.erase(it); else ++it;
        }
        std::vector<IovIndex> indexes;
        indexes.reserve(submit.iocbs.size());  // Piece pointers must remain stable.
        std::map<I, int> per_card;
        I max_pool = std::max(pool("hbm_capacity"), pool("ddr_capacity"));
        for (const auto& io : submit.iocbs) {
            indexes.emplace_back(io);
            arrivals_[{io.submit_id, io.iocb_id}] = submit.arrival_us;
            auto& h = history_[io.cache_key()];
            // on_submit contains only actual misses. Once an earlier promotion
            // has finished, a new miss is authoritative evidence of absence.
            // Misses that arrived during that promotion cannot justify another.
            if (!pending_keys_.count(io.cache_key())) h.cache_admitted = false;
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
        std::size_t count_budget = CountTransitions;
        auto build_decision = [&](I block_limit, bool align_cuts = true, bool use_counts = true) {
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
                        // Both cuts are legal: align the logical end to avoid rereading
                        // a sector, or preserve useful length to retain a bandwidth bucket.
                        // The pool bound above includes the leading alignment padding.
                        if (align_cuts && offset + length < io.length)
                            length -= (start + length) % 512;
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
            struct Edge { std::size_t end; I physical, valid, card; std::string path; };
            std::vector<std::vector<Edge>> edges(n);
            std::size_t edge_count = 0;
            bool count_search = use_counts && count_budget > 0 && n <= 16384 &&
                state.at("transfers_inflight").size() == 0 && state.at("queued_iocbs").size() == 0 &&
                state.at("promote_inflight").size() == 0 && state.at("foreground_iocb_inflight").as_int() == 0 &&
                arrivals_.size() == submit.iocbs.size();
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
                        if (path == "AGGREGATE" && r_.at("hbm_copy_concurrency") == 0) continue;
                        if (path == "FALLBACK" && r_.at("h2d_copy_concurrency") == 0) continue;
                        if (count_search) {
                            if (++edge_count <= CountStates * 2)
                                edges[i].push_back({j + 1, physical, valid, card, path});
                            else { count_search = false; edges.clear(); }
                        }
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
            if (count_search) {
                // Reachability is checked before allocating DP rows. In particular,
                // different cards, gaps, pools and slot ownership can force >1 group.
                const int unreachable = int(n) + 1;
                std::vector<int> prefix_min(n + 1, unreachable), suffix_min(n + 1, unreachable);
                prefix_min[0] = suffix_min[n] = 0;
                for (std::size_t i = 0; i < n; ++i)
                    for (const auto& e : edges[i])
                        prefix_min[e.end] = std::min(prefix_min[e.end], prefix_min[i] + 1);
                for (std::size_t i = n; i-- > 0;)
                    for (const auto& e : edges[i])
                        suffix_min[i] = std::min(suffix_min[i], suffix_min[e.end] + 1);
                std::map<std::pair<std::string, I>, int> copy_groups;
                for (const auto& [a, b, path] : plan) {
                    (void)b;
                    if (path != "DIRECT")
                        ++copy_groups[{path == "AGGREGATE" ? "D2D" : "H2D", pieces[a].io->device_id}];
                }
                const int seed = int(plan.size());
                I bytes = 0;
                for (const auto& p : pieces) bytes += p.length;
                auto samples = count_samples(int(n), active_ssd, bytes, seed);
                samples.insert(samples.begin(), prefix_min[n]);
                double best = std::numeric_limits<double>::infinity();
                int best_count = seed;
                std::set<int> tried;
                struct Cell { double value = std::numeric_limits<double>::infinity();
                              std::size_t previous = 0, edge = 0; };
                struct Row { std::size_t begin = 0; std::vector<Cell> cells;
                    Cell* get(std::size_t i) {
                        return i >= begin && i - begin < cells.size() ? &cells[i - begin] : nullptr;
                    }
                };
                auto solve_count = [&](int cnt) {
                    if (cnt < prefix_min[n] || cnt > int(n) || !tried.insert(cnt).second || !count_budget)
                        return;
                    std::vector<Row> rows(std::size_t(cnt) + 1);
                    std::size_t states = 0;
                    for (int h = 0; h <= cnt; ++h) {
                        std::size_t lo = std::max<I>(h, I(n) - I(cnt - h) * Window);
                        std::size_t hi = std::min<std::size_t>(std::size_t(h) * Window, n - (cnt - h));
                        while (lo <= hi && (prefix_min[lo] > h || suffix_min[lo] > cnt - h)) ++lo;
                        while (lo <= hi && (prefix_min[hi] > h || suffix_min[hi] > cnt - h)) --hi;
                        if (lo > hi || states + hi - lo + 1 > CountStates) return;
                        states += hi - lo + 1;
                        rows[h].begin = lo; rows[h].cells.resize(hi - lo + 1);
                    }
                    // Preflight both storage and work: sampling alone is not a bound.
                    std::size_t transitions = edge_count;
                    for (int h = 0; h < cnt; ++h)
                        for (std::size_t k = 0; k < rows[h].cells.size(); ++k) {
                            auto i = rows[h].begin + k;
                            if (prefix_min[i] <= h && suffix_min[i] <= cnt - h)
                                transitions += edges[i].size();
                        }
                    if (transitions > count_budget) return;
                    count_budget -= transitions;
                    std::vector<std::vector<double>> weights(n);
                    for (std::size_t i = 0; i < n; ++i) for (const auto& e : edges[i]) {
                        auto type = e.path == "AGGREGATE" ? "D2D" : "H2D";
                        int qc = active_copy[{type, e.card}] +
                            std::max(1, (cnt * copy_groups[{type, e.card}] + seed - 1) / seed);
                        weights[i].push_back(estimate(e.path, e.physical, e.valid,
                            std::max(1, active_ssd + cnt), qc, state));
                    }
                    rows[0].get(0)->value = 0;
                    for (int h = 0; h < cnt; ++h)
                        for (std::size_t k = 0; k < rows[h].cells.size(); ++k) {
                            auto i = rows[h].begin + k;
                            double value = rows[h].cells[k].value;
                            if (!std::isfinite(value)) continue;
                            for (std::size_t e = 0; e < edges[i].size(); ++e) {
                                auto j = edges[i][e].end;
                                if (suffix_min[j] > cnt - h - 1) continue;
                                auto* dst = rows[h + 1].get(j);
                                if (dst && value + weights[i][e] < dst->value)
                                    *dst = {value + weights[i][e], i, e};
                            }
                        }
                    auto* last = rows[cnt].get(n);
                    if (!last || !std::isfinite(last->value) || last->value >= best) return;
                    decltype(plan) proposed;
                    std::set<I> nds_owners;
                    I hbm = 0, ddr = 0;
                    for (std::size_t end = n, h = cnt; h > 0; --h) {
                        const auto& cell = *rows[h].get(end);
                        const auto& edge = edges[cell.previous][cell.edge];
                        proposed.emplace_back(cell.previous, end, edge.path);
                        if (edge.path == "AGGREGATE") hbm += edge.physical;
                        if (edge.path == "FALLBACK") ddr += edge.physical;
                        if (edge.path != "FALLBACK")
                            for (std::size_t i = cell.previous; i < end; ++i)
                                nds_owners.insert(pieces[i].io->iocb_id);
                        end = cell.previous;
                    }
                    // cnt represents simultaneous initial SSD starts only if all
                    // owners and buffers fit. This does NOT keep q fixed to the end.
                    // A rejected candidate may still be legal as a queued plan;
                    // the ordinary planner remains available for that case.
                    if (nds_owners.size() > std::size_t(r_.at("iocb_capacity")) ||
                        hbm + state.at("aggregate_hbm_used").as_int() > pool("hbm_capacity") ||
                        ddr + state.at("fallback_ddr_used").as_int() > pool("ddr_capacity")) return;
                    best = last->value; best_count = cnt; plan = std::move(proposed);
                };
                for (int cnt : samples) solve_count(cnt);
                const int center = best_count, delta = std::max(1, int(std::ceil(center * 0.05)));
                for (int d : {-1, 1, -2, 2, -delta, delta}) solve_count(center + d);
            }
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
        Json decision = build_decision(2 * MiB, true, false);
        std::size_t iov_count = 0;
        for (const auto& io : submit.iocbs) iov_count += io.iovs.size();
        // Two cut styles, paths and bounded count/split candidates on an observed
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
                    if (r_.at(forced == "AGGREGATE" ? "hbm_copy_concurrency" : "h2d_copy_concurrency") == 0) continue;
                    if (op.at("buffer_bytes").as_int() > pool(forced == "AGGREGATE" ? "hbm_capacity" : "ddr_capacity")) continue;
                    op["path"] = forced;
                    for (auto& t : op["transfers"].as_array())
                        t["type"] = ssd_type(t.at("type").as_string())
                            ? (forced == "AGGREGATE" ? "SSD_TO_HBM_BUFFER" : "SSD_TO_DDR")
                            : (forced == "AGGREGATE" ? "D2D" : "H2D");
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
                    key.push_back(I(transfers.size()));
                    std::map<I, I> transfer_order;
                    for (const auto& t : transfers.as_array())
                        transfer_order[t.at("transfer_id").as_int()] = I(transfer_order.size());
                    for (const auto& t : transfers.as_array()) {
                        auto type = t.at("type").as_string();
                        key.push_back(ssd_type(type) ? 0 : type == "D2D" ? 1 : 2);
                        key.push_back(t.at(ssd_type(type) ? "physical_length" : "bytes").as_int());
                        key.push_back(t.at("not_before_us").as_int() - now);
                        key.push_back(t.at("priority").as_int());
                        key.push_back(I(t.at("depends_on").size()));
                        for (const auto& dep : t.at("depends_on").as_array())
                            key.push_back(transfer_order.at(dep.as_int()));
                    }
                    key.push_back(I(op.at("ranges").size()));
                    for (const auto& range : op.at("ranges").as_array()) {
                        key.push_back(ordinal.at(range.at("iocb_id").as_int()));
                        key.push_back(range.at("iocb_offset").as_int());
                        key.push_back(range.at("length").as_int());
                    }
                }
                auto cached = batch_costs_.find(key);
                if (prefix.empty() && cached != batch_costs_.end()) return cached->second;
                // Counterfactual path-family comparison on observed inputs only;
                // this is a proxy, and never changes previously issued decisions.
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
            const Json initial = decision;
            auto compare = [&](const Json& candidate, const std::string& mode = "") {
                I value = evaluate(candidate, mode);
                if (value < best) { best = value; decision = candidate; }
            };
            for (I block : {2 * MiB, 8 * MiB}) {
                for (bool align_cuts : {true, false}) {
                    Json base = block == 2 * MiB && align_cuts
                        ? initial : build_decision(block, align_cuts);
                    if (block == 2 * MiB && align_cuts) compare(build_decision(block, align_cuts));
                    for (const std::string forced : {"", "AGGREGATE", "FALLBACK"}) {
                        Json candidate = base;
                        rewrite_path(candidate, forced);
                        compare(candidate, forced);
                        // Rank cheap split proposals, then simulate at most two.
                        // Limits come from size buckets, never trace names or seeds.
                        std::vector<std::pair<double, Json>> splits;
                        for (I limit = 8 * 1024; limit <= 4 * MiB; limit *= 2) {
                            auto split = split_reads(candidate, limit, now, state);
                            if (!split) continue;
                            auto work = plan_work(*split, state);
                            double read = 0, calls = 0;
                            std::map<I, double> copy;
                            for (std::size_t i = 0; i < work.size(); ++i) {
                                read += work[i].read_us; calls += work[i].commands;
                                copy[split->at("operations")[i].at("transfers")[0].at("device_id").as_int()] += work[i].copy_us;
                            }
                            double longest = 0;
                            for (const auto& [card, t] : copy) { (void)card; longest = std::max(longest, t); }
                            double value = cost("time_cost_per_us") * (read + longest) + calls;
                            splits.emplace_back(value, std::move(*split));
                            // At most three entries: stable in-place insertion avoids
                            // nothrow temporary allocations outside the SDK meter.
                            for (std::size_t k = splits.size() - 1;
                                 k > 0 && splits[k].first < splits[k - 1].first; --k)
                                std::swap(splits[k], splits[k - 1]);
                            if (splits.size() > 2) splits.pop_back();
                        }
                        for (const auto& entry : splits) compare(entry.second, forced);
                    }
                }
            }
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
        I total_bytes = 0;
        for (const auto& io : submit.iocbs) {
            card_bytes[io.device_id] += io.length;
            total_bytes += io.length;
        }
        const auto selected_work = plan_work(decision, state);
        std::size_t op_index = 0;
        double batch_read_us = 0;
        std::map<I, double> batch_copy_us;
        for (const auto& op : decision.at("operations").as_array()) {
            const auto& first = op.at("transfers")[0];
            I card = first.at("device_id").as_int();
            I bytes = 0;
            for (const auto& range : op.at("ranges").as_array()) bytes += range.at("length").as_int();
            const auto& selected = selected_work[op_index++];
            double read = selected.read_us, copy = selected.copy_us, commands = selected.commands;
            batch_read_us += read;
            batch_copy_us[card] += copy;
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
        double longest_copy_us = 0;
        for (const auto& [card, us] : batch_copy_us) {
            (void)card;
            longest_copy_us = std::max(longest_copy_us, us);
        }
        for (const auto& io : submit.iocbs) {
            auto& w = work_.at({submit.submit_id, io.iocb_id});
            w.batch_us = batch_read_us + longest_copy_us;
            w.submit_gap_us = double(submit_gap_us_);
        }
        return decision;
    }
    std::optional<Json> on_iocb_complete(const nds::Iocb& io, const Json& state) override {
        I now = state.at("current_time_us").as_int();
        sync_promotions(state);
        auto owner = std::make_pair(io.submit_id, io.iocb_id);
        auto arrival = arrivals_.find(owner);
        auto work = work_.find(owner);
        Work served;
        if (work != work_.end()) { served = work->second; work_.erase(work); }
        // Cache hits have no foreground work owned by this policy.
        bool foreground = arrival != arrivals_.end();
        I arrived_at = foreground ? arrival->second : now;
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
        if (h.cache_admitted || pending_keys_.count(io.cache_key())) return std::nullopt;
        if (h.seen < 2 || h.gap <= 0 || io.length > pool("kvcache_pool_capacity")) return std::nullopt;
        if ((h.gap >= cost("promote_ttl_us") || cost("promote_ttl_us") <= 0))
            return std::nullopt;
        // Arrival evidence can become stale while foreground IO waits in a queue.
        // This conservative admission rule uses only already observed demand.
        if ((now - h.last_arrival >= cost("promote_ttl_us") ||
             byte_clock_ - h.last_byte > pool("kvcache_pool_capacity")))
            return std::nullopt;
        if (now - h.last_arrival > std::max(1.0, h.gap + 2.0 * h.jitter)) return std::nullopt;
        int pending_hbm = 0, pending_ssd = 0;
        double pending_bytes = 0;
        for (const auto& p : state.at("promote_inflight").as_array()) {
            if (p.at("promote_id").as_int() == h.promote_id) return std::nullopt;
            auto known = promotions_.find(p.at("promote_id").as_int());
            if (known != promotions_.end()) pending_bytes += double(known->second.bytes);
            if (p.at("path").as_string() == "PROMOTE_HBM") {
                if (known != promotions_.end() && std::get<0>(known->second.key) == io.device_id)
                    ++pending_hbm;
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
        // An observed idle gap can absorb saved service time without reducing
        // TraceTime. Keep command savings even in that arrival-limited regime.
        // This extrapolates the last visible gap; future arrivals remain unknown.
        double span = std::max(served.batch_us, double(now - arrived_at));
        double time_weight = served.submit_gap_us <= 0 || span >= served.submit_gap_us ? 1.0 : 0.0;
        // Global SSD work and per-card copy work have different parallel scope.
        // This remains a marginal-work estimate, not a global optimality claim.
        double gain = time_weight * cost("time_cost_per_us") *
                          (served.read_us + (served.copy_us - hit) * served.card_share) +
                      served.commands - cost("CPU_H2D_CALL_COST") - cost("NPU_H2D_CALL_COST");
        // Fund promotion from estimated work of already observed reads. Reuse
        // must still be fresh and each future hit must have positive net value.
        if (h.observed > h.accounted) {
            // Budget measures already observed service expenditure, whereas
            // gain above measures prospective benefit. Keeping them separate
            // lets sparse but expensive non-direct reads fund useful caching.
            double spent = cost("time_cost_per_us") *
                (served.read_us + served.copy_us * served.card_share) + served.commands;
            h.credit += (h.observed - h.accounted) * std::max(0.0, spent);
            h.accounted = h.observed;
        }
        // Replacing a larger fraction of the pool requires proportionally more
        // evidence, but never permanently pins the current residents in place.
        double capacity = double(pool("kvcache_pool_capacity"));
        double reclaim = std::max(0.0, double(state.at("kvcache_pool_used").as_int()) +
                                      pending_bytes + double(io.length) - capacity);
        double admission_cost = promote_cost * (1.0 + reclaim / capacity);
        if (gain <= 0 || h.credit < admission_cost) return std::nullopt;
        h.credit = 0;
        h.last_gain = gain;
        h.paid_cost = promote_cost;
        I promote = next_promote_++;
        h.promote_id = promote;
        h.cache_admitted = true;
        promotions_.emplace(promote, Promotion{io.cache_key(), io.length});
        pending_keys_[io.cache_key()] = promote;
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
