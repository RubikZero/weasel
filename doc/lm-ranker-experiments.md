# Rime 候选重排语言模型实验记录（v1–v4）

- 目标：为 Rime/Weasel 挂载小型中文语言模型，利用上下文对候选词重排
- 基座：`uer/chinese_roberta_L-4_H-256`（4 层 / hidden 256 / 4 头，Google 中文词表 21128，单字 token 8000，约 9M 参数）
- 代码与数据均位于 `D:\Workspace\rime\third-party` 下（`lm-train`、`lm-test`、`models`、`corpus*`）

---

## 1. 训练集构造（`lm-train/prepare_data.py`）

### 1.1 数据来源
| 项目 | 说明 |
|---|---|
| 文本语料 | `corpus/CLUECorpusSmall.txt`（13.74 GB，一行一句/段） |
| 目标词表 | `weasel/output/data/luna_pinyin.dict.yaml` → zhconv 转简体 → 1–4 字纯汉字词（42,077 个） |
| 字符映射 | 基座 `vocab.txt` 的单字符 token（8,000 个）；`[PAD]=0 [UNK]=100 [CLS]=101 [SEP]=102 [MASK]=103` |

### 1.2 采样与目标选择
1. 流式扫描语料，以概率 `--sample-rate` 抽样；仅保留 16–300 字的行。
2. 选目标词：对句中每个位置按 **4→3→2 字最长匹配**词表（用 2 字前缀集合加速），收集全部命中位置后随机选一个；整句无命中则随机取 1 个汉字。
3. 短上文偏置：以概率 `--short-ratio` 把目标限制在句首前 12 字内（让上下文更短，贴近输入法场景）。
4. 上下文 = 目标前最多 64 字；上下文 <2 字或含 OOV 字符则丢弃。

### 1.3 样本格式（任务自适应 MLM）
```
input_ids      = [PAD]*(64-|ctx|) [CLS] ctx [MASK]*len(target) [SEP] [PAD]...
attention_mask = 0 0 ... 1 ... 1 0...
labels         = -100 ... 目标字符 id ... -100
固定长度 SEQ = 72
```
目标词所有字符**同时**被 mask（与推理时"打分候选"格式一致）。

### 1.4 划分与各版本数据
- 划分：扫描中**最先接受的 5000 条进 val**，其余进 train（非随机划分，val 偏向语料前部）。
- 输出：`data/train.npz`、`data/val.npz`（int16，`[N,72]` 的 ids/mask/labels）。

| 版本 | 数据文件 | 样本数 | short-ratio | 最小上下文 | 采样率 |
|---|---|---|---|---|---|
| v1 | `data/train.npz`（第 1 次生成） | 1,500,000 | 0（无短偏置） | 8 字 | 0.22 |
| v2 | `data/train.npz`（重新生成覆盖） | 1,809,924 | 0.5 | 2 字 | 0.22 |
| v3 | 同 v2 数据 | 1,809,924 | 0.5 | 2 字 | — |
| v4 | `data5m/train.npz` | 5,000,000 | 0.3 | 2 字 | 0.70 |

---

## 2. 测试集构造（`lm-test/`）

三套测试集，均由外部语料/词表构造，并**严格去重**（见 2.4）。

### 2.1 合成字级测试（`eval_models.py`）
- 词表（luna_pinyin）单字词按拼音编码归组，取组内 ≥5 字；
- 取 3–6 字词，context = 词前缀，target = 末字，候选 = target + 4 个同音字（5 选 1）；
- N=2000，固定种子 42；支持 `--simp` / `--crosssimp`（转简评测）。

### 2.2 合成词级测试（`build_testset_words.py` → `testset/testset_words.json`）
- 用 `pypinyin` 给 `essay.txt` 常用词表（442,693 条）注音，把**同拼音、同字长**的词归组：
  - ≥5 词的组：1 字 318 组、2 字 7,856 组、3 字 29 组、4 字 4 组；
- 从四个测试语料的真实句子中取目标词出现位置，context = 前 64 字，候选 = 同拼音同事长 5 个词；
- 域均衡配额 + 严格去重后：**608 条**（1 字 200 / 2 字 300 / 3 字 100 / 4 字 8）。

