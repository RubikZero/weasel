# 上下文前缀缓存（context cache）实现说明

对应配置项 **`context_cache` / `cache_tail_min` / `cache_tail_max` / `cache_min_prefix`**。
测量依据见 `lm-kv-cache-s0.md`（真实验证集 5132 例）。

## 一、它做什么

候选打分原本每次都要把整个窗口前向一遍。现在把窗口拆成两段：

```
[PAD…] [CLS] 上下文前缀（冻结，K/V 复用）  │  重算尾段 + 候选 + [MASK] [SEP]
                                            └── 只有这一段进图计算 ──┘
```

- 前缀用 `encode_prefix.onnx` 编码一次，其 K/V 存下来反复使用；
- 尾段与 `[MASK]` **保持双向注意力**（这是精度的关键：把尾段也改成因果会掉 1.4pt），
  连同候选、`[MASK]`、`[SEP]` 一起交给 `score_tail.onnx`；
- 尾段长度 = `clamp(窗口/8, cache_tail_min, cache_tail_max)`，冻结前缀短于
  `cache_min_prefix` 时不启用（那时"编码一次前缀"并不比它替代的全量计算便宜）。

因为尾段长度跟的是**窗口**（窗口本身按 8 取整分桶），冻结边界在连续打字时不会移动，
整段组字过程（上下文不变）可以一直复用同一份前缀 K/V。

## 二、代码位置

| 位置 | 改动 |
|---|---|
| `lm_rank_engine.h` | `LmRankerConfig` 的 4 个新项；`SetContextCache()`、`ContextCacheTail()`、`ScoreCandidatesWithCache()`、`EnsurePrefixCached()`；缓存会话与缓存状态 |
| `lm_rank_engine.cc` | 加载两张图（缺失即停用并回退）；前缀编码；按"尾段+候选"分块打分；失败即**停用缓存**而不是每次按键重试 |
| `lm_ranker.cc` | 解析 4 个配置项（取下限/上限并夹紧） |
| `lm_rank_service.cc` | 每个作业把配置同步给引擎（配置可热改，引擎是单例） |
| `lm_rank_engine.cc::EnsureModelFromShared` | 已有用户目录不会因为"model.onnx 已在"而跳过新文件：`encode_prefix.onnx`/`score_tail.onnx` 会单独补齐 |
| `export_onnx.py`（lm-train） | 新增 `encode_prefix.onnx` 与 `score_tail.onnx` 的正式导出；`--kv-only` 只导这两张图 |

## 三、导出的两张图

| 图 | 输入 | 输出 | 说明 |
|---|---|---|---|
| `encode_prefix.onnx` | `input_ids[B,S]`, `attention_mask[B,S]`, `position_ids[S]` | 每层 `present.<l>.key/value` `[B,H,S,D]` | 前缀双向编码 |
| `score_tail.onnx` | `input_ids[B,T]`, `attention_mask[B,T]`, `position_ids[T]`, `past_attention_mask[B,P]`, `past.<l>.key/value` | `logits[B,2,V]` | 尾段双向；输出最后两个位置，避免动态下标 |

导出时踩到并已规避的坑（都在正式脚本里注释了）：
1. `torch.onnx.export` 会把模块留在 train 模式 → 校验前必须显式 `.eval()`；
2. Python 整数形状会被烧进图（用 `x.size()`、`expand(-1,…)`、`flatten(2)`）；
3. `position_ids` 必须是**显式输入且 rank-1**（从缓存长度推会被烧成常量；写成 `[B,S]` 会被 ORT 拒绝）；
4. `past_attention_mask` 必须是输入，缓存里的 PAD key 才不会被 `[MASK]` 看见；
5. 这么多动态轴必须 `dynamo=False`（legacy TorchScript 导出）。

## 四、已验证 / 待验证

**已验证**
- 图与线上 `model.onnx` 的一致性（`third-party/lm-test/s1_verify_graphs.py`，真实数据 200 例）：
  尾段 8/16/32 字时逐例首选一致率 **99.0% / 99.5% / 99.0%**，KL **5.7e-4 / 1.9e-4 / 5e-5**，
  与 S0 在 5132 例上的结论一致（`model.onnx` SHA256 与线上相同：`af20240637c09aa2…`）。
- 插件能加载两张图并进入缓存路径：隔离运行时日志出现
  `lm_ranker: context cache ready (…\encode_prefix.onnx)`。
