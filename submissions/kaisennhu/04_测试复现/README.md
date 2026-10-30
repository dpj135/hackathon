# 测试复现

先阅读 [测试操作手册](测试操作手册.md)。全部命令从材料根目录执行，统一入口是 [run.py](../run.py)。

| 文件或目录 | 用途 |
| --- | --- |
| [regression.py](regression.py)、[boundaries.py](boundaries.py)、[gate_tests.py](gate_tests.py)、[admission_checks.py](admission_checks.py) | 功能、边界、变形与评分失败检测 |
| [evaluate.py](evaluate.py) | 重复运行、官方回放和输出确定性核验 |
| [compare.py](compare.py) | 与官方贪心逐输入比较成本 |
| [stress.py](stress.py)、[profile_main.cpp](profile_main.cpp) | 十万请求及高iov资源测试 |
| [matrix.py](matrix.py)、[independent.py](independent.py)、[renewal.py](renewal.py) | 构造与扩展不同测试家族 |
| [admission_workloads.py](admission_workloads.py)、[admission_holdout.py](admission_holdout.py) | 缓存准入专项与冻结后留出组合生成器 |
| [fixtures/](fixtures/) | 固定扩展输入、分组manifest及压力输入 |
| [生成方案报告.py](生成方案报告.py) | 从Markdown生成Word和离线HTML；仅文档生成需要python-docx |

官方39条输入位于 [03_程序/data](../03_程序/data/)。扩展输入来源和测试是否参与过选择，见 [验证报告](../05_验证结果/验证报告.md)。`fixtures/stress` 的gzip文件是脚本直接使用的压力数据。

下一步：[验证结果](../05_验证结果/)。

[返回材料首页](../README.md)
