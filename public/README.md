# NDS 多路径 IO 调度选手包

赛题说明见 [茶思屋赛题页](https://www.chaspark.com/#/races/competitions/1326700933119291392)。这个目录是本地评测框架、公开 trace 和打分脚本。正式排名使用另一套隐藏 trace，不在这个包里。

## 算法要做什么

每个 trace 是一串已经排好到达时间的 `READV`。程序在请求到达时决定怎么把 SSD 上的数据读进 HBM，并在一次读完成后决定要不要把这块数据放进 KVCache。目标是让整条 trace 的成本 `J` 更小。

```text
J = time_cost_per_us * TraceTime + CPUCost + NPUCost
```

`TraceTime` 是最后一个请求完成的时刻减去第一个请求到达的时刻，单位是微秒。CPU 和 NPU 成本按操作次数计算，不按时间计算。常量在 `suites/frozen_costs.json`，每条 trace 里的同名数字必须和它对得上。

一次读可以走三条前台路径：

| 路径 | 什么时候能用 | 数据怎么走 |
|---|---|---|
| DIRECT | SSD 偏移、HBM 地址、长度都是 512B 对齐 | SSD 直接进 HBM |
| AGGREGATE | 对不齐，但读放大不超过 1.5，而且物理长度放得进 HBM 池 | SSD 先进入临时 HBM，再 D2D 到目的地址 |
| FALLBACK | 上面两条不合适，或者 HBM 临时池放不下 | SSD 先进入 DDR，再 H2D 到目的地址 |

同一张卡上、物理上连续的多段请求可以并进同一次传输。合并后的传输更大，往往落到更高的带宽档。逐条发送 512B 会停在最低档。

读完以后可以做两种提升，把整个 iocb 放进 KVCache：

| 路径 | 数据怎么走 |
|---|---|
| PROMOTE_HBM | 从 HBM 用 D2H 写入缓存。并发受 `d2h_copy_concurrency` 限制 |
| PROMOTE_SSD | 从 SSD 再读一遍写入缓存。它占用 SSD 带宽，没有单独的并发上限 |

缓存键是 `(device_id, ssd_offset, length)`。之后同一个键再出现时，框架自己用 H2D 完成，这次请求不会进入 `on_submit`。TTL 从提升完成时起算，命中只刷新 LRU，不延长 TTL。查找时如果这个键已经过期，视为未命中，但不立刻清掉。过期项要等下一次写入、池子不够时才丢掉，然后再按 LRU 驱逐。对象比 `kvcache_pool_capacity` 还大时，提升完不成缓存。

程序只能看到当前已经到达、而且没有命中缓存的请求，不能提前看后面的 trace。完成时间和 `J` 由模拟器计算。

## 改哪里

只改 `player/solution.cpp`。正式评测也只编译这一个文件，链接的是主办方的模拟器，不会使用你带来的 `src/framework.cpp`。

入口是：

```cpp
std::unique_ptr<nds::SchedulerPolicy> nds::create_policy(
    const std::map<std::string, std::int64_t>& resources,
    const std::map<std::string, std::int64_t>& fixed_parameters);
```

在这里返回你自己的 `nds::SchedulerPolicy`。两个回调是：

- `on_submit`：这一批未命中的 iocb 已经到达。返回一个 `SCHEDULE_DECISION`，里面写每段数据走哪条前台路径。
- `on_iocb_complete`：某个前台 iocb 读完了。返回一条 PROMOTE，或者返回空表示不提升。

策略比较长时，把头文件放在 `player/`，在 `solution.cpp` 里 `#include`。评测不会编译 `player/` 里的其他 `.cpp`。

现在这个文件返回 `GreedyBaseline`，实现在 `include/nds/framework.hpp` 和 `src/framework.cpp`。它是合法起点：对齐走 DIRECT，放得进 HBM 就 AGGREGATE，否则 FALLBACK；同一个键第二次未命中后做 PROMOTE_HBM；不跨 iocb 合并。公开集自动分是 **67.20/90**。参考成本更低，因为参考调度会合并连续区间，并在合适的时候改用 PROMOTE_SSD 或停止提升。

`include/`、`src/`、`data/`、`suites/`、`scripts/` 用来本地编译和打分。提交算法时不用改它们。

## 怎么用

需要 C++17。在这个目录下：

```bash
make -j
./nds_player data/small_mixed.json /tmp/result.json
./nds_validate data/small_mixed.json /tmp/result.json
make test
make score
```

`make -j` 编出下面几个程序：

| 程序 | 作用 |
|---|---|
| `nds_player` | 用 `player/solution.cpp` 跑一条 trace，把结果写到第二个参数，并把 `metrics` 打到终端。看 `total_cost`，那就是 `J` |
| `nds_validate` | 用模拟器回放刚才的结果。不合法会报错 |
| `nds_baseline` | 不看你的 `solution.cpp`，直接跑自带贪心。用来和自己的结果对比 |
| `nds_generate` | 按参数另造一条 trace。`./nds_generate --help` |
| `nds_tests` | 框架自测。`make test` 会跑它 |

改完 `solution.cpp` 后重新执行 `make -j nds_player`，再跑题。先用 `small_mixed.json` 这种小题看 `valid` 是不是 true，再用 `make score` 跑全部 39 道公开题。整套大约一两分钟。低档每题不到 1 秒，中档几秒，大档大约 10 秒。

`make score` 的结果写在终端，也写到 `last_public_score.json`。这个文件是本地跑分留下的，不用提交。不要运行 `bash scripts/score.sh --freeze`，它会把 `suites/public_baseline.json` 里的参考成本改成你当前的程序。

## 目录和文件

| 路径 | 作用 |
|---|---|
| `player/solution.cpp` | 你的算法。提交时交这个文件 |
| `player/simple_fallback.cpp` | 对照：每个 iov 都走 FALLBACK，不提升 |
| `player/no_promote.cpp` | 对照：路径和贪心相同，从不提升 |
| `player/promote_ssd_only.cpp` | 对照：第二次未命中一律 PROMOTE_SSD |
| `include/nds/player_api.hpp` | `create_policy` 的声明 |
| `include/nds/framework.hpp` | 请求、资源和 `SchedulerPolicy` 的定义，以及贪心类 |
| `include/nds/json.hpp` | 决策和 trace 用的 JSON |
| `src/framework.cpp` | 模拟器。带宽、缓存、合法性都在这里 |
| `src/player_main.cpp` | `nds_player` 的入口，负责读 trace、调用你的策略、写出结果 |
| `src/validate_main.cpp` | `nds_validate` 的入口 |
| `src/baseline_main.cpp` | `nds_baseline` 的入口 |
| `src/generate_main.cpp` | `nds_generate` 的入口 |
| `src/json.cpp` | JSON 实现 |
| `data/` | 39 条公开 trace。每道题考什么见后面的表 |
| `suites/public_suite.json` | 每道题的文件名、权重和墙钟限制 |
| `suites/public_baseline.json` | 每道题的参考 `J` |
| `suites/frozen_costs.json` | 冻结的成本常量 |
| `scripts/score.sh` | `make score` 调用的公开集打分 |
| `scripts/score_suite.py` | 打分实现：编译结果、校验、按权重算分 |
| `scripts/score_example.sh` | 给永远 FALLBACK 的对照打分 |
| `scripts/score_distinguish.sh` | 给从不提升的对照打分 |
| `scripts/score_paths.sh` | 检查读路径合并和两条 PROMOTE 路径的成本差 |
| `scripts/generate_public.sh` | 按固定参数重新生成公开 trace |
| `scripts/smoke_test_generator.sh` | 检查生成器对同一组参数写出的文件一致 |
| `tests/framework_test.cpp` | 用几条小 trace 回放模拟器 |
| `Makefile` `CMakeLists.txt` | 本地编译 |

## 三档数据量

题面把 trace 分成三档。公开集三档都有，权重加起来是 100。

| 档 | 约束 | 公开集里有哪些 | 规模 |
|---|---|---|---|
| 低 | 单次 submit ≤ 100 个 iocb，总数 ≤ 1000 | `small_*.json`、`hand_ttl_short.json`、`01` 到 `11`、`promote_low_*.json`、`read_*_low.json`、`promote_ssd_low.json`、`promote_hbm_low.json` | 行为题几十到几百个 iocb；命中题 768 个，单次 64 |
| 中 | 单次 submit ≤ 1024，总数 ≤ 20000 | `mid_*.json`、`promote_mid.json`、`read_*_mid.json`、`promote_ssd_mid.json`、`promote_hbm_mid.json` | 混合题每个 4096 个 iocb，单次 256；路径题单次 80 到 256 |
| 大 | 单次 submit ≤ 1024，总数 ≤ 100000 | `high_*.json`、`promote_high.json`、`read_*_high.json`、`promote_ssd_high.json`、`promote_hbm_high.json` | 混合题和路径题 20480 个 iocb，单次 1024；`promote_high` 的 KVCache 只有 8MB |

低数据量覆盖路径选择、聚合、TTL、缓冲、并发，以及大部分 iocb 都能命中缓存的情况。中数据量是混合、非对齐、热复用，再加一条命中占多数的题。大数据量看调度在两万条请求上还能不能在时限里跑完。`promote_high.json` 把 KVCache 限制在 8MB，每张卡轮转 256 个 1MB 对象，工作集约 2GB。参考贪心仍会提升见过两次的对象，池子装不下就按 LRU 驱逐，被赶走的对象下一次还要从 SSD 读。题面允许的最大规模是约 10 万个 iocb；公开评分集的大档取 2 万条，这样本地打分可以在几分钟内结束。想自己压测更大的 trace，用 `./nds_generate --help`。

带编号和 `mid_` / `high_` 的 trace 每个 submit 在 8 张卡上的 iocb 数相同。四份手写小 trace 是单卡或少量卡，用来看格式和缓存语义。

## 对照策略

`player/simple_fallback.cpp` 是一份故意简单的策略：每个 iov 单独走 FALLBACK，不跨 iocb 聚合，也不 PROMOTE。它应该在全部公开题上合法，自动分低于默认贪心。

不要把它当成提交文件。编译和打分：

```bash
bash scripts/score_example.sh
```

脚本会单独编译这份策略再跑公开集，并检查“全部合法且自动分低于 80”。正式提交请改 `player/solution.cpp`。

`player/no_promote.cpp` 路径选择和参考贪心相同，只是从不 PROMOTE。用它看打分能不能把“会提升并命中”和“只选路径”分开：

```bash
bash scripts/score_distinguish.sh
```

脚本要求这份策略在全部公开题上合法，自动分不超过 80。低档两条和中档那条命中题上，`J_reference / J_player` 低于 0.80，参考解的缓存命中不少于该题 iocb 数的 70%。大档 `promote_high` 上，参考解的命中低于 5%，LRU 驱逐超过 1000 次，而且不提升的 `J` 不高于无脑提升。

`player/promote_ssd_only.cpp` 把第二次未命中都走 PROMOTE_SSD。和默认的 PROMOTE_HBM 对照：

```bash
bash scripts/score_paths.sh
```

脚本检查三件事。`read_direct_*`、`read_aggregate_*`、`read_fallback_*` 上，参考调度的 `J` 不超过贪心的一半。`promote_hbm_*` 上，一律 PROMOTE_SSD 的成本至少是 PROMOTE_HBM 的 1.4 倍，而且命中更少。`promote_ssd_*` 上，参考调度比一律 PROMOTE_HBM 更便宜。

## 决策约束

模拟器会拒绝下面这些输出：

- iocb 的字节覆盖有空洞或重叠。
- DIRECT 的 SSD 偏移、HBM 地址或长度不是 512B 对齐。
- SSD 物理读没有盖住逻辑范围。DIRECT 盖住原始区间；AGGREGATE 和 FALLBACK 盖住向前向后扩到 512B 边界之后的区间。多读可以，少读不行。
- `buffer_bytes` 小于该操作中 SSD 阶段物理长度之和，或大于对应的 HBM/DDR 池。池子可以循环使用，但一个操作在结束前一直占用它申请的缓冲。
- 一个操作里的 iocb 不属于同一张卡，或传输缺少 `device_id`。
- AGGREGATE 把物理上不连续的区间拼进同一个操作。
- 复制向量没有正好盖住这些 range 对应的 iov 字节。
- PROMOTE_HBM 的 `bytes` 不等于整个 iocb 长度。
- PROMOTE_SSD 的物理区间不是对齐后的整段 iocb。
- 传输依赖有环，或 `not_before_us` 早于请求到达。PROMOTE 可以用 `not_before_us` 推迟启动，不能早于当前时刻。

KVCache 的键是整个 iocb 的 `(device_id, ssd_offset, length)`。命中由框架模拟成该卡上的 H2D，不进入 `on_submit`。TTL 从 PROMOTE 完成时开始，命中只更新 LRU，不延长 TTL。对象比 `kvcache_pool_capacity` 还大时，PROMOTE 完不成缓存。到达和完成时不清过期项；下一次写入发现池子不够，才先丢掉过期项，再按 LRU 驱逐。

运行状态里的池用量字段是 `kvcache_pool_used`。trace 里如果还写着旧名 `mooncake_capacity`，和 `kvcache_pool_capacity` 是同一个字段。

## 公开用例

低数据量：

| 文件 | 在考什么 |
|---|---|
| `small_aligned.json` `small_mixed.json` `small_repeated.json` | 手写小例子：对齐、混合、重复命中 |
| `hand_ttl_short.json` | TTL 只有 1000us，第三次相同请求在 8000us，缓存已经失效 |
| `01_aligned.json` … `05_mixed_size.json` | 八卡均衡的对齐、低/高非对齐、碎片、混合 size |
| `06_reuse_hot.json` `07_no_reuse.json` `08_ttl_expiry.json` | 热复用、无复用、短 TTL |
| `09_buffer_pressure.json` | HBM 池 1MB、DDR 池 4MB |
| `10_concurrency_pressure.json` | 较小的 iocb 容量和拷贝并发 |
| `11_merge_friendly.json` | 同卡上相邻的对齐区间，跨 iocb 聚合会比逐条 DIRECT 更划算 |

缓存命中占多数的三题：`promote_low_aligned.json`（768 个 iocb，约 75% 命中）、`promote_low_unaligned.json`（同样 768 个，非对齐，命中更高）、`promote_mid.json`（2560 个，约 80% 命中）。到达间隔刚好让 PROMOTE 在下一次 submit 前完成。

`promote_high.json` 是大档容量题：20480 个 iocb，每张卡固定轮转 256 个 1MB 对象，`kvcache_pool_capacity` 只有 8MB。工作集约 2GB，池子大约只能留下 8 个对象。默认贪心把每个对象都提升一次，多出来的按 LRU 驱逐，之后这些对象再次出现时走 SSD。这题权重 12。低档和中档的命中题权重分别是 4、2、4。

路径题每档各一份，用来把读路径和提升路径分开：

| 文件 | 在考什么 |
|---|---|
| `read_direct_{low,mid,high}.json` | 相邻 512B 对齐读。合并成一次大 DIRECT 会落到更高带宽档；逐条 512B 停在最低档 |
| `read_aggregate_{low,mid,high}.json` | SSD 和长度对齐，HBM 错 1 字节，所以不能 DIRECT。相邻 32KB 合并后走一次 AGGREGATE |
| `read_fallback_{low,mid,high}.json` | 相邻 4KB，HBM 同样错 1 字节。小块 D2D 很慢，合并后的 FALLBACK 更便宜 |
| `promote_ssd_{low,mid,high}.json` | 4KB 轮转，D2H 并发为 1。PROMOTE_SSD 比 PROMOTE_HBM 略便宜 |
| `promote_hbm_{low,mid,high}.json` | 1MB 轮转，D2H 并发为 1。PROMOTE_HBM 能在下一次提交前完成；PROMOTE_SSD 会错过窗口，成本约 1.6 到 2 倍 |

`scripts/generate_public.sh` 用 `--hbm-unaligned` 生成后两类读路径：SSD 偏移和长度保持 512B 对齐，只把 HBM 地址错开 1 字节。

中数据量：`mid_mixed.json` 混合长度，`mid_unaligned.json` 高比例非对齐，`mid_reuse.json` 热数据重复出现。

大数据量：`high_aligned.json` 全部 512B 对齐，`high_mixed.json` 混合长度。这两题单次 submit 都是 1024 个 iocb。

生成参数写在 `scripts/generate_public.sh`，也可以用 `nds_generate --help` 造自己的 trace。

## 分数

公开集权重在 `suites/public_suite.json`，参考成本在 `suites/public_baseline.json`，成本常量在 `suites/frozen_costs.json`。

```text
可行性   = 20 * 合法题数 / 题数
优化     = 加权平均(60 * min(1, J_reference / J_player))
性能     = 10 * 在时限和内存内合法跑完的题数 / 题数
自动分   = 以上三项，满分 90
```

单题默认墙钟限制写在 suite 里。设计文档的 10 分由评委看，脚本不打。

内存上限 16384MB 只算你的策略。`nds_player` 统计 `create_policy`、`on_submit`、`on_iocb_complete` 里经 `new` 或 STL 容器申请、还没释放的字节，以及你自己开的线程申请的字节，全局对象构造时申请的也算。框架读 trace、模拟、写结果和 `nds_validate` 不算。超过上限时 `nds_player` 打印 `NDS_POLICY_MEMORY_EXCEEDED` 并退出，这一题按超内存计 0 分。正常结束时它在标准错误里打印 `NDS_POLICY_PEAK_KB`，打分结果里对应 `policy_peak_mb`。本地想换上限，设环境变量 `NDS_POLICY_MEMORY_LIMIT_MB`。

不要在 `solution.cpp` 里替换全局 `operator new` 或 `operator delete`，链接会冲突。不要用 `malloc`、`mmap` 或超大的静态数组绕过统计，评审会查。

`J_reference / J_player` 大于 1 的部分不会把优化分抬过 60，但会加进输出里的 `tie_break_ratio_sum`。

## 提交

提交 `player/solution.cpp`，以及算法说明、构建方式和复现步骤。正式评测使用主办方的模拟器源码，不使用提交包里的 `src/framework.cpp`。
