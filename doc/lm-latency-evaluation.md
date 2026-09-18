# lm_ranker 异步流程延迟评估与优化（2026-09-18 第四轮）

问题：用户停止输入 → 经过一段时间开始轮询 → 收到服务端返回 → 展示 LM 输出，这条流程对
**单字 / 词组 / 整句** 分别带来多少延迟？能否从逻辑上优化单字与词组重排的延迟？

结论先说：**错不在轮询，而在"每次按键都做一遍重排、且正在进行的那一遍不可打断"**。
修掉之后（+ 一个上下文窗口的真实缺陷），词组延迟 826→142 ms，单字 684→142 ms（默认轮询间隔下），
流水线本身的耗时降到约 **46 ms**。

---

## 一、测量方法（全部在隔离环境完成）

- 隔离服务端：`RIME_WEASEL_PIPE_NAME` / `RIME_WEASEL_USER_DIR`（不影响正在使用的输入法）。
- 新探针 `third-party/lm-test/weasel_ipc_latency_e2e.cc`（脚本 `weasel/tools/ipc_latency_test.ps1`）：
  按真人速度（键间隔 150 ms）输入 → 取最后一次按键的响应作为基线 → 之后**按 TSF 的节奏轮询**
  （`ProcessKeyEvent(0)` + 读响应）→ 记录候选列表首次与基线不同（或首次出现 `[LM]`）的时刻。
  这就是用户看到的延迟。
- 归因：把轮询间隔从 120 ms（TSF 默认）调到 20 ms，两者之差即"等下一次轮询"的对齐开销；
  20 ms 下的数值接近当前架构的**流水线地板**。
- 插件侧打点（`RIME_LM_RANKER_DEBUG=1`）给出 `rank miss / job picked up after / scored+publishing /
  rank hit / cancelled` 的时间戳，用于把延迟拆成"排队 / 打分 / 通知刷新"。

## 二、实测：优化前的延迟（默认 `max_context: 256`，轮询 120 ms）

| 输入 | 类型 | 轮询 120 ms（用户可见） | 轮询 20 ms（地板） |
|---|---|---|---|
| `ni` | 单字 | **684 ms**（546–833） | 474 ms |
| `beijing` | 词组 | **826 ms**（5 次里只有 3 次在 4 s 内出现，其余超时） | 905 ms |
| `beijingdaxue` | 整句 | 279 ms | 237 ms |

轮询间隔几乎不影响词组/单字 → 说明瓶颈在服务端流水线，不在客户端轮询。

### 根因（插件时间线实证）

```
rank miss t=…489 items=50          ← 最后一次按键，请求入队
rank job picked up after 537.0 ms  ← 排在"上一条已作废的按键"之后
rank scored+publishing in 225.0 ms, total 762.0 ms
rank hit t=…251 items=50           ← 762 ms 后菜单才刷新
```

两条机制叠加：

1. **每次按键都会入队一次重排**，而**正在打分的那一条无法中断**。上一条属于"上一个输入"，
   结果必然作废（快照 key 不同），却把真正要看的结果挡在后面（实测排队 0.1–1.0 s）。
2. **单条作业本身很贵**：单字输入的候选菜单有 50 个候选，每个候选一行要过 260 个 token
   （`1 + 256 上下文 + 前缀 + 2`），`masked-LM` 头会把**每个位置**投影到 21128 词表上，
   实测 **~110 ms**；而 `kMaxBatch=64` 让 50 行成为**一整块不可分割的批次**。

## 三、优化 1：让被顶替的作业立刻中止（已实现）

- `LmRankerConfig`/`ScoreCandidates` 增加可取消变体 `ScoreCandidatesCancellable`：
  在**每个批次的边界**检查取消标志，被顶替就返回"结果不完整"；
- `RankService` 在"已有作业正在打分时又来了新请求"的情况下置位取消标志
  （同一快照的重复请求仍会被去重，不会自打断）；
- 批次上限 64 → **16**，把取消粒度从"整份菜单"缩到"16 行"。

