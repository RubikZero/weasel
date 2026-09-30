# 拼音单字判别式重排实验记录（reranker-200k）

- 目标：拼音单字候选的**判别式重排**（listwise），把 librime 的候选分数作为独立浮点特征（不进 prompt）
- 基座：`uer/chinese_roberta_L-4_H-256`（**从基模开始**，未使用 MLM 微调权重初始化）
- 结论摘要：本规模下（20 万样本/2–6 epochs）从基模直接训练的重排器**未能超过旧 MLM 打分**；当前最佳是 `MLM(v1_final) + 0.25·先验`，真实测试集字级 top1 = **0.5234**（IME 先验 0.4542，+6.9pt）
- 路径：代码在 `D:\Workspace\rime\third-party\lm-train`、`lm-test`；文档在 `D:\Workspace\rime\doc`

---

## 0. 与上一轮实验的关系（回答"是否从 v4 继续"）

| 问题 | 答案 |
|---|---|
| 本次重排器初始化 | **从基模 `uer/chinese_roberta_L-4_H-256` 开始**，`train_reranker.py --base` 默认即基模，运行未覆盖 |
| 是否用 v1–v4 MLM 权重 | 没有。v1–v4 是 MLM 任务（mask-all 预测字），本轮为了**隔离判别式训练的效果**而未加载 |
| 旧 MLM 在本轮的角色 | 仅作为**集成评测的打分校**：`--mlm lm-train/checkpoints/final`（v1 final） |
| 含义 | 本轮回答的是"从零开始、只做 listwise 重排，能到哪"；结论是不能，下一步应改为"从 MLM 初始化 + listwise 微调" |

---

## 1. librime API 扩展（让候选分数可见）

旧版公开 API 的 `RimeCandidate` 只有 `text/comment`，拿不到分数。修改（自编 librime 分支）：

| 文件 | 修改 |
|---|---|
| `librime/src/rime_api.h` | `RimeCandidate` 增加 `double quality; char* type;` |
| `librime/src/rime_api_impl.h` | `rime_candidate_copy()` 填充 `quality`/`type`（strdup），`RimeFreeContext()` 释放 `type` |

重编与部署：
```powershell
D:\Workspace\rime\third-party\build-rime-x64.cmd librime
copy librime\dist\lib\rime.dll weasel\output\
copy librime\dist\lib\rime.dll third-party\lm-test\realcand\
```

**重要发现（影响特征设计）**：菜单排序由 `Candidate::compare()` 决定，顺序为
`start（靠前优先）→ end（同起点更长优先）→ quality（更高优先）`。
即**先按长度再按分数**；单字候选长度相同，故菜单序 = quality 序。这也是为什么特征里要包含 `rank` 与 `type`。

抽取器 `lm-test/rime_real_candidates.cc` 同步升级，输出格式：
`id \t rank \t 候选₁ \x1e comment \x1e quality \x1e type \x1f 候选₂ ...`

已观察到 type 取值：`phrase`（词典/句子）、`simplified`（simplifier 包装）等。

---

## 2. 训练数据构造

### 2.1 请求生成（`lm-train/prepare_reranker_requests.py`）
- 语料：`corpus/CLUECorpusSmall.txt`（13.7GB，一行一句）
- 词表：`essay.txt` 转简体后的高频 2–4 字词（前 50,000 个）
- 采样与目标：逐行扫描（`--sample-rate 1.0`），命中词表后随机取词内一个**单字**为目标；
  `keys` 用**词级 pypinyin** 取该字音节（避免多音字误注）；上下文 = 目标前最多 64 字
- 过滤：目标字必须在模型单字词表（7,322 字）内；上下文 ≥8 字；精确 `(context,target)` 去重
- **严格切分/去重**：
  1. 按**语料行**切分 train/val（同一行的样本只进一边，保证上下文不交叉）
  2. val 的整句若出现在 train 中则剔除
  3. val 内部再做 span 精确去重
- 产出：`reranker/requests.tsv`（id, keys, target）、`reranker/meta.tsv`（id, split, context, word）
- 规模：**train 280,714 / val 4,924**（生成耗时约 4 分钟）

