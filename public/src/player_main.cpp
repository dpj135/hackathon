#include "nds/player_api.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <unistd.h>

// 内存上限只算选手策略持有的堆：create_policy、on_submit、on_iocb_complete
// 里经 new 或 STL 申请且尚未释放的字节，加上选手自己开的线程申请的字节。
// 框架读 trace、模拟、写结果不计。上限取 NDS_POLICY_MEMORY_LIMIT_MB，默认 16384。
namespace {

// Every block carries this header so delete knows its size and owner without a lookup.
struct alignas(std::max_align_t) Header {
    std::size_t size;
    std::size_t offset;
};
static_assert(sizeof(Header) % alignof(std::max_align_t) == 0, "header must keep alignment");

constexpr std::size_t kCounted = std::size_t{1} << (sizeof(std::size_t) * 8 - 1);

std::atomic<std::size_t> current_bytes{0};
std::atomic<std::size_t> peak_bytes{0};
std::atomic<std::size_t> limit_bytes{0};

// Threads start counted, so allocations from contestant threads are charged.
// main() turns counting off for the framework thread.
thread_local bool counting = true;

class Counting {
public:
    Counting() : previous_(counting) { counting = true; }
    ~Counting() { counting = previous_; }
    Counting(const Counting&) = delete;
    Counting& operator=(const Counting&) = delete;

private:
    bool previous_;
};

[[noreturn]] void over_limit(std::size_t bytes, std::size_t limit) {
    char text[160];
    const int length = std::snprintf(text, sizeof text,
        "NDS_POLICY_MEMORY_EXCEEDED policy holds %zu MB, limit %zu MB\n", bytes >> 20, limit >> 20);
    if (length > 0) (void)!write(2, text, static_cast<std::size_t>(length));
    std::_Exit(3);
}

void charge(std::size_t size) {
    const std::size_t now = current_bytes.fetch_add(size, std::memory_order_relaxed) + size;
    std::size_t seen = peak_bytes.load(std::memory_order_relaxed);
    while (now > seen && !peak_bytes.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
    }
    const std::size_t limit = limit_bytes.load(std::memory_order_relaxed);
    if (limit && now > limit) over_limit(now, limit);
}

void* allocate(std::size_t size, std::size_t alignment) {
    if (size == 0) size = 1;
    if (alignment < alignof(Header)) alignment = alignof(Header);
    const std::size_t extra = sizeof(Header) + (alignment > alignof(Header) ? alignment : 0);
    if (size > SIZE_MAX - extra) throw std::bad_alloc();
    auto* base = static_cast<unsigned char*>(std::malloc(size + extra));
    if (!base) throw std::bad_alloc();
    std::uintptr_t user = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
    user = (user + alignment - 1) / alignment * alignment;
    Header* header = reinterpret_cast<Header*>(user) - 1;
    header->offset = user - reinterpret_cast<std::uintptr_t>(base);
    header->size = size | (counting ? kCounted : 0);
    if (counting) charge(size);
    return reinterpret_cast<void*>(user);
}

void release(void* memory) noexcept {
    if (!memory) return;
    const Header* header = static_cast<const Header*>(memory) - 1;
    if (header->size & kCounted) {
        current_bytes.fetch_sub(header->size & ~kCounted, std::memory_order_relaxed);
    }
    std::free(static_cast<unsigned char*>(memory) - header->offset);
}

class MeteredPolicy final : public nds::SchedulerPolicy {
public:
    explicit MeteredPolicy(nds::SchedulerPolicy& inner) : inner_(inner) {}

    nds::Json on_submit(const nds::Submit& submit, const nds::Json& runtime_state) override {
        Counting scope;
        return inner_.on_submit(submit, runtime_state);
    }

    std::optional<nds::Json> on_iocb_complete(const nds::Iocb& iocb,
                                              const nds::Json& runtime_state) override {
        Counting scope;
        return inner_.on_iocb_complete(iocb, runtime_state);
    }

private:
    nds::SchedulerPolicy& inner_;
};

}  // namespace

void* operator new(std::size_t size) { return allocate(size, 0); }
void* operator new[](std::size_t size) { return allocate(size, 0); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* memory) noexcept { release(memory); }
void operator delete[](void* memory) noexcept { release(memory); }
void operator delete(void* memory, std::size_t) noexcept { release(memory); }
void operator delete[](void* memory, std::size_t) noexcept { release(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { release(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { release(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { release(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { release(memory); }

int main(int argc, char** argv) {
    counting = false;
    if (argc != 3) {
        std::cerr << "usage: nds_player TRACE OUTPUT\n";
        return 2;
    }
    std::size_t limit_mb = 16384;
    if (const char* text = std::getenv("NDS_POLICY_MEMORY_LIMIT_MB")) {
        limit_mb = static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
    }
    limit_bytes.store(limit_mb << 20, std::memory_order_relaxed);
    const std::size_t held = current_bytes.load(std::memory_order_relaxed);
    if (limit_mb && held > (limit_mb << 20)) over_limit(held, limit_mb << 20);
    try {
        nds::Trace trace = nds::load_trace(argv[1]);
        std::unique_ptr<nds::SchedulerPolicy> policy;
        {
            Counting scope;
            policy = nds::create_policy(trace.instance.resources, trace.instance.fixed_parameters);
        }
        if (!policy) {
            std::cerr << "error: create_policy returned null\n";
            return 1;
        }
        MeteredPolicy metered(*policy);
        nds::Simulator simulator(std::move(trace), metered);
        nds::Json result = simulator.run();
        nds::write_json_file(argv[2], result);
        std::cout << result.at("metrics").dump(2) << '\n';
        std::cerr << "NDS_POLICY_PEAK_KB " << peak_bytes.load(std::memory_order_relaxed) / 1024 << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