### 2.3 真实候选测试（`build_requests.py` + `realcand/rime_real_candidates.cc` + `eval_real.py`）
流程：构造请求 → librime 抽取真实候选 → 转简匹配 → 评测。

1. **请求构造**（2083 条，`testset/requests.tsv`）：
   - 字桶：取常用词中的一个字，keys = 该字在词内的音节（避免多音字误注）；
   - 词桶：2–4 字常用词，keys = 整词拼音；上下文 = 句中前 64 字；
   - `sed` 严格去重（grep）后保存；另有 `requests_meta.tsv` 记录 kind/domain/context。
2. **真实候选抽取**（C++，`rime_api`）：
   - 隔离用户目录 `rime-user-real`（`default.custom.yaml` 注册 3 个 schema）；
   - 每例 `clear_composition` → `simulate_key_sequence(keys)` → 翻页收集候选（上限 27 个 = 3 页）；
   - 输出候选顺序与目标命中位置；三个 schema 各跑一遍：
     `luna_pinyin_simp`、`luna_quanpin`、`luna_pinyin_fluency`。
   - 命中率（原始）：simp 99.6%、quanpin 99.6%、fluency 59.8%（繁体输出，转简后与前者相当）。
3. **测试集**：每个 schema 600 条（300 字 + 300 词，`testset_real_<schema>.json`），目标未命中的样例保留在统计中（not_found: char 8 / word 1，基于 1078/1005 条全量）。

### 2.4 严格去重（防止与训练语料重叠）
将每条样例的 `context+target` span 写入文件，用 GNU grep 对 13.7GB 训练语料做子串匹配：
```
grep -F -h -o -f spans.txt CLUECorpusSmall.txt
```
凡命中（含包含关系）的样例一律剔除。实际剔除：四域集 32,237/35,443 保留（-3,206）；词级集 1,402/1,812 → 608；真实请求 2,083/2,400。

---

## 3. 训练设置（`lm-train/train_mlm.py`）

### 3.1 损失函数
- **训练**：标准 MLM 交叉熵，仅对目标字符位置计算（其余 `-100`）；目标词所有字符同时 mask（mask-all）。
- **推理打分**（`lm-test/eval_testset.py` / `eval_real.py`）：
  - `maskall`：候选所有字符一起 mask，一次前向，累加各位置 log P；
  - `seq`（顺序伪似然）：逐字 mask（其余候选字可见），m 字候选做 m 次前向，累加 `log P(char_i | ctx, 其他字)`；
  - 单字候选两者等价；多字候选 `seq` 明显更优（词级测试 0.653→0.788）。

### 3.2 训练超参（v1–v4 相同除非注明）
| 项目 | 值 |
|---|---|
| 硬件 | RTX 5060 Ti 8GB（torch 2.14.0+cu130，bf16 autocast） |
| batch / 序列长度 | 64 / 72 |
| 优化器 | AdamW，weight_decay 0.01 |
| 学习率调度 | 线性 warmup 5% + 线性衰减 |
| 梯度裁剪 | 1.0 |
| 验证 | 每轮结束在 5000 条 val 上算 MLM loss |

### 3.3 各版本训练配置
| 版本 | 初始化 | 数据 | epochs | lr | 保存间隔 | 总步数 | val loss |
|---|---|---|---|---|---|---|---|
| v1 | 基座 | `data/train.npz` (1.50M) | 2 | 3e-5 | 5000 | 46,874 | 2.6582 |
| v2 | 基座 | `data/train.npz` (1.81M, 短上文 0.5) | 2 | 3e-5 | 5000 | 56,560 | 2.9555 |
| v3 | v1 final | 同 v2 数据 | 1 | 1e-5 | 5000 | 28,280 | 2.9767 |
| v4 | 基座 | `data5m/train.npz` (5.00M, 短上文 0.3) | 2 | 3e-5 | 10000 | 156,250 | 2.6518 |