### 2.2 真实候选抽取（`lm-test/realcand/rime_real_candidates.exe`）
- 隔离用户目录 `rime-user-real`，schema `luna_pinyin_simp`，每例 `clear_composition` → `simulate_key_sequence(keys)` → 翻页收集
- 上限 60 个候选（12 页）；结果：**285,421 / 285,638 命中（99.9%）**，输出 439MB，耗时 2 分 08 秒

### 2.3 训练张量打包（`lm-train/build_reranker_npz.py`）
- 候选过滤：仅保留**单个汉字**且词表内；同字去重保留最高 quality
- 截断：按 quality 取 top-20；若正样本不在其中则追加（最终 C=21）
- 特征（均为**独立数值输入**，不拼接进文本）：
  - `logq = log1p(quality)`（case 内 z-score 后作基线项）
  - `rankn = 菜单排名 / (候选数-1)`
  - `type_id`（phrase/simplified/…，共 4 类）
- 文本序列：`[CLS] ctx(64) [SEP] cand(1) [SEP]`，左侧 PAD
- 产出：`reranker/data.npz`（ctx/cand/logq/rankn/types/mask/target_pos/split）
- 规模：**train 280,500 / val 4,921**；丢弃（目标不在单字候选内）217 条

---

## 3. 模型与训练（`lm-train/train_reranker.py`）

### 3.1 结构（score 作为独立浮点数）
```
score_i = α · z_case(log quality_i) + head( [CLS](ctx ⊕ cand_i), rank_norm_i, type_emb_i )
```
- 编码器：`BertModel`（基模，4 层/256 隐层）
- head：`Linear(256+8+1 → 128) → GELU → Linear(128 → 1)`，**末层零初始化**
  → 训练起点严格等于 IME 先验序
- `α` 默认固定 1.0；`--concat-score` 可把 z(logq) 再拼进 head（默认关闭，避免与 α 项重复计入）

### 3.2 损失与超参
- 损失：case 内 listwise 交叉熵 + `shrink=1e-4` 惩罚 head 输出幅度
- batch：16 或 32（每个 case 21 个候选 → 336/672 条序列）
- lr 3e-5，AdamW（wd 0.01），warmup 5%，线性衰减，梯度裁剪 1.0，bf16
- MLflow 记录（实验 `lm-ranker`），每 epoch 广播验证指标并按 val top1 保存 `model_best.pt`

### 3.3 两次正式运行
| 运行 | 数据 | epochs | batch | 耗时 | val top1（最终） | 备注 |
|---|---|---|---|---|---|---|
| reranker-200k | 200k | 2 | 16 | 15.6 min | 0.3916（先验 0.3985） | 未学到东西 |
| reranker-200k-e6 | 200k | 6 | 32 | 44.9 min | **0.5023** | epoch 6 突然跃升；训练不稳定（loss 尖峰 6+） |

epoch 明细（e6）：0.3910 → 0.3881 → 0.4011 → 0.3812 → 0.2229 → **0.5023**；
epoch 3/5 前训练 loss 从 ~1.1 飙至 ~6 后回落，存在明显的优化不稳定。

### 3.4 验证集 α/τ 扫描（model_best = e6）
| 配置 | top1 | churn | override 精确率 |
|---|---|---|---|
| IME 先验 | 0.3985 | — | — |
| α=2, τ=2 | **0.5446** | 61.5% | 40.9% |
| α=4, τ=2 | 0.4570 | 11.8% | 58.6% |
| α=8, τ=0 | 0.4139 | 4.4% | 44.9% |

（验证集来自与训练同语料的其它行，**收益未迁移到外部测试集**，见下节。）

---

## 4. 真实测试集评测（`lm-test/eval_reranker.py` / `lm_only_eval.py`）

测试集：luna_pinyin_simp 单字用例 **1,070 条**，IME 先验 top1 = **0.4542**。

| 方案 | top1 | 相对先验 | top3 | MRR | churn | override 精确率 |
|---|---|---|---|---|---|---|
| 新重排器 α=4, τ=4 | 0.4654 | +1.1pt | — | — | 8.4% | 34.4% |
| 新重排器 α=8, τ=0 | 0.4645 | +1.0pt | — | — | 5.2% | 37.5% |
| **旧 MLM 单独（β=0）** | **0.5084** | +5.4pt | 0.7794 | 0.6636 | 52.1% | 36.4% |
| **MLM + 0.25·先验** | **0.5234** | **+6.9pt** | 0.8037 | 0.6800 | 24.4% | 45.6% |
| MLM + 0.5·先验（保守） | 0.4907 | +3.7pt | 0.8047 | — | 11.0% | **50.8%** |
| MLM + 先验 + 重排器（rr≥0.25） | 0.353~0.412 | 变差 | — | — | — | 24~28% |

