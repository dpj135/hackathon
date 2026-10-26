#include "nds/player_api.hpp"

#include <memory>

// 对照策略：每个 iov 单独走 FALLBACK，不做跨 iocb 聚合，也不 PROMOTE。
// 对齐请求本来可以走 DIRECT，所以这份策略合法，但 J 应该高于参考贪心。
// 用法见 README「对照策略」。不要用它替换 solution.cpp 作为正式提交。
namespace {

using Json = nds::Json;

class AlwaysFallback final : public nds::SchedulerPolicy {
public:
    Json on_submit(const nds::Submit& submit, const Json& runtime_state) override {
        Json operations = Json::array();
        const std::int64_t now = runtime_state.at("current_time_us").as_int();
        for (const nds::Iocb& iocb : submit.iocbs) {
            std::int64_t cursor = 0;
            for (const nds::Iov& iov : iocb.iovs) {
                const auto [physical_offset, physical_length] =
                    nds::physical_span(iocb.ssd_offset + cursor, iov.length);
                const std::int64_t ssd_id = next_transfer_++;
                const std::int64_t copy_id = next_transfer_++;
                const std::int64_t group = next_group_++;

                Json ranges = Json::array();
                ranges.push_back(Json::object());
                ranges[0]["iocb_id"] = iocb.iocb_id;
                ranges[0]["iocb_offset"] = cursor;
                ranges[0]["length"] = iov.length;

                Json vectors = Json::array();
                vectors.push_back(Json::object());
                vectors[0]["iocb_id"] = iocb.iocb_id;
                vectors[0]["iov_id"] = iov.iov_id;
                vectors[0]["iov_offset"] = static_cast<std::int64_t>(0);
                vectors[0]["length"] = iov.length;

                Json depends = Json::array();
                depends.push_back(Json(ssd_id));

                Json transfers = Json::array();
                transfers.push_back(Json::object());
                transfers[0]["transfer_id"] = ssd_id;
                transfers[0]["type"] = "SSD_TO_DDR";
                transfers[0]["device_id"] = iocb.device_id;
                transfers[0]["physical_ssd_offset"] = physical_offset;
                transfers[0]["physical_length"] = physical_length;
                transfers[0]["group_id"] = group;
                transfers[0]["not_before_us"] = now;
                transfers[0]["depends_on"] = Json::array();
                transfers[0]["priority"] = 100;

                transfers.push_back(Json::object());
                transfers[1]["transfer_id"] = copy_id;
                transfers[1]["type"] = "H2D";
                transfers[1]["device_id"] = iocb.device_id;
                transfers[1]["bytes"] = iov.length;
                transfers[1]["vectors"] = std::move(vectors);
                transfers[1]["group_id"] = group;
                transfers[1]["not_before_us"] = now;
                transfers[1]["depends_on"] = std::move(depends);
                transfers[1]["priority"] = 100;

                Json operation = Json::object();
                operation["operation_id"] = next_operation_++;
                operation["path"] = "FALLBACK";
                operation["ranges"] = std::move(ranges);
                operation["buffer_bytes"] = physical_length;
                operation["transfers"] = std::move(transfers);
                operations.push_back(std::move(operation));
                cursor += iov.length;
            }
        }
        Json decision = Json::object();
        decision["event"] = "SCHEDULE_DECISION";
        decision["submit_id"] = submit.submit_id;
        decision["operations"] = std::move(operations);
        decision["next_wakeup_us"] = Json(nullptr);
        return decision;
    }

    std::optional<Json> on_iocb_complete(const nds::Iocb&, const Json&) override {
        return std::nullopt;
    }

private:
    std::int64_t next_operation_{1};
    std::int64_t next_transfer_{1};
    std::int64_t next_group_{1};
};

}  // namespace

std::unique_ptr<nds::SchedulerPolicy> nds::create_policy(
    const std::map<std::string, std::int64_t>&,
    const std::map<std::string, std::int64_t>&) {
    return std::make_unique<AlwaysFallback>();
}
