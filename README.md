# NDS 多路径 IO 分配聚合与缓存提升调度

本仓库包含官方选手包及解题提交材料。官方程序、公开测试集和评分依据位于 `public/`；KaisennHu 的完整解题材料位于 [`submissions/kaisennhu/`](submissions/kaisennhu/)。

## 获取解题材料

- [下载完整提交包 NDS_Submission.zip](submissions/kaisennhu/NDS_Submission.zip?raw=true)（24.74 MiB）及 [SHA256](submissions/kaisennhu/NDS_Submission.zip.sha256)。
- [零基础方案介绍](submissions/kaisennhu/00_快速入门/方案介绍.md)：从存储术语和问题目标开始介绍算法与使用方法。
- [材料目录与运行说明](submissions/kaisennhu/README.md)。
- [算法设计](submissions/kaisennhu/02_算法方案/算法设计.md)与[通用性、边界条件](submissions/kaisennhu/02_算法方案/通用性与边界条件.md)。
- [验证报告](submissions/kaisennhu/05_验证结果/验证报告.md)：官方评分、完整逐例对照、成本回退及资源开销。
- [答辩材料](submissions/kaisennhu/06_答辩演示/)：可编辑 PPTX、PDF、逐页图片与离线演示。

提交包包含一份 C++17 在线策略、原始官方 SDK 和数据、Linux x86_64 静态可执行程序、测试与复现脚本，以及 Word、网页和 Markdown 文档。

## 运行

Linux 或 Windows WSL 中进入材料目录：

```bash
cd submissions/kaisennhu
python3 run.py solve --prebuilt --input 03_程序/data/small_mixed.json --result result.json
python3 run.py quick
python3 run.py score
python3 run.py demo
```

预编译入口需要 Linux x86_64 和 Python 3.10 或更高版本；从源码构建另外需要 C++17 编译器和 make。文档可以直接在 Windows 阅读。程序运行不需要 GPU 或网络。

## 已验证结果与范围

固定官方 SDK 提交 `6b1666eb908a4cb55a84dc20832d9e57ffc1e4c4`，原始 39 条公开输入自动分 **89.9792/90**，官方贪心 **67.1981/90**；另 10 分由评委评分。438 条输入各运行三次，共 1314 次运行与官方回放通过，116 项功能、边界和评分门禁检查通过。

这些结果来自官方模拟器，不代表隐藏集排名或真实硬件加速。扩展数据中只有 24 条独立家族输入在最终源码冻结后生成且未用于选择；报告完整保留成本回退，不承诺每个输入均优于官方贪心。

策略源码 SHA256：`6a30c42d1a939b131290c2802a1b2942fad991cc857dede96cbc3890b8cb22d6`。包内文件校验清单及 ZIP 校验值用于验证下载内容。