### 3.4 Checkpoint 管理
- 本地仅保留 **best + final**（7 个）：v1 `step-5000`/`final`、v2 `step-5000`/`final`、v3 `final`、v4 `step-70000`/`final`；
- 其余 37 个已归档到 **DVC**（本地 remote `third-party/dvc-store`，1.21GB），`dvc pull` 可恢复。

---

## 4. 指标

### 4.1 四域测试集（5000 条：wiki/LCCC/LCSTS/THUCNews 各 1250，字级 5 选 1，mask-all）
| 模型 | top1 | top3 | MRR | lccc | lcsts | thucnews | wiki |
|---|---|---|---|---|---|---|---|
| 频率基线（essay） | 0.718 | 0.972 | 0.840 | — | — | — | — |
| base_L4（未微调） | 0.8984 | 0.9952 | 0.9448 | 0.915 | 0.898 | 0.899 | 0.882 |
| base_L2 | 0.8796 | 0.9924 | 0.9343 | 0.905 | 0.877 | 0.882 | 0.854 |
| **v1_step-5000** | 0.9136 | 0.9954 | 0.9531 | 0.928 | 0.919 | 0.912 | 0.895 |
| v1_final | 0.9094 | 0.9954 | 0.9512 | 0.920 | 0.915 | 0.910 | 0.892 |
| **v2_step-5000** | **0.9182** | **0.9960** | **0.9558** | 0.938 | 0.919 | 0.914 | 0.902 |
| v2_final | 0.9132 | 0.9960 | 0.9531 | 0.925 | 0.927 | 0.907 | 0.894 |
| v3_final | 0.9122 | 0.9956 | 0.9528 | 0.924 | 0.924 | 0.907 | 0.894 |
| v4_step-70000 | 0.9036 | 0.9948 | 0.9484 | 0.914 | 0.917 | 0.899 | 0.884 |
| v4_final | 0.9014 | 0.9948 | 0.9466 | 0.917 | 0.915 | 0.894 | 0.879 |

### 4.2 词级测试集（608 条，候选为同拼音词，seq 打分）
| 模型 | top1 | top3 | MRR |
|---|---|---|---|
| 频率基线 | 0.748 | 0.934 | 0.849 |
| base_L4 | 0.7303 | 0.9688 | 0.8465 |
| base_L2 | 0.7599 | 0.9589 | 0.8612 |
| **v1_step-5000** | **0.7878** | 0.9737 | 0.8796 |
| v1_final | 0.7697 | 0.9770 | 0.8683 |
| v2_step-5000 | 0.7763 | 0.9737 | 0.8728 |
| v2_final | 0.7862 | 0.9737 | 0.8795 |
| v3_final | 0.7664 | 0.9753 | 0.8680 |
| v4_step-70000 | 0.7632 | 0.9671 | 0.8650 |
| v4_final | 0.7451 | 0.9688 | 0.8565 |

### 4.3 真实候选测试（600 条/schema：300 字 + 300 词；seq 打分）
`luna_pinyin_simp` 与 `luna_quanpin` 候选完全一致，下表合并；`luna_pinyin_fluency` 单列。

| 模型 | simp/quanpin top1 | top3 | MRR | fluency top1 | top3 | MRR |
|---|---|---|---|---|---|---|
| **IME 自身排序** | **0.6483** | 0.8600 | 0.7666 | **0.6583** | 0.8617 | 0.7723 |
| base_L4 | 0.3283 | 0.4617 | 0.4431 | 0.3350 | 0.4800 | 0.4534 |
| base_L2 | 0.2450 | 0.3883 | 0.3690 | 0.2533 | 0.3983 | 0.3786 |
| v1_step-5000 | 0.4217 | 0.6767 | 0.5721 | 0.4283 | 0.6783 | 0.5776 |
| v1_final | 0.4167 | 0.7117 | 0.5907 | 0.4217 | 0.7133 | 0.5923 |
| **v2_step-5000** | **0.4450** | 0.6900 | 0.5898 | **0.4467** | 0.6817 | 0.5891 |
| v2_final | 0.4117 | 0.7400 | 0.5938 | 0.4167 | 0.7367 | 0.5950 |
| v3_final | 0.3983 | 0.7150 | 0.5813 | 0.4033 | 0.7117 | 0.5820 |
| v4_step-70000 | 0.3717 | 0.6617 | 0.5519 | 0.3700 | 0.6550 | 0.5487 |
| v4_final | 0.3617 | 0.6383 | 0.5377 | 0.3600 | 0.6283 | 0.5339 |

