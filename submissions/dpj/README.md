# dpj 3x 数据集

本目录把公开的 39 条 trace 等比扩展为 117 条，用于算法规模与泛化测试。

## 组成

- `data/`：117 条 trace
  - 39 条公开原始 trace，原样复制
  - 每条原始 trace 另有 2 条变体：`<name>__v2.json`、`<name>__v3.json`
- `suite.json`：117 个 case，字段与 `public/suites/public_suite.json` 一致

每条原始 trace 及其两条变体共享原用例权重，因此 suite 总权重为 300，归一化后的权重分布与公开集 100 分完全一致：

| 单例权重 | case 数 |
|---:|---:|
| 1 | 48 |
| 2 | 33 |
| 4 | 24 |
| 6 | 9 |
| 12 | 3 |

## 生成方式

- 有 `generator_metadata` 的 trace：复用原生成参数，仅更换 seed（`+20000`、`+30000`）
- 手写 trace：按原始工作负载形状重新生成
- AGGREGATE / FALLBACK 变体额外使用 `--hbm-unaligned`，保持存储侧非对齐特征
- 变体生成后保留原始 `instance.resources`、`instance.fixed_parameters` 与 `instance.runtime_limits`，只替换 `instance_id`

## 校验

已在 `public/` 下对全部 117 条 trace 执行：

```bash
./nds_baseline ../submissions/dpj/data/<trace>.json /tmp/sol.json
./nds_validate ../submissions/dpj/data/<trace>.json /tmp/sol.json
```

结果：117/117 `valid: true`，0 失败。

本数据集用于离线扩展测试，不替代官方公开集评分。