> MLM 指 `lm-train/checkpoints/final`（v1 final），评分方式为掩码伪似然 `[CLS] ctx [MASK] [SEP]` 的 log P(候选字)。

---

## 5. 诊断与结论

### 5.1 诊断证据
- **过拟合测试**（1,000 样本训 20 epochs）：训练集上纯上下文（α=0）top1 = 22.2%，说明网络有学习能力；但正式训练 20 万样本、2 epochs 后，纯上下文仅 3%（≈随机 1/21）
- **训练集 5,000 子集**与验证集表现几乎一致（rank1-2 桶 7.7% vs 5.9%）→ 不是过拟合，而是**训练不充分/优化不稳定**
- 6 epochs 后 val 大幅提升，但外部测试集仅 +1pt → **域不匹配**：训练仅来自 CLUECorpusSmall，测试来自 wiki/LCCC/LCSTS/新闻
- 集成实验中重排器输出为**负贡献**（即使权重 0.25 也把 MLM+先验从 0.5234 拉低到 0.4121）

### 5.2 结论
1. 在本次数据/预算下，"从基模直接 listwise 重排"不敌"MLM 打分 + 先验融合"
2. 旧 MLM 的价值来自：150 万样本、目标即 P(字|上下文)、迁移性好
3. 重排器的三个短板：初始化（基模而非 MLM）、数据（20 万、单域）、稳定性（loss 尖峰）

---

## 6. 下一步（建议按序）
1. **重排器改造**：编码器改用 v1/v2 MLM 初始化；数据扩至 50 万并混入多域（wiki/LCCC/LCSTS/新闻，与测试集严格去重）；修复稳定性（head 预热、降低 lr、fp32 head、增大 shrink）
2. **MLM 路线加强**：继续扩数据/域训更久的 MLM，配合 `MLM + β·先验`（β=0.25~0.5）作为部署方案
3. **保守集成**：以 MLM+先验为主，重排器仅在高置信时叠加（需要重新训练出正贡献的版本）

当前最佳可直接落地方案：**MLM + 0.25·先验（收益档）或 + 0.5·先验（稳定档）**，仅用于拼音单字；五笔保持不动。

---

## 7. 复现命令

```powershell
# 请求生成（在 third-party\lm-train）
python prepare_reranker_requests.py --sample-rate 1.0 --target 450000

# 真实候选抽取
realcand\rime_real_candidates.exe --shared weasel\output\data `
  --user third-party\rime-user-real --schema luna_pinyin_simp `
  --input reranker\requests.tsv --output reranker\candidates_luna_pinyin_simp.tsv `
  --max 60 --pages 12

# 打包 + 训练 + 评测
python build_reranker_npz.py
python train_reranker.py --train-samples 200000 --epochs 6 --batch 32 `
  --run-name reranker-200k-e6 --out reranker\model6
python sweep_reranker.py --model reranker\model6\model_best.pt
# 真实测试集（在 third-party\lm-test）
python eval_reranker.py --alphas 4 --mlm ..\lm-train\checkpoints\final --mlm-weight 0,0.25,0.5,1
python lm_only_eval.py --betas 0,0.25,0.5,1,2
```

## 8. 文件清单
| 文件 | 用途 |
|---|---|
| `lm-train/prepare_reranker_requests.py` | 训练请求生成（严格切分/去重） |
| `lm-train/build_reranker_npz.py` | 候选+特征打包 |
| `lm-train/train_reranker.py` | 重排器训练（残差 listwise，MLflow） |
| `lm-train/sweep_reranker.py` | α/τ 扫描（验证集） |
| `lm-train/diag_reranker.py` | 诊断（context-only、分桶） |
| `lm-test/eval_reranker.py` | 真实测试集评测（重排器/集成） |
| `lm-test/lm_only_eval.py` | 纯 MLM / MLM+先验基线 |
| `lm-test/rime_real_candidates.cc` | 抽取器（含 quality/type） |

