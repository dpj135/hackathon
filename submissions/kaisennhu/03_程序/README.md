# 程序与运行

唯一提交策略为 player/solution.cpp。src、include、tests、data、suites 和 scripts 来自固定官方 SDK；正式评测链接主办方模拟器。推荐从材料根目录执行 python3 run.py，评分入口会额外检查失败状态。

```bash
python3 run.py solve --prebuilt --input 03_程序/data/small_mixed.json --result result.json
python3 run.py quick
python3 run.py score
```

预编译程序在 bin/linux-x86_64，为 Linux x86_64 静态程序；manifest.json 记录对应源码及二进制哈希。Windows 下请在 WSL 中运行。源码构建依赖 C++17、make、Python 3.10或更高版本。

手工构建和运行：

```bash
make -C 03_程序 -j4
03_程序/nds_player 03_程序/data/small_mixed.json result.json
03_程序/nds_validate 03_程序/data/small_mixed.json result.json
```

nds_baseline 为官方贪心，nds_validate 为独立回放器，nds_generate 为官方生成器。不要执行评分脚本的 --freeze 选项。未经根入口检查的 make score 退出码本身不能证明每条通过。

## 文件导航

| 目录 | 用途 |
| --- | --- |
| [player/solution.cpp](player/solution.cpp) | 唯一提交策略 |
| [include/](include/)、[src/](src/) | 官方SDK接口与实现 |
| [bin/linux-x86_64/](bin/linux-x86_64/) | 静态可执行程序与源码/二进制校验清单 |
| [data/](data/)、[suites/](suites/) | 官方原始输入、评分配置与参考成本 |
| [tests/](tests/)、[scripts/](scripts/) | 官方框架检查与辅助脚本 |

下一步：[测试复现](../04_测试复现/) · [返回材料首页](../README.md)。