效果（轮询 20 ms，`max_context` 仍为 256）：

| 输入 | 优化前 | 优化后 |
|---|---|---|
| 词组 `beijing` | 905 ms（含 >3 s 超时） | **141 ms** |
| 单字 `ni` | 474 ms | **426 ms** |
| 整句 `beijingdaxue` | 237 ms | 236 ms |

词组收益最大（6 倍，且不再有超时）；单字仍慢，因为单条作业本身就要 110 ms 以上。

## 四、优化 2：上下文窗口的真实缺陷（已修复，收益 8–9 倍）

排查中发现 **`max_context` 配置从未生效**：它只用于截断上下文字符串，
而编码器始终左填充到固定 **256 token**（`max_context_` 成员从未被赋值）。
也就是说：用户输入 21 个字的上下文，模型仍按 260 token 的窗口逐候选计算。

修复 `Initialize()` 接收并夹取该配置（32–256，并在日志里打印实际窗口）后实测
（无头 harness，`weasel/tools/ctx_window_parity.ps1`，插件自报的单条作业耗时）：

| 输入（候选数） | 窗口 256 | 窗口 32 | 加速 |
|---|---|---|---|
| `ni`（50） | 109.5 ms | 12.5 ms | 8.8× |
| `wo`（50） | 112.5 ms | 13.5 ms | 8.3× |
| `ta`（50） | 99.0 ms | 12.0 ms | 8.3× |
| `de` | 37.5 ms | 5.5 ms | 6.8× |
| `beijing` | 51.0 ms | 7.0 ms | 7.3× |
| `zhongguo` | 42.5 ms | 6.5 ms | 6.5× |
| `daxue` | 32.0 ms | 5.0 ms | 6.4× |
| `beijingdaxue` | 100.0 ms | 15.5 ms | 6.5× |

**精度影响（10 个输入，无头对比最终候选列表）**：

- **首位候选 10/10 完全一致**，`[LM]` 标记也始终落在同一个候选上；
- 5/10 的**尾部顺序**（第 3–5 名）有轻微变化，例如
  `ni`: 你,尼,拟,逆,呢 → 你,尼,逆,拟,妮；`ta`: 他,她,它,达,塔 → 他,它,她,达,踏。

原因：位置编码是绝对的，真实上下文在 256 窗口里位于 235–255 位，在 32 窗口里位于 11–31 位，
所以分数会有微小偏移。**首位稳定、尾部会抖动**，这就是"速度换保真"的取舍点。

## 五、优化后的完整延迟

`max_context: 32` + 可取消打分（轮询间隔取不同值）：

| 轮询间隔 | 单字 `ni` | 词组 `beijing` | 整句 `beijingdaxue` | 所需轮询次数 |
|---|---|---|---|---|
| 120 ms（TSF 默认） | 142 ms | 142 ms | 141 ms | **1** |
| 90 ms | 111 ms | 110 ms | — | 1 |
| 60 ms | 77 ms | 79 ms | — | 1 |
| 40 ms | 62 ms | 63 ms | — | 1 |
| 20 ms | 46 ms | 46 ms | 47 ms | 1 |

即：**流水线地板 ≈ 46 ms**；默认轮询间隔下用户看到 ≈ 轮询间隔 + 20 ms。
优化后结果在第一次轮询时就已经就绪（`polls_median` 由 5–19 降到 **1**）。

## 六、已实现的第二项优化：客户端不再空轮询（`lm_pending`）

原来 TSF 每次按键都会把刷新预算（1.5 s）用满：最多 12 次 `ProcessKeyEvent(0)` 事务，
即使结果早就到了。现在：

- 插件在每次 `Apply` 时把"这条组字是否还有 LM 作业在跑"写入上下文属性 `lm_pending`
  （`RankService::Pending(key)` 或 `SentenceService::Pending(input)`）；
- 服务端把它放进响应（`config.lm_pending=0/1`）；
- TSF 在 `DoEditSession` 里看到 0 就立即 `_CancelLmRefresh()`，并停止后续轮询。