> 注：以上新增脚本/数据尚未提交 git；MLflow 实验 `lm-ranker` 中可查看 `reranker-200k`、`reranker-200k-e6` 的曲线与指标。

---

## 9. 长上下文重训实验（v2，2026-09-14）

### 9.1 背景与改动
v1 的上下文实际中位仅 **11 字（训练）/ 9 字（测试）**，且请求生成在句中**第一个**词命中处截断，导致目标几乎都靠近句首。v2 修正：

- 目标位置：在句中所有词命中里**均匀随机**取
- 上下文：**跨句拼接**（同文档内前文 + 当前句前缀），上限 **256 字**、下限 32 字；文档边界重置
- 切分：训练/验证按**文档**切分（上下文不跨越切分）；测试集同规则重建并扩到 **2000 例**（4 域 × 字/词各 250）
- 严格去重：本地窗口（target+前 64 字）与完整 span（≤257 字）都对 CLUECorpusSmall 做 grep 匹配后剔除

### 9.2 数据与训练
| 项 | 值 |
|---|---|
| 训练请求 | 320,000（train 314,864 / val 5,136，全库均匀采样） |
| 上下文长度 | 中位/均值 **256** 字（此前方差极大） |
| 候选抽取 | 319,883 / 320,000（100.0%） |
| 打包 | train 314,751 / val 5,132，C=21（top-20+正样本），ctx=256 |
| 训练 | 从 **base** 初始化（隔离变量），200k 样本 / 2 epochs，batch 16 × accum 2（等效 32），1h10m |
| MLflow | run `reranker-v2-ctx256` |

### 9.3 结果（新测试集，字级 1000 例）
| 方案 | top1 | top3 | MRR | churn | override 精确率 |
|---|---:|---:|---:|---:|---:|
| IME 先验 | 0.4890 | — | — | — | — |
| **新重排器（v2，2ep）** | 0.4850 | — | — | 0.5% | 0% |
| MLM（v1_final）单独 | **0.8220** | 0.9540 | 0.8905 | 48.5% | 76.7% |
| MLM + 0.25·先验 | 0.7970 | 0.9510 | 0.8758 | 36.2% | 86.5% |
| MLM + 1·先验（保守） | 0.5650 | 0.9260 | 0.7474 | 9.6% | 79.2% |

验证集上：context-only（α=0）从 v1 的 **3% 提升到 16%**（长上下文确实提供了信号），但重排器仍几乎不翻盘（rank1-2 桶命中 0%），2 epochs 内没有"开窍"。

### 9.4 结论
1. **长上下文红利几乎全部被 MLM 拿走**：旧 MLM 在 256 字上下文上 top1 从 0.51 跃升到 **0.82**，与先验融合后仍达 0.80（精确率 86.5%）。
2. 从 base 直接 listwise 重排仍然弱：即便上下文加长，2 epochs 的模型也未能学会在强先验下翻盘（与 v1 需要 6 epochs 才出现的"跃升"一致）。
3. 当前最强可用方案：**MLM + 0.25·先验**（收益档）或 `+1·先验`（保守档，churn 9.6%）。
4. 重排器若要翻盘，下一步应：更多 epochs（6–10）或**用 MLM 初始化编码器**，并可考虑更大基座（L8/512）。

### 9.5 可视化
`doc\reranker-examples-v2.md`：展示**模型实际可见的完整 256 字上下文**、候选分数与选择，分"修正成功/保持正确/改错"三类（各 4 例；1000 例中修正 283、保持 492、改错 225）。

---

## 10. E1：候选集 constrained softmax 微调（2026-09-14）

### 10.1 动机与设计
- 长上下文测试集上 MLM 已达 0.822，说明"P(字|上下文)"信号充分；而 cross-encoder 需要重新学习候选匹配且每候选一次前向（贵 21 倍）
- 新目标：在 MLM 的 mask 位置上，**只对候选集做 softmax**：
  `L = -log softmax(z_{c} | c ∈ 候选集)`，正样本为语料中的目标字
- 每个 case 仅一次前向；可从已有 MLM checkpoint 初始化；每 0.25 epoch 在验证集评测并保存最优

