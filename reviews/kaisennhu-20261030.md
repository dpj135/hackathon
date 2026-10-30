# kaisennhu 提交独立评审（2026-10-30）

评审对象：`submissions/kaisennhu/03_程序/player/solution.cpp`（687 行，SHA256 `6a30c42d…`）
运行环境：`hackthon` 容器（openEuler 24.03），官方 `public/` 框架与 `public/data` 39 条公开 trace
评审范围：正确性、运行约束、边界条件（含 1 MB KVCache）、隐藏集规模（10 万 iocb）
本 PR 仅新增评审材料，**未改动 `submissions/kaisennhu/` 下的任何文件**。

## 0. 结论摘要

- **正确性：通过。** 39/39 公开 trace `nds_validate` 通过；1 MB 池、1 字节读、2048 iov、1000 iocb/批、10 万 iocb、非对齐切分全部合法；ASan/UBSan 无报错；策略无进程/线程/文件/网络调用。
- **公开成绩：89.9792/90**（feasibility 20、optimization 59.9792、performance 10、tie-break 58.003333），与提交 README 一致。
- **运行约束：满足。** 公开集单次 `on_submit` 最大 1.008 s（限 60 s）；峰值 RSS 286 MB（限 16 GB）；单线程、无子进程。
- **主要风险在 promotion 准入，而非输出合法性：**
  1. 到达受限（makespan 由提交间隔决定）时，promotion 不缩短 TraceTime，但固定 CPU 成本 80/次照付，策略仍大量提升。`threesight_20k`：20 000 次 promotion、0 命中，J 比 no-promote 高 27%。
  2. 准入不感知池占用/淘汰：1 MB/8 MB 池长周期用例出现 33–49 次淘汰，J 比 no-promote 差 656–2788。
  3. `cached_ranges` 每次 `on_iocb_complete` 全量扫描，缓存条目多时 O(n²)。
  4. `card_share` 缩放是对“SSD 全局瓶颈 + H2D 按卡并行”的调参补偿，不是可单侧删除的笔误；直接改公式会掉到 88.71/89.43。
- **已验证低风险补丁 V3b**（池占用 + inflight 守卫，见 `reviews/patches/kaisennhu-v3b-occupancy-guard.diff`）：公开 **89.99/90**，小池淘汰 33–49→0，promotion 34–57→1–8，10 万 iocb 结果不变。

## 1. 公开集与运行约束

| 指标 | 实测 | 赛题限制 | 结论 |
|---|---|---|---|
| 公开 trace 合法性 | 39/39 | 全部合法 | 通过 |
| 单次决策最大耗时 | 1.008 s（`read_fallback_high`） | 60 s | 通过 |
| 次大决策耗时 | 1.004 s（`read_aggregate_high`）、0.601 s（`read_direct_high`） | 60 s | 通过 |
| 进程峰值 RSS（公开集） | 237–286 MB | 16 384 MB | 通过 |
| 线程/子进程 | 单线程；无 `fork/exec/popen/system` | 8 CPU / 32 进程 | 通过 |
| 事件步数 | 10 万 iocb ≈ 10 万 op | 2 000 000 步 | 通过 |
| 公开成绩 | 89.9792/90 | — | 与 README 一致 |

### 10 万 iocb（隐藏集上限）

| 用例 | J | 墙钟 | 峰值 RSS | 命中 / promotion | 对照 |
|---|---:|---:|---:|---:|---|
| 100×1024×4 KiB 不复用 | 297 806 | 4.4 s | 1158 MB | 0 / 0 | baseline 同 J，1072 MB |
| 100×1024×4 KiB，2048 key 循环 | 297 806 | 3.9 s | 1143 MB | 0 / 0 | 4 KiB 下固定成本 80 压过收益，策略正确地不提升 |
| 100×1024×1 MiB，256 key/卡循环 | 1 490 312 | 2.6 s | 434 MB | 75 568 / 1 024 | baseline 1 536 536；no-promote 4 966 705 |
| 100×1024×4 MiB，64 key/卡循环 | 9 891 928 | 4.7 s | 787 MB | 27 082 / 256 | baseline 19 115 832；no-promote 19 252 420 |

> 4 KiB/10 万 iocb 的 1.16 GB 峰值中，框架自身基线为 1.07 GB（同一 trace 跑 `nds_baseline`），策略增量约 86 MB。团队文档的“峰值 <500 MB”在 10 万 iocb 下无法只靠策略达成。

## 2. 边界条件

