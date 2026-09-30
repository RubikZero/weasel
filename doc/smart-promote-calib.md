best T=+0.50

T=-1.00  hard=0.4375(promo 0.97)  words=0.6900(promo 0.95) avg=0.5637
T=-0.90  hard=0.4375(promo 0.96)  words=0.6975(promo 0.94) avg=0.5675
T=-0.80  hard=0.4375(promo 0.96)  words=0.7050(promo 0.94) avg=0.5713
T=-0.70  hard=0.4375(promo 0.96)  words=0.7125(promo 0.93) avg=0.5750
T=-0.60  hard=0.4450(promo 0.95)  words=0.7200(promo 0.92) avg=0.5825
T=-0.50  hard=0.4450(promo 0.95)  words=0.7350(promo 0.90) avg=0.5900
T=-0.40  hard=0.4450(promo 0.95)  words=0.7400(promo 0.89) avg=0.5925
T=-0.30  hard=0.4550(promo 0.93)  words=0.7475(promo 0.87) avg=0.6013
T=-0.20  hard=0.4575(promo 0.93)  words=0.7550(promo 0.86) avg=0.6062
T=-0.10  hard=0.4600(promo 0.92)  words=0.7625(promo 0.85) avg=0.6112
T=+0.00  hard=0.4675(promo 0.69)  words=0.7675(promo 0.27) avg=0.6175
T=+0.10  hard=0.4750(promo 0.66)  words=0.7975(promo 0.23) avg=0.6362
T=+0.20  hard=0.4750(promo 0.64)  words=0.8225(promo 0.19) avg=0.6487
T=+0.30  hard=0.4775(promo 0.61)  words=0.8300(promo 0.17) avg=0.6537
T=+0.40  hard=0.4825(promo 0.56)  words=0.8375(promo 0.16) avg=0.6600
T=+0.50  hard=0.4925(promo 0.52)  words=0.8400(promo 0.14) avg=0.6663
T=+0.60  hard=0.4900(promo 0.51)  words=0.8400(promo 0.13) avg=0.6650
T=+0.70  hard=0.4800(promo 0.47)  words=0.8500(promo 0.10) avg=0.6650
T=+0.80  hard=0.4750(promo 0.46)  words=0.8450(promo 0.10) avg=0.6600
T=+0.90  hard=0.4750(promo 0.43)  words=0.8425(promo 0.08) avg=0.6587
T=+1.00  hard=0.4625(promo 0.40)  words=0.8400(promo 0.08) avg=0.6512
T=+1.10  hard=0.4625(promo 0.39)  words=0.8375(promo 0.07) avg=0.6500
T=+1.20  hard=0.4600(promo 0.38)  words=0.8425(promo 0.06) avg=0.6512
T=+1.30  hard=0.4475(promo 0.36)  words=0.8400(promo 0.06) avg=0.6438
T=+1.40  hard=0.4400(promo 0.34)  words=0.8375(promo 0.05) avg=0.6388
T=+1.50  hard=0.4375(promo 0.32)  words=0.8350(promo 0.05) avg=0.6362
T=+1.60  hard=0.4375(promo 0.31)  words=0.8300(promo 0.04) avg=0.6338
T=+1.70  hard=0.4350(promo 0.29)  words=0.8275(promo 0.04) avg=0.6312
T=+1.80  hard=0.4325(promo 0.27)  words=0.8225(promo 0.03) avg=0.6275
T=+1.90  hard=0.4250(promo 0.25)  words=0.8200(promo 0.03) avg=0.6225
T=+2.00  hard=0.4200(promo 0.23)  words=0.8175(promo 0.02) avg=0.6188
T=+2.10  hard=0.4200(promo 0.23)  words=0.8175(promo 0.02) avg=0.6188
T=+2.20  hard=0.4150(promo 0.22)  words=0.8150(promo 0.02) avg=0.6150
T=+2.30  hard=0.4075(promo 0.20)  words=0.8125(promo 0.01) avg=0.6100
T=+2.40  hard=0.4000(promo 0.19)  words=0.8100(promo 0.01) avg=0.6050
T=+2.50  hard=0.3975(promo 0.17)  words=0.8100(promo 0.01) avg=0.6038
T=+2.60  hard=0.3900(promo 0.15)  words=0.8050(promo 0.01) avg=0.5975
T=+2.70  hard=0.3850(promo 0.14)  words=0.8050(promo 0.01) avg=0.5950
T=+2.80  hard=0.3800(promo 0.13)  words=0.8050(promo 0.01) avg=0.5925
T=+2.90  hard=0.3775(promo 0.12)  words=0.8050(promo 0.01) avg=0.5913
T=+3.00  hard=0.3750(promo 0.12)  words=0.8025(promo 0.01) avg=0.5887


## 2026-09-16 复校（修复 ime_top 选取 + 新解码器）

### 发现的 bug
- 插件 PromoteSentence 原先用 `quality` 最大者作为 IME 基准候选；但 rime 的整句候选 quality=0（最低），实际选中了单字候选（如 `副`/`之一`），margin 口径完全失真（旧生产阈值实际等价于 always-promote）。
- 修复：IME 基准 = 菜单首位候选（用户本应看到的首选）。

### 复校数据（600 例：100+100 与 300+300 holdout，C++ 生产链路 margin）
| T | hard100 | words100 | hard_hold300 | words_hold300 | 合计均值 |
|---|---:|---:|---:|---:|---:|
| 0.0 | 0.495 (promo 0.67) | 0.850 (0.20) | 0.527 (0.67) | 0.803 (0.24) | 0.669 |
| +0.2 | 0.515 (0.62) | 0.880 (0.14) | 0.523 (0.62) | 0.833 (0.18) | 0.688 |
| **+0.3** | **0.556** (0.56) | **0.870** (0.13) | 0.517 (0.58) | 0.837 (0.17) | 0.695 |
| **+0.4** | **0.556** (0.55) | **0.870** (0.13) | 0.517 (0.56) | 0.843 (0.16) | **0.696** |
| **+0.5** | **0.556** (0.48) | 0.860 (0.11) | 0.517 (0.53) | 0.850 (0.14) | 0.696 |
| +0.6 | 0.556 (0.47) | 0.860 (0.11) | 0.510 (0.51) | 0.850 (0.12) | 0.694 |
| +0.8 | 0.556 (0.41) | 0.860 (0.08) | 0.493 (0.47) | 0.850 (0.09) | 0.690 |

- 结论：**维持默认 T=+0.5**（平台 0.4~0.5），修复后的生产行为首次与标定一致

### 生产链路验证（extractor，smart, margin=0.5）
| 集合 | 修复前(first==target) | 修复后 |
|---|---:|---:|
| hard100 | 40/100 | **55/100** |
| words100 | 78/100 | **86/100** |
| hard_hold300 | 145/300 | **155/300** |
| words_hold300 | 211/300 | **255/300** |

（promote=always 对照；置顶率 hard≈0.48~0.53、words≈0.11~0.14）

### 复现
```powershell
# 采集 margin（需插件 debug 日志）：config promote: smart, promote_margin: -100 + RIME_LM_RANKER_DEBUG=1
python calib_margin.py --log testset\test_margin_h.log --requests testset\requests_sent_100.tsv --label hard100
# 生产验证（margin=0.5）
rime_real_candidates.exe ... --requests requests_hard_hold.tsv ...
```
