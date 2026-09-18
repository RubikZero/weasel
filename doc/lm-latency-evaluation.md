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

## 六、下一步可做的两件事（尚未实现）

1. **客户端轮询可以更早停止、也更密一点**（纯客户端改动，收益 ≈ 80 ms）：
   现在 TSF 每次按键固定轮询最多 12 次（1.5 s），即使结果早已就绪；而第一次轮询往往就拿到了结果。
   设计：插件在 `Apply` 中把"是否仍有 LM 作业在跑"写入上下文属性（`set_property`），
   服务端把它放进响应（如 `config.lm_pending=0/1`），TSF 在 `lm_pending=0` 时立即
   `_CancelLmRefresh()`，并把 `ui_refresh/interval_ms` 从 120 ms 调到 40–60 ms。
   预期：可见延迟 142 → 60–80 ms，同时每次按键的 IPC 事务数显著下降。
2. **模型导出层面的大头**：当前图输出**所有位置**的 logits（`[batch, seq, 21128]`，ctx=256 时
   单批约 350 MB），而打分只需要 `mask_index` 那一个位置。若在导出的 ONNX 里先按
   `mask_index` 取 hidden state 再做 LM head，理论上可再省 90%+ 的计算，
   从而在**保留 256 窗口保真度**的前提下把单条作业压到 10 ms 量级。
   这属于 `third-party/lm-train` 的导出改动，建议作为独立一轮。

## 七、建议

- **立即采用**：优化 1（可取消打分，已经默认生效）与 `max_context` 修复（无行为变化）。
- **需要你决定**：是否把编码窗口改成**自适应**（例如 `min(max_context, 上下文实际字数上取整到 8 + 8)`，
  下限 32）——短上下文时自动获得 8 倍加速，长上下文时保持 256 的保真度；
  代价是短上下文场景下候选尾部顺序可能与现在略有不同（首位不受影响）。
  保持现状也可以：把 `%APPDATA%\Rime\lm_ranker.yaml` 里的 `max_context` 设为 32/64/128 手动取舍。

## 八、复现命令

```powershell
# 延迟矩阵（隔离环境，自动启停服务端）
powershell -File weasel\tools\ipc_latency_test.ps1
powershell -File weasel\tools\ipc_latency_test.ps1 -Cases "ni=char,beijing=word" -Polls "120,60,40,20"

# 上下文窗口的速度/一致性对比（无头 harness）
$env:RIME_LM_RANKER_DEBUG = "1"
powershell -File weasel\tools\ctx_window_parity.ps1 -Window 32
```

## 九、本轮改动文件

| 文件 | 改动 |
|---|---|
| `librime/plugins/lm_ranker/lm_rank_engine.{h,cc}`（独立仓库） | `ScoreCandidatesCancellable`（批次边界可取消）、批次 64→16、`Initialize` 接收并生效 `max_context`、可取消变体的错误返回语义 |
| `librime/plugins/lm_ranker/lm_rank_service.{h,cc}` | 取消标志与排队/打分/取消的打点；`WaitIdle`/`Pending` 供后续轮询优化使用 |
| `librime/plugins/lm_ranker/lm_ranker.cc` | `rank hit/miss/skip` 带时间戳 |
| `weasel/tools/ipc_latency_test.ps1`（新增） | 延迟矩阵 |
| `weasel/tools/ctx_window_parity.ps1`（新增） | 窗口速度/一致性对比 |
| `third-party/lm-test/weasel_ipc_latency_e2e.cc`、`build_weasel_ipc_latency_e2e.cmd`（新增，不在 git 内） | 延迟探针客户端 |
