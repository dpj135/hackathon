#!/usr/bin/env bash
# Regenerate the scored public traces. Hand-written traces are left in place.
set -euo pipefail
cd "$(dirname "$0")/.."
gen="${1:-./nds_generate}"
out="${2:-data}"
mkdir -p "$out"
common=(--balance-cards --cards 8)

"$gen" "${common[@]}" --preset aligned --submits 4 --iocbs 32 --iovs 1 --seed 101 \
  --instance-id public-01-aligned -o "$out/01_aligned.json" >/dev/null
"$gen" "${common[@]}" --preset low-unaligned --submits 4 --iocbs 32 --seed 102 \
  --instance-id public-02-low-unaligned -o "$out/02_low_unaligned.json" >/dev/null
"$gen" "${common[@]}" --preset high-unaligned --submits 4 --iocbs 32 --iovs-max 4 --seed 103 \
  --instance-id public-03-high-unaligned -o "$out/03_high_unaligned.json" >/dev/null
"$gen" "${common[@]}" --preset fragmented --submits 4 --iocbs 16 --iovs-max 6 --seed 104 \
  --instance-id public-04-fragmented -o "$out/04_fragmented.json" >/dev/null
"$gen" "${common[@]}" --preset balanced --submits 4 --iocbs 32 --size-profile mixed --seed 105 \
  --instance-id public-05-mixed-size -o "$out/05_mixed_size.json" >/dev/null
"$gen" "${common[@]}" --preset reuse-hot --submits 8 --iocbs 32 --seed 106 \
  --instance-id public-06-reuse-hot -o "$out/06_reuse_hot.json" >/dev/null
"$gen" "${common[@]}" --preset no-reuse --submits 4 --iocbs 32 --seed 107 \
  --instance-id public-07-no-reuse -o "$out/07_no_reuse.json" >/dev/null
"$gen" "${common[@]}" --preset ttl-expiry --submits 6 --iocbs 16 --seed 108 \
  --instance-id public-08-ttl-expiry -o "$out/08_ttl_expiry.json" >/dev/null
"$gen" "${common[@]}" --preset buffer-pressure --submits 4 --iocbs 16 --seed 109 \
  --instance-id public-09-buffer-pressure -o "$out/09_buffer_pressure.json" >/dev/null
"$gen" "${common[@]}" --preset concurrency-pressure --submits 2 --iocbs 64 --seed 110 \
  --instance-id public-10-concurrency -o "$out/10_concurrency_pressure.json" >/dev/null
"$gen" "${common[@]}" --preset aligned --submits 4 --iocbs 32 --iovs 1 \
  --reuse-rate 0 --unaligned-rate 0 --adjacent-rate 1 --sizes 4096,8192,65536 --seed 111 \
  --instance-id public-11-merge-friendly -o "$out/11_merge_friendly.json" >/dev/null

# 中规模：单次 submit 256 个 iocb，总共 4096。高于小规模上限 1000，低于中规模上限 20000。
"$gen" "${common[@]}" --preset balanced --submits 16 --iocbs 256 --size-profile mixed --seed 201 \
  --instance-id public-mid-mixed -o "$out/mid_mixed.json" >/dev/null
"$gen" "${common[@]}" --preset high-unaligned --submits 16 --iocbs 256 --iovs-max 4 --seed 202 \
  --instance-id public-mid-unaligned -o "$out/mid_unaligned.json" >/dev/null
"$gen" "${common[@]}" --preset reuse-hot --submits 16 --iocbs 256 --seed 203 \
  --instance-id public-mid-reuse -o "$out/mid_reuse.json" >/dev/null

# 大规模：单次 submit 1024 个 iocb，总共 20480。超过中规模总条数上限，仍低于大规模上限 100000。
"$gen" "${common[@]}" --preset aligned --submits 20 --iocbs 1024 --iovs 1 \
  --reuse-rate 0 --unaligned-rate 0 --seed 301 \
  --instance-id public-high-aligned -o "$out/high_aligned.json" >/dev/null
"$gen" "${common[@]}" --preset balanced --submits 20 --iocbs 1024 --iovs 1 --size-profile mixed --seed 302 \
  --instance-id public-high-mixed -o "$out/high_mixed.json" >/dev/null

# 低档和中档：热键反复读取，池子装得下，大部分 iocb 是缓存命中。
# 到达间隔让 PROMOTE 在下一次 submit 之前完成，又不会把 J 变成纯等待。
hot=(--iovs 1 --sizes 1048576 --reuse-rate 1 --adjacent-rate 0 --hotset-size 32)
"$gen" "${common[@]}" --preset aligned "${hot[@]}" --unaligned-rate 0 \
  --submits 12 --iocbs 64 --arrival-gap-us 2000 --seed 401 \
  --instance-id public-promote-low-aligned -o "$out/promote_low_aligned.json" >/dev/null
"$gen" "${common[@]}" --preset aligned "${hot[@]}" --unaligned-rate 1 \
  --submits 12 --iocbs 64 --arrival-gap-us 2500 --seed 402 \
  --instance-id public-promote-low-unaligned -o "$out/promote_low_unaligned.json" >/dev/null
"$gen" "${common[@]}" --preset aligned "${hot[@]}" --unaligned-rate 0 \
  --submits 10 --iocbs 256 --arrival-gap-us 8000 --seed 403 \
  --instance-id public-promote-mid -o "$out/promote_mid.json" >/dev/null
