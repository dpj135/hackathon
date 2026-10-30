# NDS 多路径IO调度

本仓库包含官方选手包与 KaisennHu 的解题方案。

| 入口 | 内容 |
| --- | --- |
| [官方选手包](public/) | engine、接口、公开数据与评分器 |
| [方案目录与运行入口](submissions/kaisennhu/) | 唯一策略、复现脚本与验证摘要 |
| [零基础介绍](submissions/kaisennhu/00_快速入门/方案介绍.md) | 任务、术语、计费与整体流程 |
| [算法设计](submissions/kaisennhu/02_算法方案/算法设计.md) | 调度规则、公式、设计依据与通用性边界 |
| [5MiB双卡逐步示例](submissions/kaisennhu/02_算法方案/双卡读取逐步示例.md) | 从100B起点到分块、选路、执行和成本 |
| [验证报告](submissions/kaisennhu/05_验证结果/验证报告.md) | 最新engine下的评分、完整逐例对照和回退 |
| [答辩与演示](submissions/kaisennhu/06_答辩演示/讲解与演示流程.md) | 讲解提纲与可运行现场演示 |

完整克隆后，在Linux或WSL执行：

```bash
cd submissions/kaisennhu
python3 run.py quick
python3 run.py score
python3 run.py demo
```

需要C++17编译器、make和Python 3.10+。入口从本仓库`public/`校验并复制固定SDK，按需重建扩展输入；不修改官方源码与参考成本。详情见[复现手册](submissions/kaisennhu/04_测试复现/测试操作手册.md)。

Git仅保留源码、必要的解释和复现依据。二进制、Word/HTML报告、PPTX/PDF、逐页图片与完整原始记录由本地离线交付提供，不在仓库中重复存储。