### 10.2 结果（新测试集，字级 1000 例，IME 先验 0.4890）
| 模型 | top1 (β=0) | +0.25·先验 | churn | override 精确率 | β=1 保守档 |
|---|---:|---:|---:|---:|---:|
| MLM v1（基线） | 0.8220 | 0.7970 | 36.2% | 86.5% | 0.5650 |
| MLM v2（基线） | 0.8150 | 0.8030 | — | 87.8% | — |
| **MLM-FT v1**（3ep） | **0.8670** | 0.8280 | 37.9% | 90.5% | 0.5670（churn 9.5%） |
| **MLM-FT v2**（3ep） | 0.8580 | **0.8330** | 37.7% | **91.8%** | 0.5680（churn 9.8%） |

分桶（ft_v2 vs v1 基线）：overall **0.858 / 0.821**；先验 rank1 0.949 / 0.916；rank2-3 0.801 / 0.782；rank4-9 **0.767 / 0.700**；rank10+ 0.662 / 0.600。

验证集（5,132 例）：最优 0.8975（v1 init）/ **0.8985**（v2 init），约 1.5 epoch 后进入平台（0.896–0.898）。

### 10.3 结论
1. **E1 方案有效**：constrained softmax 微调把纯 logits top1 从 0.822 提到 0.867（+4.5pt），融合 0.25 先验后 0.833（+3.0pt），override 精确率提升到 91.8%。
2. 所有先验排名桶均提升，越难的桶提升越大（rank4-9 +6.7pt）。
3. 这是目前最强系统：**MLM-FT v2 + 0.25·先验**；保守档可用 `+1·先验`（top1 0.568、churn 9.8%）。
4. 可视化更新：`doc\reranker-examples-v3.md`（修正 305 / 保持 494 / 改错 201，对比基线 283/492/225）。

---

## 11. 词级与整句评测（2026-09-15）

### 11.1 词级：MLM 超过 IME 先验
- 测试集：v2 测试集 word 例 1000（4 域 × 250）。候选需过滤为与目标**同词长**（否则单字候选在 seq 打分下占优，先验 top1 异常为 0.16；过滤后 0.861）
- seq 伪似然打分：`P(w|ctx) = ∏ P(c_k | ctx, c_1..c_{k-1})`，每字一次前向

| 模型 | top1（纯 LM） | +0.25·先验 | top3 | mrr | churn | override 精度 |
|---|---:|---:|---:|---:|---:|---:|
| IME 先验 | 0.861 | — | — | — | — | — |
| MLM base（v1 final） | 0.928 | 0.925 | 0.996 | 0.960 | 9.6% | 79.2% |
| **MLM-FT v2** | 0.920 | **0.928** | 0.989 | 0.959 | 9.8% | 80.6% |

- 按词长（FT 纯 LM）：2 字 0.907（n=838）、3 字 0.991（115）、4 字 0.979（47）
- 结论：**词级也能超过先验（+6~7pt）**，但提升空间小于字级（+33pt），因为词级先验本身已强（0.861）

### 11.2 IME 并不生成多个整句
- librime 的句子组合器实际只输出 **1 个整句候选**，菜单其余为分段（词/字）候选：
  - 词对齐 400 例中，397 例"同长度候选"恰好 1 个（2 例 2 个，1 例 0 个）
  - 难例整句 400 例中，凡目标出现在候选里的（124 例），名次全部为 0 → IME 的整句要么对、要么根本组不出来
- 因此"对整句 rerank"在 IME 侧没有候选列表可用

### 11.3 MLM 逐字生成整句（拼音约束解码）——难句上超过 IME
- 方法（`lm_decode_sentences.py`）：像 LLM 一样逐字生成——每步把已生成的字接在上下文后，`[CLS] ctx gen… [MASK] [SEP]`，在**该位拼音允许的字集合**（heteronym 全读音）上取概率，束搜索保留最优整句
- 修正（2026-09-15）：此前解码时上下文未左侧 padding 到 256，与训练格式不一致；改为固定 256 左 padding + attention mask（与字级/词级评测一致）后**全面变好**，下表为修正后数据
- 难例集（随机切片 4–10 字，400 例，目标 100% 可达）：

| 方案 | 整句精确匹配 | 字级准确率 |
|---|---:|---:|
| IME 整句（组合器唯一输出） | 124/400 = 0.310 | 0.752 |
| MLM-FT v2 贪心（beam=1） | 86/400 = 0.215 | 0.640 |
| **MLM-FT v2 beam=4** | **166/400 = 0.415** | **0.778** |