# 大档：每张卡轮转 256 个 1MB 对象，工作集 2GB，KVCache 只有 8MB。
# 参考贪心会把见过两次的对象都 PROMOTE，随后被 LRU 驱逐，后面还得从 SSD 读。
"$gen" "${common[@]}" --preset aligned --iovs 1 --sizes 1048576 \
  --unaligned-rate 0 --adjacent-rate 0 --cycle-per-card 256 \
  --mooncake-capacity-mb 8 --submits 20 --iocbs 1024 --arrival-gap-us 20000 --seed 404 \
  --instance-id public-promote-high -o "$out/promote_high.json" >/dev/null

# 读路径：相邻的小块。参考调度把同一张卡上连续的范围并成一次传输。
# 贪心按 iov 各发一次，512B 落在最低带宽档。
direct=(--preset aligned --iovs 1 --sizes 512 --reuse-rate 0 --unaligned-rate 0 \
  --adjacent-rate 1 --arrival-gap-us 20)
"$gen" "${common[@]}" "${direct[@]}" --submits 10 --iocbs 80 --seed 501 \
  --instance-id public-read-direct-low -o "$out/read_direct_low.json" >/dev/null
"$gen" "${common[@]}" "${direct[@]}" --submits 16 --iocbs 256 --seed 502 \
  --instance-id public-read-direct-mid -o "$out/read_direct_mid.json" >/dev/null
"$gen" "${common[@]}" "${direct[@]}" --submits 20 --iocbs 1024 --seed 503 \
  --instance-id public-read-direct-high -o "$out/read_direct_high.json" >/dev/null

# SSD 对齐、HBM 错 1 字节，DIRECT 不合法。32KB 以上走 AGGREGATE，合并后 D2D 档位更高。
aggregate=(--preset aligned --hbm-unaligned --iovs 1 --sizes 32768 --reuse-rate 0 \
  --unaligned-rate 0 --adjacent-rate 1 --arrival-gap-us 20)
"$gen" "${common[@]}" "${aggregate[@]}" --submits 8 --iocbs 80 --seed 511 \
  --instance-id public-read-aggregate-low -o "$out/read_aggregate_low.json" >/dev/null
"$gen" "${common[@]}" "${aggregate[@]}" --submits 12 --iocbs 256 --seed 512 \
  --instance-id public-read-aggregate-mid -o "$out/read_aggregate_mid.json" >/dev/null
"$gen" "${common[@]}" "${aggregate[@]}" --submits 20 --iocbs 1024 --seed 513 \
  --instance-id public-read-aggregate-high -o "$out/read_aggregate_high.json" >/dev/null

# 4KB 且 HBM 不对齐。小块 D2D 很慢，合并后的 FALLBACK（一次 SSD→DDR 加一次大 H2D）更快。
fallback=(--preset aligned --hbm-unaligned --iovs 1 --sizes 4096 --reuse-rate 0 \
  --unaligned-rate 0 --adjacent-rate 1 --arrival-gap-us 20)
"$gen" "${common[@]}" "${fallback[@]}" --submits 8 --iocbs 80 --seed 521 \
  --instance-id public-read-fallback-low -o "$out/read_fallback_low.json" >/dev/null
"$gen" "${common[@]}" "${fallback[@]}" --submits 12 --iocbs 256 --seed 522 \
  --instance-id public-read-fallback-mid -o "$out/read_fallback_mid.json" >/dev/null
"$gen" "${common[@]}" "${fallback[@]}" --submits 20 --iocbs 1024 --seed 523 \
  --instance-id public-read-fallback-high -o "$out/read_fallback_high.json" >/dev/null

# PROMOTE_SSD：4KB，D2H 并发为 1。SSD 提升能在下一次提交前完成，D2H 队列完不成。
ssd=(--preset aligned --iovs 1 --sizes 4096 --reuse-rate 0 --unaligned-rate 0 \
  --adjacent-rate 0 --d2h-concurrency 1)
"$gen" "${common[@]}" "${ssd[@]}" --cycle-per-card 10 --submits 8 --iocbs 80 \
  --arrival-gap-us 600 --seed 531 \
  --instance-id public-promote-ssd-low -o "$out/promote_ssd_low.json" >/dev/null
"$gen" "${common[@]}" "${ssd[@]}" --cycle-per-card 32 --submits 8 --iocbs 256 \
  --arrival-gap-us 600 --seed 532 \
  --instance-id public-promote-ssd-mid -o "$out/promote_ssd_mid.json" >/dev/null
"$gen" "${common[@]}" "${ssd[@]}" --cycle-per-card 64 --submits 20 --iocbs 1024 \
  --arrival-gap-us 1400 --seed 533 \
  --instance-id public-promote-ssd-high -o "$out/promote_ssd_high.json" >/dev/null

# PROMOTE_HBM：1MB，D2H 并发为 1。对象大，SSD 提升会错过复用窗口，D2H 赶得上。
hbm=(--preset aligned --iovs 1 --sizes 1048576 --reuse-rate 0 --unaligned-rate 0 \
  --adjacent-rate 0 --d2h-concurrency 1 --arrival-gap-us 2000)
"$gen" "${common[@]}" "${hbm[@]}" --cycle-per-card 8 --submits 6 --iocbs 64 --seed 541 \
  --instance-id public-promote-hbm-low -o "$out/promote_hbm_low.json" >/dev/null
"$gen" "${common[@]}" "${hbm[@]}" --cycle-per-card 16 --submits 6 --iocbs 256 \
  --arrival-gap-us 8000 --seed 542 \
  --instance-id public-promote-hbm-mid -o "$out/promote_hbm_mid.json" >/dev/null
"$gen" "${common[@]}" "${hbm[@]}" --cycle-per-card 8 --submits 20 --iocbs 1024 \
  --arrival-gap-us 10000 --seed 543 \
  --instance-id public-promote-hbm-high -o "$out/promote_hbm_high.json" >/dev/null

echo "regenerated public traces in $out"
echo "re-freeze suites/public_baseline.json before publishing scores"