- 失败回退是安全的：第一次实机运行时（当时 `position_ids` 秩写错）日志出现
  `prefix encode failed: Invalid rank for input: position_ids Got: 2 Expected: 1`，
  每次作业都**回退到全量计算**，候选与延迟与关闭缓存时完全相同（46–47ms @20ms 轮询）。
- 修复：`position_ids` 改为 rank-1；并在失败时直接停用缓存，避免每次按键白跑一次尝试。
- **实机端到端**（隔离运行时，2026-09-20，`third-party/lm-test/s1_final_acceptance.ps1`）：
  三种配置各做一次"预热 + 3 次组字 + 3 次延迟测量"（上下文 `meeting at three pm, `，
  按键 `beijingdaxue`，按键间隔 150ms）：

  | 配置 | `cache scored` | 图错误 | 可见延迟中位数（poll=20ms） | 最终候选菜单 |
  |---|---|---|---|---|
  | int8 前缀 + fp32 尾段，缓存开（出厂配置） | 78 | 0 | 46 ms | 基准 |
  | 同一份图，缓存关（全量重算） | 0 | 0 | 47 ms（最差 92 ms） | **逐字相同** |
  | fp32 前缀 + fp32 尾段，缓存开 | 78 | 0 | 46 ms | **逐字相同** |

  即：缓存开/关之间、前缀 int8/fp32 之间，**可见候选菜单完全一致**（含唯一那处 `[LM]` 标记）。
  窗口 32 时缓存省下的算力被 20ms 轮询粒度盖住了——收益随窗口增长，图级实测（S0 表，
  16 行候选）：窗口 32 → 1.23×、64 → 2.28×、128 → 3.42×、256 → **6.44×**
  （每键：从"全量重算"变成"复用冻结前缀 + 只算尾段"）；同一份对照见 `lm-kv-cache-s0.md`。
  隔离运行时日志里同时可见 `lm_ranker: context cache ready (…\encode_prefix.onnx)` 与
  每次作业的 `cache scored N candidates window=… tail=… prefix=…`。

- **长上下文下的实机收益**（`third-party/lm-test/s1_longctx_ab.ps1`，上下文换成约 200 字符
  的英文句子，自适应窗口因此到 216）：

  | 配置 | 可见延迟（poll=20ms，3 次） | 最终候选菜单 |
  |---|---|---|
  | 缓存开（出厂配置） | **中位数 47 ms**（46/47/47） | 基准 |
  | 缓存关（全量重算） | 中位数 **187 ms**（186/187/234） | **逐字相同** |

  也就是在这个组字场景里，缓存把**用户可见**延迟降到约 1/4，而候选顺序和 `[LM]` 标记不变。
  这正是缓存的目标场景：上下文越长，冻结前缀越大，省下的重复前向越多。

**待验证**
- 生产安装后的实测（安装包已含两张图；插件会把它们复制到 `%AppData%\Rime\lm-mlm`）。

**本轮新增**：整句解码现在也复用同一份前缀缓存，周围文本变化时还会提前把前缀编码好
（第七、八节）——解码推入模型的位置数减少约 4.7×，12 个输入上结果与改前逐字相同；
冷上下文后的第一下按键从 ~140ms 降到 ~93ms。

## 五、如何验证与开关

```yaml
lm_ranker:
  context_cache: false     # 关掉缓存，做 A/B 对照
  context_cache: true      # 打开（默认）
```
- 对照实验：同一输入下比较候选顺序与 `ipc_latency_test.ps1` 的可见延迟；
  本文用的对照脚本在 `third-party/lm-test/`：`s1_final_acceptance.ps1`（缓存开/关 ×
  前缀 int8/fp32，三种配置各跑一轮）、`s1_longctx_ab.ps1`（长上下文下缓存开/关）、
  `s1_decode_warm_ab.ps1`（解码缓存 A 与改前二进制对照）、`s1_warm3.ps1`（前缀预热 B），
  都用 `s1_compare_menus.py` 逐块比对可见候选菜单；
- 日志（`RIME_LM_RANKER_DEBUG=1`）里 `lm_ranker: cache scored … window=… tail=… prefix=…`
  表示缓存路径生效；`decode steps=… positions=… ms=…` 报告解码开销，
  `prefix hit/encoded`、`warmed prefix` 报告前缀是命中、编码还是预热作业完成的；