**分字/词看 IME 基线**（600 例）：字级 top1 0.4067 / top3 0.7367 / MRR 0.5945；词级 top1 **0.890** / top3 0.9833 / MRR 0.9387。

**保守融合**（`z(LM) + β·z(IME 排名先验)`，luna_pinyin_simp，600 例）：

| 配置 | 字级 top1 | 字级 MRR | 词级 top1 | 全体 top1 | 全体 MRR |
|---|---|---|---|---|---|
| IME 基线 | 0.4067 | 0.5945 | 0.890 | 0.6483 | 0.7666 |
| v1_step-5000, β=1 | 0.4867 | 0.6477 | 0.630 | 0.5583 | 0.7093 |
| **v1_final, β=2** | **0.5033** | **0.6617** | 0.710 | 0.6067 | 0.7464 |
| v1_final, β=4 | 0.4933 | 0.6552 | 0.790 | 0.6417 | 0.7690 |
| v1_final, β=8 | 0.4367 | 0.6191 | 0.847 | 0.6417 | 0.7679 |
| 仅重排 top-3（v1_final） | — | — | — | 0.5833 | 0.7319 |

- 融合可把**字级 top1 从 0.407 提升到 ~0.50（+9pt）**；
- 词级永远不要用 LM 重排（IME 0.89，任何重排都会拉低）；
- 纯 essay 词频基线在真实候选上很差（词级 0.133），因为 luna 候选与 essay 词表不对齐。

---

## 5. 结论

1. 微调有效：合成测试上 v2_step-5000 达 0.9182（比基座 +2pt），词级 v1_step-5000 达 0.7878（比基座 +5.8pt）。
2. 真实候选上 IME 自身排序很强（词级 0.89），通用 MLM 直接重排无法超过它；**LM 的价值在字级歧义**。
3. 正确用法：**只对字级（或低置信）候选做融合重排**，`score = z(LM) + β·z(IME 先验)`，β≈1~2；词级保持原序。
4. 后续若要进一步提高，建议**判别式重排训练**：以 librime 真实候选为负样本、目标词为正样本做 listwise 交叉熵（可把 IME 排名作为特征），而不是继续通用 MLM。

---

## 6. 复现命令

```powershell
# 数据（在 third-party\lm-train）
python prepare_data.py --sample-rate 0.22 --max-samples 1500000 --val-samples 5000 --out-dir data
python prepare_data.py --sample-rate 0.7  --max-samples 5000000 --short-ratio 0.3 --out-dir data5m

# 训练（自动写 MLflow）
python train_mlm.py --data data\train.npz --val data\val.npz --epochs 2 --lr 3e-5 `
  --save-steps 5000 --out checkpoints_v2 --log train-v2.log --run-name v2

# 评测（在 third-party\lm-test）
python eval_testset.py --data testset\testset.json       --list models.txt   --out testset\results.jsonl
python eval_testset.py --data testset\testset_words.json --list models_words.txt --out testset\results_words_seq.jsonl --scoring seq
python eval_real.py --schema luna_pinyin_simp --list models_real.txt

# MLflow UI
python -m mlflow ui --backend-store-uri sqlite:///D:/Workspace/rime/third-party/mlflow.db
```

## 7. 版本管理

| 对象 | 工具 | 位置 |
|---|---|---|
| 代码/脚本/测试集 JSON/日志 | git | `third-party`（首提交 `1501026`） |
| 归档 checkpoint（37 个，1.23GB） | DVC（本地 remote） | `third-party/dvc-store`，清单 `lm-train/checkpoints_archive.dvc` |
| 实验指标（曲线/参数/评测值） | MLflow（SQLite） | `third-party/mlflow.db`，实验名 `lm-ranker` |
| 大语料/基座模型/预处理数据 | 不入库（可重下，见各脚本头部 URL/参数） | `corpus*/`、`models/`、`data*/` |