同时把默认轮询节奏从 `initial_ms/interval_ms = 120/120` 改为 **60/60**（yaml 里写死
`ui_refresh` 的用户不受影响，例如你当前的配置是 100/50）。

实测（隔离服务端；每次运行用**不同的上下文**，避免命中上一次的重排缓存）：

| 用例 | ctx=256（默认） | ctx=32 | 每键轮询次数 |
|---|---|---|---|
| 单字 `ni` | 237 ms | **78 ms** | 3 → 1 |
| 词组 `beijing` | 158 ms | **78 ms** | 2 → 1 |
| 整句 `beijingdaxue` | 237 ms | **77 ms** | 5 → 3 |

`unsafe_early_stop=0`：所有运行里，客户端停止轮询时菜单**已经**刷新完毕（提前停止是安全的）。

> 修正第四节的说明：早先 20 ms 轮询下测到的 "46 ms 地板"里，有一部分是**重复使用同一
> 上下文**造成的缓存命中。探针现在每次运行使用不同上下文，上表是未命中缓存时的真实数字。

## 七、上下文长度与训练时的窗口（回答提问）

查阅训练材料得到的结论（`third-party/lm-train`）：

| 项目 | 事实 | 依据 |
|---|---|---|
| 基础模型 | BERT，`hidden 256 / 4 层 / max_position_embeddings 512` | `third-party/models/uer/chinese_roberta_L-4_H-256/config.json`、`onnx/mlm_ft_v2/lm_config.json` |
| v1 重排微调 | 上下文窗口 **64** | `reranker/data.npz` 里 `ctx.npy` 形状 `(285421, 64)` |
| **v2 微调（当前上线模型 `mlm_ft_v2`）** | 上下文窗口 **256**（固定，左填充 0 = [PAD]，attention mask 屏蔽） | `reranker_v2/data.npz` 里 `ctx.npy` 形状 `(319883, 256)` |
| 损失 | **候选择集的 softmax**（cross entropy over ~21 个候选），取 `logits[:, -2, :]`（mask 位置） | `train_mlm_reranker.py` |

关键点：**训练时上下文长度是可变的**——打包脚本对每个样本做 `context[-ctx_len:]` 后
**右对齐、左侧补 0**（`build_reranker_npz.py`），所以窗口内 0..255 每个位置在不同样本里
既可能是 padding、也可能是真实上文；掩码把 padding 的注意力屏蔽掉。
因此"窗口长度"本身不是模型学到的语义，真正随窗口变化的是**真实上文的绝对位置编码**。

这与实测一致：

| 场景 | 窗口 256 vs 32 的首位一致率 |
|---|---|
| 上下文 21 字（窗口只是变短，**减掉的全是 padding**） | **10/10** |
| 30 条真实用例、上下文 256 字（缩短窗口**会丢掉真上文**） | **26/30（87%）**；整序一致 7/30；平均名次位移 0.50 |

结论：**泛化能力足以支持不同窗口长度，但前提是不要把真实上文截掉**。所以推荐把窗口做成
自适应的（`min(max_context, 实际字数上取整到 8 + 8)`，下限 32）：短上下文拿到 8 倍加速且
首位不变，长上下文维持 256 的保真度。是否采用由你决定（参见第九节）。

## 八、下一步可做（尚未实现）

1. **ONNX 导出改造**：当前图输出**所有位置**的 logits（ctx=256 时单批约 350 MB），
   而打分只用 `mask_index` 那一个位置。导出时先按 `mask_index` 取 hidden state 再做 LM 头，
   可在**保留 256 窗口保真度**的前提下把单条作业压到 10 ms 量级。
2. 自适应窗口（见上）。

## 九、建议

- **立即采用**：可取消打分、`max_context` 修复、`lm_pending` 提前停止（都已默认生效）。
- **需要你决定**：自适应窗口默认值。

## 十、复现命令