| 用例 | 结果 |
|---|---|
| KVCache = 1 MB（合法最小值） | 合法输出；大于池的对象不 promotion；1 MiB 对象最多驻留 1 个 |
| KVCache = 0 / 负数 | 框架 `parse_trace` 拒绝：`resources.kvcache_pool_capacity must be positive`，策略不会运行 |
| hbm = ddr = 1 MB，8 MiB 非对齐读 | 合法切分；官方 baseline 报 `operation buffer exceeds pool` |
| 1 字节读、1000/2048 iov | 合法，无崩溃 |
| 1024×8 MiB 读 + 1 MB 池（8192 片） | 策略 0.059 s，仿真器约 55 s |
| 1024×64 MiB 读 + 1 MB 池（65536 片） | 策略 0.06 s，仿真器 >300 s（引擎上限，非策略决策超时） |
| ASan/UBSan（11 个用例） | 无报错 |

## 3. 发现的问题

### 3.1 P0：到达受限时 promotion 系统性过提升

`threesight_20k`（20 000 个 1 MiB key，每个恰好出现 3 次，提交间隔 100 ms，单批工作约 48 ms，无排队）：

| | promotion | 命中 | J |
|---|---:|---:|---:|
| 提交策略 | 20 000 | 0 | 7 719 387 |
| no-promote | 0 | 0 | 6 066 503 |
| 官方 baseline | 20 000 | 20 000 | 7 672 208 |

策略在“凑够 credit 的那次访问”后提升；若这是最后一次访问，则纯付固定成本。更深一层：该形态下 makespan 由提交间隔决定，命中即使更快也不缩短 TraceTime，只增加 CPU 成本。策略的收益模型默认“省下的服务时间一定缩短 makespan”，缺少拥塞判断。

公开集 `promote_hbm_low` 是相反情形：no-promote 的 TraceTime 17 858 us > 最后到达 10 000 us，存在积压；promotion 把 TraceTime 降到 10 435 us，单次命中实测节省约 38.7 us。策略估算 `gain = 39.7` 恰好接近实测值，但这个“正确”来自 `card_share` 的经验缩放，而非显式拥塞建模。

### 3.2 P0：准入不感知池占用与淘汰

`solution.cpp:613-617` 只检查该 key 是否已在缓存，不检查加入后是否会把别的条目挤掉。长周期用例（80 轮，每轮 8 卡各 1 MiB）：

| 用例 | 原版 promotion / 淘汰 | 原版 J | no-promote J | 差值 |
|---|---:|---:|---:|---:|
| long_kv1_cycle2（1 MB 池，2 key） | 34 / 33 | 162 482 | 159 694 | +2 788 |
| long_kv1_cycle8（1 MB 池，8 key） | 8 / 7 | 160 350 | 159 694 | +656 |
| long_kv8_cycle8（8 MB 池，8 key） | 57 / 49 | 164 052 | 159 694 | +4 358 |
| long_kv8_cycle9（8 MB 池，9 key） | 56 / 48 | 164 332 | 159 694 | +4 638 |
| long_ttl1000_gap500（TTL 1 ms） | 0 / 0 | 41 194 | 41 194 | 0 |

`long_kv8_cycle8` 中 8×1 MiB 恰好放得进 8 MB 池，仍发生 57 次 promotion/49 次淘汰——这是重复提升造成的抖动。

### 3.3 P1：短重复序列必然浪费

- `twosight_20k`（每个 key 只出现 2 次）：提交策略 0 次 promotion，与 no-promote 相同；baseline 20 000 次无用 promotion，J 高 41%。
- `threesight_20k`：提交策略在第 3 次（最后一次）访问后 promotion，20 000 次全部无命中。

### 3.4 P1：`cached_ranges` 全扫描的 O(n²) 风险

`solution.cpp:613-617` 每次 `on_iocb_complete` 遍历全部缓存条目。`threesight_20k` 中缓存条目增至 20 000，60 000 个 iocb 让策略总耗时 11.2 s（no-promote 1.4 s），峰值 869 MB。10 万 iocb 且 promotion 很多时会继续放大；单次决策仍远低于 60 s，但会拉长套件总时间，且“单批 <100 ms”在缓存条目多时不再成立（本次平均约 0.18 s/批）。

### 3.5 其他观察

- `promote_hit_rate` 是“成功插入数 / promotion 数”，不是复用率；提交文档已说明，不算 bug，但看板解读容易误解。
- `history_` 上限 131 072，10 万 key 够用；`batch_costs_` 64 条未见问题；`promotion_cards_` 有清理逻辑。
- TTL 判断使用“promotion 完成 + ttl”与“预计下次到达”，在到达受限场景下无法识别“没有下次到达”。
- 未发现整数溢出、空指针、越界；ASan/UBSan 干净。

## 4. 已验证补丁 V3b（建议采纳）

补丁：`reviews/patches/kaisennhu-v3b-occupancy-guard.diff`

内容：

