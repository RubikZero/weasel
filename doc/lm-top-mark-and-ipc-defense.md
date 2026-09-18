# 字词 [LM] 标记 + 拼音"无响应"防御（2026-09-18 第三轮）

对应用户待办 1 与 2。前两轮见 `lm-ranker-ui-fixes.md`（黑框、整机卡死）与
`server-blocking-fix.md`（服务端持续阻塞）。

---

## 一、待办 1：模型排第一的字/词候选也标 `[LM]`

### 改动（`librime/plugins/lm_ranker/`，该插件是独立 git 仓库）

| 位置 | 改动 |
|---|---|
| `lm_rank_engine.h` | `LmRankerConfig` 新增 `bool mark_top = true;`（yaml 键 `mark_top`） |
| `lm_ranker.cc` | `Rebuild()` 新增第 4 个参数 `mark`；新增 `MarkWithLm()`（用 `ShadowCandidate` 加 `[LM]` 注释，**不改动 text/start/end/quality**，因此选择上屏的行为完全不变）；新增 `TopRanked()`（取缓存排序的首位）；`Apply()` 在命中缓存时计算并传入 `mark` |

规则与边界：

- 只在**命中重排结果**（`RankService::Lookup`）时标记，因此 `[LM]` 与"这一版候选是模型排过的"严格对应；
- `MarkWithLm()` 跳过已带 `[LM]` 或类型已是 `lm_ranker` 的候选 → **不会重复标记**；
- 若该候选正好是整句生成要替换的那个（`promotion.replacement_source`），由整句路径处理并 `continue`，同样不会出现两个 `[LM]`；
- 排序后的第 1 名通常就是被标记的那个（`ApplyCachedOrder` 把它放在最前，仅在整句插入到位置 0 时退居第 2）。

### 一个必须知道的机制约束

过滤器（`Filter::Apply`）**只在组字被重建时运行一次**，而不是每次读候选时运行：

```
按键/上下文变化 → Context update → Engine::Compose → TranslateSegments → Menu::AddFilter → lm_ranker Apply
异步 LM 结果就绪 → RankService::Notify("lm_ranker") → refresh_non_confirmed_composition
                → ClearNonConfirmedComposition() → update_notifier_ → Compose → 再次 Apply → 命中缓存 → 上标记
```

因此：**打字速度过快、且模型尚未加载完**（服务刚启动的第一次输入）时，最后一次按键的候选
会在模型结果就绪后由上面第二条路径补上标记；正常情况下（模型已就绪）每次都会命中。
新增诊断（`RIME_LM_RANKER_DEBUG=1`，输出到服务端 stderr）可直接观察：

```
[lm_ranker] rank miss items=5          ← 本次未命中（已发起异步请求）
[lm_ranker] rank hit items=5 order=5 mark=北京   ← 命中并标记
[lm_ranker] rank skip input=beijing cands=3 ctx=0 ← 无可排候选（如无上下文）
```

### 验证

| 场景 | 命令 | 结果 |
|---|---|---|
| 无头（真实 rime.dll + ORT） | `e2e-check\weasel_lm_e2e.exe --keys beijing` | `1 北京 [lm_ranker] [LM]`，exit 0 |
| 无头整句回归 | `--keys beijingdaxue --expect-lm` | `1 北京大学 [lm_ranker] [LM]`，exit 0 |
| IPC 端到端字词 | `tools\ipc_word_mark_test.ps1` | `[LM]` 出现 1 次，pass |
| 旧构建对照（无 `mark_top`） | 同上 `-NoMark`（用已安装的旧 `rime.dll`） | `[LM]` 0 次，pass |

> 用 `beijing`（2 音节）作为输入是关键：`sentence.min_syllables = 4`，所以整句路径不可能
> 产生 `[LM]`，响应里出现的 `[LM]` 只可能来自本轮新增的字词标记。
> `tools/ipc_word_mark_test.ps1` 会在按键之间加入 150ms 间隔（`KeyDelayMs`），模拟真人打字；
> 零间隔会把所有按键在模型加载完成前发完，从而看不到重排（这不是缺陷，而是测试驱动问题）。

### 关闭方式

`%APPDATA%\Rime\lm_ranker.yaml` 里加 `mark_top: false`（重启算法服务生效）。

---

## 二、待办 2：拼音"无响应"的防御代码

### 2.1 区分"服务端说没处理"与"服务端根本没回答"

新增 `Client::ProcessKeyEventChecked(key, bool* transaction_failed)`
（`include/WeaselIPC.h`、`WeaselIPC/WeaselClientImpl.{h,cpp}`；`_SendMessage` 增加事务结果出参）。
原来两者都表现为"返回 false"，无法区分；现在超时/连接断开会被单独标记。

### 2.2 LM 轮询链不再叠加

`WeaselTSF::_PollLmRefresh()`（`WeaselTSF/KeyEventSink.cpp`）：

- 用 `ProcessKeyEventChecked(0, &failed)`；**事务失败即取消整条刷新链**（`_CancelLmRefresh()`），
  下一次真实按键会重新武装——原来会在 1.5s 内每 120ms 再发一次注定失败的事务；
- 失败累计到 3 次（`kStalledTransactionLimit`）→ 触发本地自愈（见 2.4）。

### 2.3 连接重试退避（这一条对"卡多久"影响最大）

`WeaselTSF::_EnsureServerConnected()`（`WeaselTSF/WeaselTSF.cpp`）每次按键都会 `Echo()`，
失败还会 `_Reconnect()`（EndSession→Connect→StartSession），每一步都是一个可能有超时的管道事务。
现在失败后进入 **1 秒冷却**（`kServerRetryCooldownMs`），冷却期内不再阻塞 UI 线程重试。

