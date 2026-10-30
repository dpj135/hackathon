// Semantic probe for the bundled engine; not the submitted scheduling policy.
#include "nds/framework.hpp"
#include <iostream>
using nds::Json;
using I = std::int64_t;
class Probe final : public nds::SchedulerPolicy {
    nds::GreedyBaseline greedy_;
    int submits_ = 0, completions_ = 0;
    static bool contains(const Json& state, I offset) {
        for (const auto& key : state.at("cached_ranges").as_array())
            if (key[0].as_int()==0 && key[1].as_int()==offset && key[2].as_int()==1048576) return true;
        return false;
    }
    static void require(bool yes, const char* detail) {
        if (!yes) throw nds::ValidationError(detail);
    }
public:
    explicit Probe(const std::map<std::string,I>& r) : greedy_(r,false) {}
    Json on_submit(const nds::Submit& s, const Json& state) override {
        ++submits_;
        require(s.iocbs.size()==1,"expired key must reach policy as a miss");
        if (s.submit_id==2 || s.submit_id==3)
            require(contains(state,0) && state.at("kvcache_pool_used").as_int()==1048576,
                    "expiry must not remove entries during arrival");
        if (s.submit_id==4)
            require(!contains(state,0) && contains(state,2097152) && state.at("kvcache_pool_used").as_int()==1048576,
                    "even non-full insertion must clear old expired entries");
        return greedy_.on_submit(s,state);
    }
    std::optional<Json> on_iocb_complete(const nds::Iocb& io, const Json& state) override {
        ++completions_;
        if (io.submit_id==2)
            require(contains(state,0) && state.at("kvcache_pool_used").as_int()==1048576,
                    "expiry must not remove entries during completion");
        if (io.submit_id!=1 && io.submit_id!=3) return std::nullopt;
        Json t=Json::object();t["transfer_id"]=1000+io.submit_id;t["device_id"]=0;
        t["type"]="D2H";t["bytes"]=io.length;t["vectors"]=1;t["priority"]=10;
        Json p=Json::object();p["promote_id"]=io.submit_id;
        p["not_before_us"]=state.at("current_time_us");
        p["source_submit_id"]=io.submit_id;p["source_iocb_id"]=io.iocb_id;
        p["path"]="PROMOTE_HBM";p["transfer"]=t;return p;
    }
    void verify() const {
        require(submits_==4 && completions_==4,"expired requests must not be filtered as cache hits");
    }
};
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    try {
        auto trace=nds::load_trace(argv[1]);Probe policy(trace.instance.resources);
        nds::Simulator simulator(std::move(trace),policy);auto result=simulator.run();policy.verify();
        nds::write_json_file(argv[2],result);
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