1. 在准入处累计 `cached_ranges` 的 `length` 得到驻留字节；
2. 遍历 `promote_inflight`，用新增的 `promote_lengths_[promote_id] = io.length` 计入 inflight 字节；
3. 若 `驻留 + inflight + io.length > kvcache_pool_capacity`，拒绝 promotion；
4. `promote_lengths_` 超过 8192 条时按当前 inflight 集合清理。

效果：

| 指标 | 原版 | V3b |
|---|---:|---:|
| 公开 automated | 89.9792 | **89.99** |
| promote_high opt | 59.89 | **60.00** |
| promote_high promotion | 30 | 8 |
| long_kv1_cycle2 promotion / 淘汰 | 34 / 33 | **1 / 0** |
| long_kv1_cycle8 promotion / 淘汰 | 8 / 7 | **1 / 0** |
| long_kv8_cycle8 promotion / 淘汰 | 57 / 49 | **8 / 0** |
| long_kv8_cycle9 promotion / 淘汰 | 56 / 48 | **8 / 0** |
| 10 万 iocb 大对象结果 | — | 完全一致 |

V3b 仍不能消除 `long_kv8_cycle8` 的 +656（8 次 promotion × 80 固定成本，命中在到达受限形态下不省 TraceTime）；这需要 3.1 的拥塞/收益判断。

## 5. 走不通的“直接修公式”方案

| 方案 | 改动 | 公开分 | 问题 |
|---|---|---:|---|
| V1 | 收益侧去掉 `card_share`：`read + copy - hit` | 88.71 | `promote_hbm_low/mid` 全部不再提升，掉到 52.99/44.40 |
| V2 | 成本侧去掉 `card_share`（D2H service 全价） | 89.43 | promotion 延迟，命中 192→64 / 784→256，J 变差 |
| V5 | V3b + 至少 3 次观测才 promotion | 89.72 | 公开集少一轮命中，`promote_hbm_low` 命中 192→64 |

结论：`card_share` 是对公开集“全局 SSD 瓶颈 + 按卡并行 H2D”的经验补偿，单侧移除会破坏已调好的平衡。若要改，必须同时引入“是否处于拥塞/积压”的判断，并在公开集与小池/短重复集上同时回归。

## 6. 建议的修复优先级

1. **P0（低风险，已验证）**：采纳 V3b 的池占用 + inflight 守卫。公开 +0.01，小池淘汰清零，10 万 iocb 无回归。
2. **P0（性能）**：用增量计数/集合替代每次 `on_iocb_complete` 对 `cached_ranges` 的全扫描；池占用守卫也可 O(1) 维护。
3. **P1（收益模型）**：promotion 准入增加“系统是否有积压”的条件（同一提交内其他 iocb 是否仍在排队/在飞、`now - arrival` 相对整批服务时间的比值、连续提交的完成时间是否晚于到达）。到达受限时不做 promotion。
4. **P1（小池）**：准入时校验复用工作集是否放得下，即 `byte_clock_ - h.last_byte <= kvcache_pool_capacity`（代码已有该检查，但长周期小池仍漏过 1 次，需确认字节时钟更新粒度），并对同一池代际限制 promotion 次数。
5. **P2（证据门槛）**：对同一 key，要求至少观察到一次真实命中后再允许后续 promotion，避免“最后一次访问才提升”的纯亏。
6. **测试补充**：长周期小池循环、2/3 次重复、10 万 iocb、1 MB 池 + 对齐/非对齐、TTL 边界。

## 7. 复现

```bash
# 公开集评分
P=/home/dpj135/hackthon/hackathon/public
docker exec -w "$P" hackthon python3 scripts/score_suite.py \
  --player /path/to/nds_player \
  --validate "$P/nds_validate" --suite "$P/suites/public_suite.json" \
  --data-dir "$P/data" --frozen "$P/suites/frozen_costs.json" \
  --baseline "$P/suites/public_baseline.json" \
  --output /tmp/score.json

# 应用 V3b 补丁
git apply reviews/patches/kaisennhu-v3b-occupancy-guard.diff
```

测试脚本与合成 trace 位于评审工作区（未入库）：`run_all.py`、`compare_noprom.py`、`gen_cases*.py`、`compare_long.py`、`run_variant.py`。

## 8. 关键代码位置

- promotion 准入与收益计算：`submissions/kaisennhu/03_程序/player/solution.cpp:595-682`
  - 收益：`:645-647`（`hit` 与 `copy_us` 同乘 `card_share`）
  - 成本：`:638-640`（D2H service 乘 `card_share`；SSD service 不乘 `qs`）
  - credit 累计：`:650-652`
- 缓存成员扫描：`:613-617`
- 本地仿真评估：`evaluate()` 7 次/提交，仅在 `iocbs<=1024 && iov<=2048 && ops<=2048` 时触发；公开集最大决策 1.008 s 即来自此路径，仍远低于 60 s，可保留。