### 2.4 自愈：等价于"切换输入法再切回来"

`WeaselTSF::_RecoverStalledSession()`：连续 3 次事务失败后，**不做任何 IPC**地
结束本地 TSF 组字、清 `_status.composing`、销毁候选窗、`Client::DiscardConnection()`
（丢弃管道与会话号，下次按键重连并新建会话）。这正是用户发现的有效恢复手法的代码化。

### 2.5 顺带修掉的真实缺陷：IPC 等待预算被放大 ~7.8 倍

`PipeChannelBase::_WaitReadable()` 原来只累加 `Sleep(2)` 的时间来判断是否超时，
没有计入 `PeekNamedPipe()` 自身的开销（对端停摆时每次约 10~15ms）。实测（挂起服务端全部线程）：

| 配置预算 | 修复前实测 | 修复后实测 |
|---|---|---|
| 1000 ms | 7848 ms | **999 ms** |
| 3000 ms（默认） | 23455 ms | **2999 ms** |

也就是说：默认配置下，一个停摆的服务端本来会让**每次按键阻塞 UI 线程约 23 秒**（12 次轮询
叠加更久），这正是"输入法完全没反应"的体感来源。改为按墙钟时间（`GetTickCount64()`）
判定截止时间后，阻塞时长与配置严格一致。

### 2.6 新增诊断：IPC 阶段耗时

`RIME_WEASEL_IPC_TRACE=1` 时，`PipeChannel.cpp` 会把超过 50ms 的阶段写入
`%TEMP%\rime.weasel\weasel-watchdog.log`：

```
ipc: read-wait took 7843 ms      ← 修复前的那次超预算等待
ipc: connect / write / read / disconnect took N ms
```

### 2.7 验证

| 用例 | 脚本 | 结果 |
|---|---|---|
| 健康 vs 全部线程挂起 | `tools\ipc_flags_test.ps1` | 健康：`transaction_failed=0`；挂起 33 线程：`transaction_failed=1` 且 **恰好用满预算** |
| 服务端阻塞回归 | `tools\ipc_stall_test.ps1` | 冻结消息线程仍 0.3s 内出候选，优雅退出 ✓ |
| 维护往返 | `tools\ipc_maintenance_test.ps1` | `EndMaintenance` 0ms，781ms 后恢复打字 ✓ |
| 5 客户端并发 ×2 | `tools\ipc_stress_test.ps1` | 全部 `[LM]`，无锁告警 ✓ |
| 字词标记 | `tools\ipc_word_mark_test.ps1` | ✓ |

**诚实说明**：2.2–2.4 的 TSF 侧反应（取消轮询、退避、自愈）无法在无头环境下自动断言——它们
运行在真实应用的 TSF 线程里。已验证的是它们依赖的信号（2.1/2.5）以及代码路径本身；
建议用户按下面的手动步骤确认一次。

### 手动验证步骤（安装新包后）

1. 正常打字组字到一半，用任务管理器**结束 `WeaselServer.exe`**：
   - 期望：按键不再长时间无响应；屏幕上的组字被清掉；下一次按键自动拉起服务并重新开始组字。
   - 对照：旧版本此时每次按键会卡住数秒（现在最多一个事务预算，默认 3 秒）。
2. 服务刚重启后**立刻快速输入**：期望不出现长时间无响应（冷却 + 单次预算）。
3. 想留证据：设置 `RIME_WEASEL_IPC_TRACE=1` 与 `RIME_WEASEL_WATCHDOG_VERBOSE=1`
   （用户环境变量，重启算法服务生效），复现后把
   `%TEMP%\rime.weasel\weasel-watchdog.log` 发我。

---

## 三、本轮改动文件

| 文件 | 改动 |
|---|---|
| `librime/plugins/lm_ranker/lm_rank_engine.h`（独立仓库） | `mark_top` 配置项 |
| `librime/plugins/lm_ranker/lm_ranker.cc`（独立仓库） | 标记最高概率字/词、rank 命中诊断 |
| `weasel/include/WeaselIPC.h` | `ProcessKeyEventChecked`、`DiscardConnection` |
| `weasel/WeaselIPC/WeaselClientImpl.{h,cpp}` | 事务结果出参、`ProcessKeyEventChecked`、`DiscardConnection` |
| `weasel/WeaselIPC/PipeChannel.cpp` | **墙钟预算**、IPC 阶段耗时追踪 |
| `weasel/WeaselTSF/WeaselTSF.{h,cpp}` | 失败计数、1 秒重试冷却、`_RecoverStalledSession()` |
| `weasel/WeaselTSF/KeyEventSink.cpp` | 轮询链失败即取消、按键路径失败计数与自愈 |
| `weasel/tools/ipc_word_mark_test.ps1`（新增） | 字词 `[LM]` 断言（含旧构建对照） |
| `weasel/tools/ipc_flags_test.ps1`（新增） | 事务失败标志断言（挂起服务端全部线程） |
| `third-party/lm-test/weasel_ipc_flags_e2e.cc`、`build_weasel_ipc_flags_e2e.cmd`（新增） | 上述断言用客户端 |
| `third-party/lm-test/weasel_ipc_lm_e2e.cc` | 新增每键延时参数（模拟真人打字速度） |
| `third-party/lm-test/runtime/user/lm_ranker.yaml` | 记录 `mark_top` |

## 四、仍待办

1. 延迟优化与小型 causal Transformer 整句模型（用户待办 3）。
2. 客户端 LM 轮询频率优化：目前每次按键最多 12 次轮询；可在响应里带 `lm.pending` 标志提前停止。
3. 分析 09-17 的两个 `WeaselServer.exe` 转储（`%TEMP%\rime.weasel\`）。
