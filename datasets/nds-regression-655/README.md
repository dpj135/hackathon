# NDS 完整回归集：655 条

固定保存提交报告使用的 655 条输入，供不同调度策略在相同负载上复现和对照。数据内容与已有验证记录逐字节一致。

## 目录与来源

```text
nds-regression-655/
├── README.md           # 分组、来源与运行方法
├── manifest.json       # 全部 655 条的路径、SHA256、来源及生成参数
├── verify.py           # 检查数量、文件完整性与 JSON 格式
└── inputs/<分组>/*.json # 616 条补充输入
```

39 条官方输入直接引用仓库的 `public/data/`，避免重复保存。完整输入共约 372.9 MB，其中本目录新增输入约 300.7 MB；最大单文件约 7.25 MB。保持仓库目录结构即可离线使用，无需先生成数据或获取 Git 历史。

| 分组 | 条数 | 来源与覆盖内容 |
| --- | ---: | --- |
| official | 39 | 官方公开集，引用原始文件 |
| resources | 72 | 本地 `matrix.py`，资源组合 |
| distribution | 96 | 本地 `matrix.py`，访问分布 |
| regression | 96 | 本地 `matrix.py`，回归组合 |
| coverage | 54 | 已冻结的专项构造，含池压力、热点、碎片和 TTL |
| robustness | 33 | `robustness_inputs.py` 生成 24 条，另有 9 条尺寸边界输入 |
| cache_mix | 24 | `independent.py`，混合大小与复用 |
| blind | 24 | `renewal.py`，热点更新 |
| admission | 19 | `admission_workloads.py`，缓存准入 |
| holdout / engine_holdout / planning_holdout / transfer_holdout | 96 | `admission_holdout.py`，每组 24 条既有组合 |
| pr17_added | 78 | PR #17 的新增变体，剔除其重复收录的 39 条官方输入 |
| ablation_holdout | 24 | `admission_holdout.py`，种子 451009、451019、451027、451039、451049、451057 |
| **合计** | **655** | 官方 39 条＋补充回归 616 条 |

固定来源：

- 官方数据与生成脚本依据仓库提交 [`519ef500`](https://github.com/dpj135/hackathon/tree/519ef500ad5f685a7e7e010c076960610018c20b)。原始分组参数见该提交的 `submissions/kaisennhu/04_测试复现/fixtures/*/manifest.json`；本清单保留可用的种子、生成参数和拓扑标记。
- PR #17 数据来自提交 [`f26313bd`](https://github.com/dpj135/hackathon/tree/f26313bd67e7640da27635cb3588062a6884911b/submissions/dpj/data)，文件名中的 `__v2`、`__v3` 是原始负载变体名。
- 全部输入 SHA256 对应 [提交材料 PR #25](https://github.com/dpj135/hackathon/pull/25) 的 `06_性能与资源报告/逐输入验证.csv`，可按分组、文件名核对。

655 条没有字节完全相同的输入，但相近变体并不代表相互独立的分布。`blind`、`holdout` 等是保留的历史分组名，全部补充数据均按**已见回归集**使用；含非赛题拓扑的输入仅用于健壮性检查。只有官方 39 条参与官方公开集评分。

## 使用方法

在仓库根目录检查数据，Python 3.10+ 即可，无第三方依赖：

```bash
python3 datasets/nds-regression-655/verify.py
```

使用仓库现有策略和官方回放器跑全部 655 条，每条三轮：

```bash
cd submissions/kaisennhu
python3 run.py extended --data ../../datasets/nds-regression-655 --repeat 3
```

该命令同时适用于当前主线和 PR #25 的整理目录，自动构建程序、按清单逐条校验哈希、执行策略及官方回放，并检查重复输出的一致性。需要 Linux/WSL、C++17 编译器、make、nm 和 Python 3.10+。结果写入提交目录下的 `运行结果/`，不写入数据集。

`manifest.json` 的 `file` 相对于本目录；`group` 是回归分组，`report_group` 对应提交报告中的中文组名；`source` 区分官方、本地构造与 PR #17；`usage` 区分公开评分与已见回归。运行单个输入时，将该相对路径传给 `nds_player`，再用 `nds_validate` 回放输出即可。