- 交叉对比（beam=4）：都对 75 ｜ MLM 独对 **91** ｜ IME 独对 49 ｜ 都错 185
- 词对齐整句集（400 例，IME 可组合 320 例 = 0.800）：贪心 0.315、beam=4 0.6475（IME 在"能组合"场景几乎不会错）
- 结论：
  1. **在 IME 组合失败的难句上，MLM 逐字生成超过 IME**（41.5% vs 31.0%，+10.5pt），说明"MLM 当生成模型解码"可行；
  2. 但 MLM 会改错 49 例 IME 本已正确的结果（净收益 +42 例）；无脑置顶会在"词对齐"场景伤害 IME 的 80% 准确率，需混合策略（见 11.5 smart 判别）；
  3. 束搜索显著优于贪心（+20pt），padding 修正额外贡献 +3.25pt（beam4：38.25%→41.5%）。
- 样例（含完整上下文、键入拼音、贪心/beam 双输出对比）：`doc\reranker-sent-decode-ftv2.md`（难例集）、`doc\reranker-sent-decode-ftv2-words.md`（词对齐集）

### 11.4 beam × topk 扫参与延迟（2026-09-15，上下文 padding 修正后）
- 脚本：`lm-test/sweep_sent_decode.py`（GPU，只计时解码循环；400 例/组）
- 难例整句集（IME 基线：exact 0.310 / char 0.752）：

| beam | topk | 整句精确 | 字级 | 每句延迟 (GPU) |
|---:|---:|---:|---:|---:|
| 1（贪心） | 8/12/16 | 0.2150 | 0.6395 | ~18 ms |
| 4 | 8/12/16 | 0.4150 | 0.7779 | ~28 ms |
| 8 | 8/12/16 | 0.4650 | 0.8218 | ~42 ms |
| 12 | 8 | 0.4675 | 0.8306 | 56 ms |
| 12 | 12 | **0.4825** | **0.8381** | 58 ms |

- 词对齐整句集（IME 基线 0.800）：贪心 0.315（14.7ms）→ beam4 0.6475（22ms）→ beam8 0.7225（33ms）→ beam12/topk12 0.7425（46ms）
- **结论**：beam 是主旋钮、topk 仅在 beam=12 时有效；1→4 提升最大（+20pt），4→8（+5pt），8→12（+1.75pt）；推荐预设 beam4/topk8（低延迟）、beam8/topk8（平衡）、beam12/topk12（精度）

### 11.5 ONNX 导出、CPU 延迟与 smart 置顶判别（2026-09-15）
- 导出：`lm-train/export_onnx.py` → `onnx/mlm_ft_v2/`（model.onnx 33.6MB / model.int8.onnx 8.5MB / vocab.txt / lm_config.json）
- ORT 一致性（字级 1000 例，β=0.25）：fp32 top1 **0.8320**（PyTorch 0.8330）、int8 0.8250；CPU 单次前向（ctx 256）fp32 **~11ms**、int8 ~11.5ms → **默认用 fp32**
- 整句解码 CPU 延迟（ORT，难例 400）：beam1 72ms ｜ beam4 **238ms** ｜ beam8 465ms ｜ beam12 705ms，精度与 GPU 完全一致 → **默认 beam=4/topk=8**
- `smart` 置顶判别（`lm-test/smart_promote_calib.py`）：比较 MLM 整句与 IME 首选的**每字平均对数似然差** margin，`margin > T` 才置顶；扫参 T∈[-1,+3]：

| T | 难例集 | 词对齐集 | 平均 |
|---:|---:|---:|---:|
| 不置顶（纯 IME） | 0.310 | 0.800 | 0.555 |
| 总是置顶（纯 MLM） | 0.415 | 0.6475 | 0.531 |
| +0.4 | 0.4825 | 0.8375 | 0.660 |
| **+0.5（默认）** | **0.4925**（置顶率 52%） | **0.8400**（置顶率 14%） | **0.666** |
| +0.7 | 0.4800 | 0.8500 | 0.665 |

- 结论：**smart 判别在两个集合上都超过"纯 IME"与"纯 MLM"**，且在 T∈[0.2, 1.0] 区间稳定；插件默认 `promote: smart`，`promote_margin: 0.5`