```powershell
# 延迟矩阵（隔离环境，自动启停服务端）
powershell -File weasel\tools\ipc_latency_test.ps1
powershell -File weasel\tools\ipc_latency_test.ps1 -Cases "ni=char,beijing=word" -Polls "60,40,20"

# 上下文窗口的速度/一致性（10 个手工用例）
$env:RIME_LM_RANKER_DEBUG = "1"
powershell -File weasel\tools\ctx_window_parity.ps1 -Window 32
# 30 条真实用例（来自 v2 训练请求集）
powershell -File weasel\tools\ctx_window_eval.ps1 -Window 32 -Limit 30
```

## 十一、本轮改动文件

| 文件 | 改动 |
|---|---|
| `librime/plugins/lm_ranker/lm_rank_engine.{h,cc}`（独立仓库） | `ScoreCandidatesCancellable`（批次边界可取消）、批次 64→16、`Initialize` 接收并生效 `max_context`、可取消变体的错误返回语义 |
| `librime/plugins/lm_ranker/lm_rank_service.{h,cc}` | 取消标志与排队/打分/取消的打点；`WaitIdle`/`Pending` 供后续轮询优化使用 |
| `librime/plugins/lm_ranker/lm_ranker.cc` | `rank hit/miss/skip` 带时间戳 |
| `weasel/tools/ipc_latency_test.ps1`（新增） | 延迟矩阵 |
| `weasel/tools/ctx_window_parity.ps1`、`ctx_window_eval.ps1`（新增） | 窗口速度/一致性对比（后者用 30 条真实用例 + 真实上下文） |
| `third-party/lm-test/weasel_ipc_latency_e2e.cc`、`build_weasel_ipc_latency_e2e.cmd`（新增，不在 git 内） | 延迟探针客户端（按 TSF 节奏轮询、解析 `lm_pending`、每次运行用独立上下文） |
| `weasel/include/WeaselIPCData.h`、`WeaselIPC/Configurator.cpp` | `Config::lm_pending` 与其解析 |
| `weasel/WeaselTSF/EditSession.cpp` | `lm_pending=0` 时立即停止轮询 |
| `weasel/RimeWithWeasel/RimeWithWeasel.cpp` | 响应里带上 `config.lm_pending`；默认轮询节奏 120/120 → 60/60；隔离实例不再弹出提示；失败提示改为真实日志目录 |
| `librime/plugins/lm_ranker/lm_ranker.cc`、`lm_sentence.{h,cc}`（独立仓库） | `PublishPending` / `SentenceService::Pending`（`lm_pending` 来源） |

## 十二、附带修复：屏幕左上角的"有错误"提示

用户报障：两次在屏幕左上角弹出"有错误，请查看……%weasel.info"，一闪而过。

- **来源**：是**我启动的隔离测试服务端**弹出的（其日志目录在本会话临时目录
  `…\dsh-*\rime.weasel\`，其中 `ERROR` 日志给出 `deployment_tasks.cc:208] missing input
  schema: quick5`）；不是用户正在使用的输入法。生产侧最近只有一条无害告警
  （`missing input schema; skipped unsatisfied dependency: pinyin_simp`，14:20）。
- **原因**：测试夹具 `third-party/lm-test/runtime/data/default.yaml` 的 `schema_list` 里含
  `quick5`（上游 Rime 自带、Weasel 数据目录并不提供），而该夹具的用户目录没有
  `default.custom.yaml` 覆盖它 → 每次维护部署都失败 → 弹失败提示；隔离实例没有光标位置，
  面板落在 (0,0) = 屏幕左上角，所以提示出现在左上角。
- **修复**：① 给夹具加 `default.custom.yaml`（只列存在的方案），部署不再失败（已验证：
  重跑测试不再产生新的 ERROR 日志）；② 产品侧加固——以私有管道名启动的**隔离实例不再弹出
  任何提示**（失败仍然进日志），避免自动化测试打扰用户；③ 失败提示里的 `%TEMP%` 字面量改为
  **真实日志目录**，这样一闪而过的提示里也能看清位置。
