# lm_ranker 集成进度（M2，2026-09-15）

## 已完成
- **Weasel 侧（光标上下文 + 服务端推送）**
  - TSF：异步 edit session 读取光标前最多 256 字（`WeaselTSF::_RequestSurroundingText`），焦点/排版/编辑变化时失效
  - IPC：新增 `WEASEL_IPC_SET_CONTEXT`（客户端在按键前发送上下文）
  - 服务端：解析后 `set_property(session, "surrounding_text", ...)`，插件可读
  - 服务端推送：`WM_WEASEL_POST_CALLBACK` + `Server::Post()`，让后台线程（模型解码）在服务器消息线程刷新候选窗；librime 通知 `lm_ranker` 已在 `RimeWithWeaselHandler::OnNotify` 接线
- **librime 插件（lm_ranker）**
  - ONNX Runtime C++ 接入（NuGet 1.20.1，含 win-x64/x86/arm64）
  - `%AppData%\Rime\lm_ranker.yaml` 配置（enabled/model_dir/prior_beta/chars/words/sentence/beam/topk/promote…）
  - 上下文取值：`surrounding_text` 属性 → `commit_history` 兜底
  - 候选打分：mask_index ONNX 图（每行只输出 mask 位 logits，低内存），z-score 融合先验
- **模型产物**（`third-party/lm-train/onnx/mlm_ft_v2/`）
  - `model.onnx`（mask_index 图 33.6MB）/ `model.int8.onnx`（8.5MB）/ `model_full.onnx`（参考）
  - `vocab.txt`（21128，已按 id 顺序重建）、`syllable_table.tsv`（413 音节）
- **端到端**：抽取器 + 真实 `rime.dll` + ORT，200 例字级请求跑通（无崩溃）

## 当前问题（待修）
1. **C++ 精度未对齐 Python**
   - C++ 插件 top1 = **0.735**（200 例，真实菜单）
   - 同 200 例 Python 参考：全菜单 0.835 / top-20 截断 0.855
   - 现象：插件模式下最终候选菜单出现**上一输入状态的候选**（如 `bao` 菜单里混入 `ba` 的「吧」，其 quality=0.00386 为全场最高），异常高的先验 z 分主导融合；基线抽取与 Python 菜单均无此候选
   - 推断：过滤器 eager 消费翻译并重建 FifoTranslation，可能与翻译流的增量续传/分页重建相互作用（待读 librime filter 调用与 translation 生命周期）
2. 并发/性能：单 session 互斥下 rerank 每次全菜单打分；应限制为 top-K（按先验）并评估单键延迟
3. 整句生成尚未在插件内实现（束搜索/置顶/通知已具备服务端通路）

## 已修复（2026-09-15 晚）
### 菜单污染 + 性能（同一根因）
- 根因：`ScriptTranslation` 按设计包含"只覆盖输入前缀"的候选（如 `bao` 里的「吧」`[0,2)`）与大量部分音节候选；插件对**全部候选**打分，两个后果：
  1. 精度：高先验前缀候选被 z 分抬到首位（0.735）；
  2. 性能：部分输入状态菜单达 ~1955 个候选，一次巨型 ORT 批（1955×259×21128），单次 Apply 秒级 → 8 例 56s、200 例跑不完。
- 修复：
  - **跨度门控** `full_span_only`（默认 true）：单段组合时只重排 `end() >= 输入长度` 的候选，其余保持 IME 原序；
  - **先验 top-K 截断** `max_candidates: 20`（与 Python 评测口径一致）；
  - **长度上限** `max_candidate_length: 8`；
  - 引擎内部按 64 行分批（防御性）。
- 结果：8 例 56.4s → **5.3s**；200 例可完成（123.6s，含部署/翻页开销）；C++ top1 **0.735 → 0.845**（Python 同口径参考 0.855）

### 稳定性（RAII/崩溃修复）
- **ORT 动态加载**：不再链接 import lib；`onnxruntime.dll` 仅从 rime.dll 同目录加载（避免 `C:\Windows\System32\onnxruntime.dll` 野生副本；该文件在本机确实存在且缺少 `OrtGetApiBase`）
- **`ORT_API_MANUAL_INIT`**：ORT C++ 头默认在静态初始化（DllMain 内）调用 `OrtGetApiBase()->GetApi()`，缺 DLL 时空指针 → 进程 0xC0000142；改用官方手动初始化，加载成功后自行 `Ort::InitApi()`
- **单例故意不析构**：避免 onnxruntime.dll 被卸载后，静态析构再调用 ORT 内存导致退出崩溃（退出码 0xC0000005）
- 缺 DLL 场景：插件打印诊断并**安全禁用**，抽取正常完成（EXIT=0，0.33s）
- 配置读取容忍 UTF-8 BOM；解析失败回退默认值并记录
- `ServerImpl::Post`：`PostMessage` 失败/异常时不再泄漏回调对象（unique_ptr）
- TSF 上下文 edit session：所有失败分支正确 `Release`；上下文**变化才发送**（避免每键 4 次 IPC）；失焦发送空串清除服务端上下文
- `SetSurroundingText`：会话不存在时直接返回，避免 map 膨胀
- 并发约束（已核对源码）：librime 非线程安全；Weasel 用 `g_api_mutex` 串行化所有管道请求（`WeaselServerImpl.cpp`），插件 `Apply` 因此始终在引擎线程同步执行；**后续整句异步线程只允许处理快照，不得触碰 `Context`/`Engine`**；托盘命令线程绕过 `g_api_mutex` 是 Weasel 既有风险点