- 任何图不匹配都会自动回退到全量计算，不影响可用性。

## 六、内存与体积（实测）

ONNX Runtime（CPU，intra_op=2）逐个加载各图时的 RSS 增量：

| 图 | fp32 | int8 |
|---|---|---|
| `encode_prefix.onnx` | +34.6 MB（文件 30.7 MB） | **+9.9 MB（文件 7.8 MB）** |
| `score_tail.onnx` | +56.5 MB（文件 54.2 MB） | +16.3 MB（文件 13.8 MB） |

（作为参照：原有的 `model.onnx` 是 +65.1 MB，而插件里它有两份会话，所以那部分约 130 MB 是既有开销。）

- 缓存带来的额外内存：两张 fp32 = **+91 MB**；出厂配置（int8 前缀 + fp32 尾段）= **+66 MB**；
  两张都 int8 = +26 MB。
- 安装包体积：50.7 MB（无缓存）→ 126.4 MB（两张 fp32）→ **103.1 MB（出厂配置）**。
- 精度（`s1_verify_graphs.py`，200 例真实数据，尾段 16）：int8 前缀 + fp32 尾段
  **99.5% 逐例首选一致 / KL 4.7e-4**，与两张 fp32 的 99.5% / 1.9e-4 相同；尾段 8 与 32 同样是 99.5%。
  两张都 int8 则掉到 97.0% / KL 7.4e-3（约 −1.5pt）⇒ **只量化前缀编码器是免费的，尾段打分器不能量化**。
- 速度（ORT，16 行候选）：前缀编码 5.05 → 4.19 ms；尾段步进 12.52 → 8.76 ms。
- 因此出厂只把 `encode_prefix.onnx` 换成 int8；插件侧不需要任何改动（文件名与输入输出都不变），
  实机菜单与 fp32 逐字相同（见上表）。要换回 fp32：用 `export_onnx.py --kv-only --kv-int8 none`
  重新导出后覆盖同名文件即可。
- 另：两张图里的词嵌入/输出头各 20.6 MB，尝试过"合并重复初始化器"没有收益——一份是
  `Gather` 用的 `[vocab,hidden]`、一份是 `MatMul` 用的转置布局，fp32 下无法共用，量化才是正解。

## 七、整句解码复用同一份前缀缓存

原来 `DecodeRange` 的 beam search **每一步、每条 beam 都把整段上下文重新前向一次**
（`[CLS] + ctx + beam + [MASK] [SEP]`），可组字期间这段上下文根本没变。现在解码行只带
`[上下文尾段][beam 词][MASK][SEP]`——和重排行完全相同的形状——所以和重排共用同一份冻结前缀、
同一对图，**不需要任何新导出**；前缀的切分点也只由窗口决定，两个路径天然命中同一份缓存。

实测（隔离运行时，长上下文窗口 216，poll=20ms，各 3 次；`third-party/lm-test/s1_decode_warm_ab.ps1`）：

| 输入 | 本次改动后（缓存开） | 改前（普通解码） | 缓存关（对照） | 最终菜单 |
|---|---|---|---|---|
| 8 音节 `zheshiyigeceshi` | **236 ms** | 376 ms | 470 ms | 逐字相同 |
| 9 音节 `jintiantianqihenbucuo` | **236 ms** | 461 ms | 564 ms | 逐字相同 |

解码自身的日志 `decode steps=… rows=… positions=… ms=…`：
- 缓存开：`steps=7 rows=25 cached_rows=25 positions=809 ms=45`
- 缓存关：`steps=5 rows=17 cached_rows=0 positions=3763 ms=147`

即推入模型的位置数减少约 **4.7×**、单次解码 **147 ms → 45 ms**。再取 12 个输入（含 20 字符音节串）
逐例比较"改前 vs 改后"的最终候选菜单：**12/12 完全一致**——说明"冻结前缀"这个近似在整句解码上
和在重排上一样没有改变结果（解码用的是同一套 logits，只是前缀不再重算）。

## 八、前缀预热：把编码从"第一下按键"里挪走

前缀只取决于周围文本，而周围文本是在**提交上屏**时变化的，那一刻没有按键在等。插件因此监听
context 的两种通知（`surrounding_text` 是 context 属性，走 `property_update_notifier`；普通更新
走 `update_notifier`），一发现周围文本变了就排一个**低优先级**的预热作业，只做前缀编码：
它永远不会挤掉排队中的重排（重排自己会编同一份前缀，白等没有意义），完成后也不发布任何菜单。
Hook 在过滤器构造时就接上——否则"本会话第一次组字"的周围文本更新会赶在第一次 `Apply` 之前，
那一轮的预热量就丢了。

