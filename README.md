# NDS 多路径 IO 调度

本仓库包含[官方选手包](public/)与 [KaisennHu 的正式提交材料](submissions/kaisennhu/)。材料按赛题要求排序，PPT在最后。

| 顺序 | 入口 |
| --- | --- |
| 1. 调度算法源代码 | [唯一策略 solution.cpp](submissions/kaisennhu/01_调度算法源代码/player/solution.cpp) |
| 2. 可执行程序 | [Linux x86_64 程序与校验清单](submissions/kaisennhu/02_可执行程序/linux-x86_64/) |
| 3. 输入输出格式 | [JSON协议与运行方式](submissions/kaisennhu/03_输入输出说明/输入输出格式.md) |
| 4. 算法设计与复杂度 | [方案介绍](submissions/kaisennhu/04_算法设计与复杂度/方案介绍.md)、[算法设计](submissions/kaisennhu/04_算法设计与复杂度/算法设计.md)、[复杂度分析](submissions/kaisennhu/04_算法设计与复杂度/复杂度分析.md)、[双卡示例](submissions/kaisennhu/04_算法设计与复杂度/双卡读取逐步示例.md) |
| 5. 自动化测试与复现 | [统一入口](submissions/kaisennhu/run.py)、[复现手册](submissions/kaisennhu/05_自动化测试与复现/测试操作手册.md) |
| 6. 性能与资源报告 | [验证报告与逐例证据](submissions/kaisennhu/06_性能与资源报告/验证报告.md) |
| 7. 答辩PPT | [9页、约7分钟PPT](submissions/kaisennhu/07_答辩PPT/答辩介绍.pptx)、[讲解与演示流程](submissions/kaisennhu/07_答辩PPT/讲解与演示流程.md) |

完整克隆后，在Linux或WSL执行：

```bash
cd submissions/kaisennhu
python3 05_自动化测试与复现/verify_submission.py
python3 run.py quick
python3 run.py score
python3 run.py demo
```

交付核验使用随附Linux x86_64二进制；从源码构建另需C++17编译器、make、nm和Python 3.10+。脚本从本仓库`public/`校验并复制固定SDK，按需重建扩展输入，不修改官方源码与参考成本。逐文件摘要和交付范围见[材料目录与提交清单](submissions/kaisennhu/README.md)。