### completion 参与重排的实测结论
- 真实菜单中 `completion`（预测补全）候选极少：全音节 200 例菜单 11530 个单字 vs 23 个双字词；前缀输入 300 例 0 个 `completion` 类型
- 含/不含多字预测候选：**top1 完全一致**（multi_picked = 0/23、0/581）→ `rank_completion` 默认开启无影响
- 前缀（半截拼音）输入下重排收益显著：IME 0.2295 → **0.6639**（n=122，300 例中目标在候选内的子集）

## 整句生成（M2d，2026-09-15）
- 实现：`lm_sentence.cc` 后台单 worker（debounce=120ms、作业去重、seq 取消），只处理**数据快照**（input/上下文/音节序列/IME 首选），使用**独立 ORT 会话**（不阻塞交互重排）；完成后按 `promote` 策略（always/smart/off；smart 用每字平均对数似然差 > margin 判定）写入 ready 缓存并触发 `lm_ranker` 通知
- 可见性链路：通知 → Weasel 服务器线程 → **新增 librime API `refresh_non_confirmed_composition`**（重建组合并重跑过滤器）→ `_UpdateUI`；插件在重排时若发现 ready 整句，则作为 `SimpleCandidate` 插入候选首位
- 冒烟（20 例难例整句，`--wait 1200`，promote=always，扩展抽取器带 `--wait`）：
  - C++ 首候选与 Python 解码**一致 18/20**；首候选==目标 5/20，与 Python 参考（beam4，同 20 例）**完全一致**
  - 例：`fuyinxingdanbaozhaiwuhuoxubu` → 首位「府银行担保债务或许不」（IME 首位「福音行担保债务或许不」）
- `smart` 策略冒烟（同 20 例，margin=0.5）：置顶 12/20（其余保持 LM 重排后的 IME 候选）；被置顶且正确 4 例（`always` 下 MLM 正确共 5 例）→ **阈值需在真实使用中复校**
- 已知待改进：
  1. **音节切分歧义**：无分隔符输入存在多种切分（如 `weiyanabiandeweidu` = wei ya na… 或 wei yan a…），当前 DP 取"最长优先"；建议改为**音节二元组 Viterbi**（由 luna 词典离线统计）
  2. **罕见异读污染**：音节表含全部 heteronym（如 `chi`→提、`wei`→有、`yan`→但），解码可能选出怪字；可改为常见读音优先/白名单
  3. cap=50 已生效（配置默认值），partial 场景覆盖 97.5%

## Phase 1 + Phase 2（引擎分词口径 / 词边界，2026-09-15 夜）
### Phase 1：引擎分词 + 只读 C API
- 内核：新增 `SegmentParse`（音节序列 / 词边界 / 引擎词文本 / 来源）挂到 `Context`（输入变化即清理）；`script_translator` 在物化**整串候选**时发布（首选或句候选）；只读 C API `get_segmentation(type: 0 音节 / 1 词边界 / 2 词文本)`
- 插件：分词优先级改为 `segment_parse → 候选 preedit → DP`；DP 兜底改为**先按用户输入的分隔符分块**再切（修掉此前丢弃 `'` 的问题）
- 结果：`bao` 类前缀污染后，`weiyanabiandeweidu` 现由引擎切为 `wei ya na bian de wei du`（此前 DP 切错为 `wei yan a …`）；难例 100 例 C++ 与 Python 参考 **93/100 一致**；字级 200 例无回归

### Phase 2：词边界进入解码（两次尝试）
- **2a 词块独立解码**：按引擎词边界分块、每块独立束搜索再组合 —— **失败**（丢失跨词条件，难例 5→4/20），已移除
- **2b 词边界软加分**（保留全局束搜索，仅在词边界处对"引擎认可的词"加分；endorse 词优先取菜单同跨度**简体**候选，回退引擎词文本）：
  - 难例 100 例：Phase1 32 → **36**（+4pt）
  - 词对齐 100 例：Phase1 74 → 72（−2pt）
  - 样本噪声约 ±5pt，**结论：未达"稳定提升且不伤害词对齐"的标准**