实测（每轮换一段全新周围文本 ⇒ 每轮前缀都是缓存没见过的，即"刚提交完"的状态；单键 `n`，
poll=20ms，`third-party/lm-test/s1_warm3.ps1`）：

| | 各轮延迟 | 平均 | 日志 |
|---|---|---|---|
| 改动后 | 92 / 91 / 93 / **139** / 94 ms | **104 ms** | 预热作业 5 次、`prefix encoded` 5 次（预热做的）、`prefix hit` 23 次 |
| 改前 | 93 / 140 / 138 / 137 / 138 ms | 170 ms | 无预热 |

即冷上下文后的第一下按键 ~140ms → ~93ms（省掉的正是那 ~50–70ms 的前缀编码）。偶尔（1/5）
按键会赶在预热编码完成之前到达，此时要等缓存锁，仍会看到 ~139ms——是竞态，不是回退。

**回归**：改动后标准延迟表与改动前一致（窗口 32，poll=20ms：单字 46 / 词 47 / 整句 46 ms；
poll=120ms：139 / 139 / 139 ms），长上下文下缓存开/关的候选菜单依然逐字相同。

## 九、plan key 改为内容寻址

重排结果的 key 原来包含每个候选**在 IME 列表中的下标**与原始顺序，缓存结果也按**下标**回放。
于是只要 IME 重排了同一批候选（文本、span、quality 都不变），就会算出一个新 key、把完全相同的工作
再排一次；而按下标回放本身也依赖"请求时与回放时的列表顺序一致"。

现在 key 由（上下文 + 每个候选的 `start:end:quality:文本`）**排序后**拼接而成，结果按**文本**回放，
模型没打分的条目保持原有相对顺序（`lm_ranker.cc::MakeRankPlan` / `ApplyCachedOrder`）。

实测（`third-party/lm-test/s1_jobcount.ps1`，单词与整句各 3 次测量）：作业数**完全不变**
（改前 `rank miss=77 hit=81`，改后 `rank miss=77 hit=81`），菜单与延迟也不变——因为隔离运行时里
IME 的候选顺序是确定的，构造不出"同一候选集、不同顺序"的场景。所以这条改动的价值在真实使用中：
选词后 userdb 学习会改变同一批候选的排序，那时不再重复排队同一份工作，结果也不再依赖位置。

## 十、上下文窗口上限 256 → 384

模型位置数是 512，而解码行是 `[CLS] + 窗口 + 音节数(≤16) + [MASK] [SEP]`，所以窗口上限是 **480**。
原来有两处硬编码把它卡在 256：配置解析（`lm_ranker.cc`）和引擎初始化
（`lm_rank_engine.cc`，`std::min<size_t>(256, …)`）——第二处正是"改了 yaml 却不生效"的原因，
现在两处都放开到 480，出厂默认 **384**（代码默认 + `lm_ranker.yaml`）。自适应逻辑不变：
短上下文仍然只用 32–64 的窗口。

实测（`third-party/lm-test/s1_window_ab.ps1`，566 字符周围文本，窗口由缓存日志读出）：

| `max_context` | 实际窗口 | tail | 编码的前缀长度 | 8 音节延迟 | 短上下文（窗口 32） | 图错误 |
|---|---|---|---|---|---|---|
| 256 | 256 | 32 | 225 | 233 ms | 46 ms | 0 |
| 384 | **384** | 32 | 353 | 234 ms | 47 ms | 0 |

即多出来的 128 字上下文**不增加每次按键的开销**：代价集中在一次更长的前缀编码（353 字，
比 256 时多约 30–40 ms），而那一次发生在提交时并被预热作业挪出按键路径。

**精度上的诚实说明**：离线验证集**无法**衡量 256 以上的收益——`reranker_v2/data.npz` 的 `ctx`
形状就是 `(N, 256)`（构建时已把上下文截断到 256 token；5132 条验证例中 1610 条已用满 256）。
因此本节能证明的是"384 窗口确实生效、不增加按键延迟、短上下文无回归"；
"更长上下文是否让首选更准"需要另建一份长上下文评测集，尚未做。