- 处置：保留 `sentence/word_bonus` 能力，**默认 0**（即回退 Phase 1 行为）；下一步先做 bonus 扫参（0.5/1/2 × 更大样本）或直接进入 Phase 3

### 结论与建议
- Phase 1 达成目标（MLM 所见 = 引擎/用户所见，含手输 `'`）；Phase 2 收益不稳健
- 建议按门槛进入 **Phase 3**：暴露**完整音节图 + 词典/prism 句柄**，在图上做词级/晶格解码（按词典词打分的词级束搜索），以缩小词对齐场景与 IME 的差距（当前 72~74 vs 80）

## Phase 2.5：rime 字表 + 读音权重（Python 消融 → C++ 移植，2026-09-16）
### 失败归因（100+100，现有输出经 `diag_errors.py`）
- 分类占比：rare（罕读污染）22~30%、word（词约束可救）8~21%、other（LM 上下文排序）55~70%、coverage 0
### 数据表（`build_readings.py` / `export_plugin_readings.py`）
- `char_readings.tsv`：37,544 字（1,654 条显式百分比；繁简合并、按字归一化，缺省读音按剩余概率摊分）
- `word_pron.tsv`：441,414 词×读音（dict 权威 19k + essay 派生 423k；相对最优 ≥25% 过滤，如 `遺毒` 只保留 `yi du`，`银行` 以 dict 的 `yin hang` 为准）
- 插件用 `syllable_readings.tsv`：416 音节 / 7,279 行（vocab 过滤，109KB）
### Python 消融（beam4/topk8；基线 A=33/67）
| 配置 | hard100 | words100 |
|---|---|---|
| A 基线（pypinyin 字集） | 33 | 67 |
| B0.5 / B2 / B4（软权重） | 34 / 31 / 30 | 71 / 66 / 67 |
| R0（rime 字集） | 38 | 73 |
| **R0B0.5（rime+λ0.5）** | **40** | **78** |
| C2 / C4（词奖励，pypinyin） | 40 / 41 | 65 / 64 |
| RD1（rime+λ0.5+dict 词奖励 λ1） | 41 | 78 |
| holdout 300 例：A→R0B0.5→RD1 | 133→**148**→148 | 192→211→**212** |
- 结论：**收益主体 = rime 字集 + 读音权重 λ0.5**；词约束在 words 上仅 +0.4pt，**未过 Phase 3-lite 门槛**（暂不做），词奖励对 hard 有效但对 words 有害（已弃用）
### C++ 移植与复核
- 插件新增：优先加载 `syllable_readings.tsv`（保留旧表回退）、配置 `sentence/pron_weight`（默认 0.5）、打分增加 `pron_weight·log P(读音|字)`
- 100+100：hard 32→**40**（+8）、words 74→**78**（+4）；holdout 300：hard **145**、words **211**（与 Python 逐例一致）
- 归因复测：rare 类占比 24%→**8%**（hard）、30%→**4.5%**（words）
- 剩余失败以 LM 上下文排序（other ~65-68%）与词级选择为主

## 下一步
1. M3：x86 Boost + Win32 librime/Weasel + ARM64X + `install.nsi` 打包（**模型目录需包含 `syllable_readings.tsv`**；ORT/模型/许可）
2. smart 置顶阈值在真实使用中复校（当前 margin=0.5；Phase 2.5 后 MLM 首候选质量已提升，阈值可能需放宽）
3. 剩余误差以 LM 上下文为主（other 55~70%）；如继续提升需考虑更大模型/更多上下文或词级晶格（Phase 3-full，需内核暴露音节图）
4. 清理本机 `C:\Windows\System32\onnxruntime.dll`（非本工程产物，建议排查来源）

## 复现命令
```powershell
# 生成读音表（Python 侧）与插件用表（需在 lm-test 目录）
python build_readings.py --stats
python export_plugin_readings.py --out D:\Workspace\rime\third-party\lm-train\onnx\mlm_ft_v2\syllable_readings.tsv
# 失败归因 / 消融
python diag_errors.py --requests testset\requests_sent_100.tsv --meta testset\meta_sent_100.tsv --cand testset\cand_h100_p3.tsv --label cpp
python run_sweep.py hard --only A,R0B0.5,RD1
# 构建 librime（x64, 含插件）
$env:PATH = "D:\Software\cmake-3.31.12-windows-x86_64\bin;" + $env:PATH
cmake --build D:\Workspace\rime\weasel\librime\build --config Release --target install
# 运行真实候选抽取（插件 + 上下文）
rime_real_candidates.exe --shared D:\Workspace\rime\weasel\output\data `
  --user D:\Workspace\rime\third-party\rime-user-real --schema luna_pinyin_simp `
  --input requests_ort_test.tsv --output cand_ort_test.tsv --meta meta_ort_test.tsv --max 100 --pages 12
```
（测试用 `luna_pinyin.schema.yaml` 需临时在 filters 末尾追加 `- lm_ranker`）
