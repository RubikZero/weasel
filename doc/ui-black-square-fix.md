# lm_ranker UI 问题修复记录（2026-09-17 ~ 09-18）

本轮围绕 lm_ranker 改造引入的 UI 问题：光标附近的黑色方块、（拼音）输入偶发无响应、
以及 09-18 上午的整机 UI 卡死。本文档记录证据、根因、修复、验证与遗留问题。

---

## 一、黑色方块（已修复，用户实测确认消失）

### 现象
光标下方出现黑色小方块，约 1~2 秒后消失；打字 / 删字 / 移光标 / 按 F4 都会出现；
记事本、VS Code、浏览器都有；**换任何配色方案都是黑的**。

### 根因（日志实证）
每按键都会产生一条 librime 属性通知：

```
每按键 → TSF 送光标上下文 → rime_api->set_property(session_id, "surrounding_text", ...)
       → Context::set_property → property_update_notifier_
       → ConcreteEngine::OnPropertyUpdate（engine.cc:147 "updated property: surrounding_text"）
       → message_sink_("property", ...) → RimeWithWeaselHandler::OnNotify
       → 存入 m_message_type/m_message_value
       → _UpdateUI → _ShowMessage 把它当成"给用户看的提示"
       → m_ui->Update(ctx,status) + ShowWithTimeout(show_notifications_time=1200ms)
```

而 `m_message_type == "property"` 不匹配 deploy/schema/option 任何分支，`tips`
（`ctx.aux.str`）保持为空 → 弹出的是**没有任何文字的空提示窗**，位置=输入光标下方，
1200ms 后自动隐藏。日志证据（`%TEMP%\rime.weasel\*.INFO`）：单次输入会话内
**212 次 `updated property: surrounding_text`**；`weasel.yaml` 默认
`show_notifications: true` → `m_show_notifications["always"]` → `falways` 命中 →
**键键必弹**。

> 上游也有"property 会弹提示"的逻辑，但上游只在建会话时触发一次（`client_app`）；
> 本轮新增的**逐键 surrounding_text 通路**把它变成每键一次，问题才暴露。

### 修复（`RimeWithWeasel/RimeWithWeasel.cpp`）
1. `OnNotify()`：只接受 `deploy`/`schema`/`option`，其余（`property` 等）直接返回。
   既不再弹空提示窗，也不会覆盖同一按键里真正的通知。
2. `_ShowMessage()`：既无文字又无图标时**不弹窗**（兜底，消除"空提示窗"这一类问题）。

### 关于"起初黑框、后来变图标"的解释
面板窗口创建时是 `WS_EX_TRANSPARENT`，只有 `DoPaint()` 里
`ModifyStyleEx(WS_EX_TRANSPARENT, WS_EX_LAYERED)` + `UpdateLayeredWindow()` 之后，
它的分层表面才是良定义的；在那之前 `ShowWithTimeout()` 直接 `ShowWindow()` 显示的
是未定义内容 → 黑框。已有过一次成功绘制之后，同样的提示窗就只剩状态图标。
另：`weasel/resource/zh.ico` 6 帧全为 32bpp、32×32 帧采样 `(0,0)=RGB(255,255,255) A=255`、
`(16,16)=RGB(69,69,69) A=255`、AND mask 采样位全 0 —— **图标本身一直是好的**，
"画出来了"就该是白底 + 深灰"中"字，所以纯黑只可能是"什么都没画"。

---

## 二、加固（同批提交）

| 位置 | 改动 |
|---|---|
| `WeaselUI/WeaselPanel.cpp` | 分层窗口源位图由 `CreateCompatibleBitmap`（内容未定义、无有效 alpha）改为**清零的 32bpp DIB section**（`CreateDIBSection` + `memset`），未绘制像素=真透明；创建失败/客户区为空时不再推送未定义表面 |
| `WeaselUI/WeaselPanel.cpp/.h` | 记录本次绘制是否有内容（`m_paintDrew` → `HasContent()`）；`StartLmRefreshTimer()` 对 initial/interval 加 20/30ms 下限 |
| `WeaselUI/WeaselUI.cpp` | `UIImpl::Show()` 幂等 + 内容门控：已可见则不重复 `ShowWindow`；无内容则保持/置为隐藏（LM 轮询期间不再反复显示同一张分层表面） |
| `WeaselTSF/KeyEventSink.cpp`、`WeaselTSF/CandidateList.*`、`include/WeaselUI.h` | `_ScheduleLmRefresh()` 改为查询面板**真实定时器状态**（`IsLmRefreshTimerRunning()` 全链路），不再信任可能残留的 `_lm_refresh_pending` |

---

## 三、验证与打包

- `build.bat rime`（x64+Win32，含 lm_ranker 最新提交）→ `build.bat weasel` → `makensis` 打包，
  产物 `weasel/output/archives/weasel-0.18.0.0-installer.exe`；解包逐项哈希校验与构建产物一致
  （x64/Win32 `rime.dll`、`weasel*.dll`、`WeaselServer.exe`、`data/lm-mlm/*`）。
- 无头端到端冒烟（`third-party/lm-test/e2e-check/`，真实 `rime.dll` + ORT）：
  - `beijingdaxue`：首位候选被包装为 `[lm_ranker] [LM]`，`RESULT lm_candidate=yes`；
  - `fuyinxingdanbaozhaiwuhuoxubu`：整句解码 10 音节，`margin=+0.6058` > 0.5 → 整句置顶，EXIT=0。
- 用户实测：安装最新版并重启后，**黑色方块消失**。

---

## 四、遗留问题 A：拼音输入偶发"完全无响应"

### 代码事实（已核实）
1. 所有管道请求串行在一把全局 `g_api_mutex` 下（`WeaselServerImpl.cpp:232-235`）；
   LM 通知的刷新任务也在同一把锁下运行（`PostRime`，`:175-181`），且**遍历刷新所有会话**
   （`RimeWithWeasel.cpp:536-549`）→ 带 lm_ranker 的拼音每键会额外产生这些工作，五笔没有，
   这正是"只有拼音"的差异来源。
2. 客户端**每按键**都会 `Echo()` 校验会话，失败即 `_Reconnect()`（`Disconnect`→`Connect`→
   `StartSession`，`WeaselTSF.cpp:228-237`）；会话 id 内嵌服务端 PID
   （`_GenerateNewWeaselSessionId`，`:30-34`），故服务端重启后旧 id 不会误判有效 → 有自愈路径。
3. 若服务端不可用，`_ProcessKeyEvent` 会 `*pfEaten = FALSE`（`:54-57`）→ 按键漏给应用（出英文），
   而不是"什么都不发生"。
4. 因此"什么都没发生"更像：**按键到了服务端且被 Rime 消费**（键被吃掉），但 TSF 侧的
   显示/上屏 edit session 链路失同步 —— 这解释了"仅该窗口、服务端无异常日志、切输入法可恢复"。
   LM 改造在这条链路上增加了额外 edit session（组字期每 120ms 的 `ProcessKeyEvent(0)` +
   `_UpdateComposition`）与通知刷新，正是风险来源。
5. 已知瑕疵：`EndMaintenance()` 无条件 `m_session_status_map.clear()`（`:652-658`），
   即任一客户端走恢复路径会清掉**所有窗口**的会话映射（能自愈，但会造成连锁抖动）。

### 日志排查结论
12:00–16:00 的 23 份日志：**无任何 E/W 行**；部署进程 7 次（14:43:24/40、14:51:20、15:13:16、
15:15:25、15:29:08、15:31），部署分钟里逐键事件降到 0–2/分（正常 20–64/分），每次部署后出现
会话初始化突发（6/8/10）= 客户端重建会话。未见"服务端卡住"的痕迹。

---

## 五、遗留问题 B（优先级最高）：09-18 上午整机 UI 卡死

### 时间线（由日志与目录时间还原）
| 时刻 | 事件 |
|---|---|
| 09-18 09:40:02 | 服务端进程启动（现场日志 `…INFO.20260918-094002.5436.log`） |
| 10:00:01.005 | `lm_rank_engine.cc:221] lm_ranker: model loaded: %APPDATA%\Rime\lm-mlm\model.onnx, vocab 21128, syllables 416`（首次用拼音触发加载，线程 15560） |
| 09:40–10:02 | 共 441 行日志，**无任何 E/W**；10:02:09/12/50/51 有多次 `updated option: ascii_mode` |
| **10:02:56.905** | 最后一行：`updated property: surrounding_text`。之后日志**直接停住** |
| ~10:03–10:08 | 用户描述：浏览器先无响应 → 切焦点到其他窗口/桌面时，其他窗口与 **Windows Explorer 一并卡死** → 强制断电 |
| 10:08:12 | 重启后新的服务端进程启动（0 字节新日志） |

### 关键判据
- **不是崩溃**：09-18 无任何 `WeaselServer.exe` 转储（目录里的两个转储是 09-17 14:44 的），
  日志也无异常 → 服务端是被**阻塞**（blocked），不是异常退出。
- **不是模型/内存问题**：`%APPDATA%\Rime\lm-mlm\model.onnx` 与安装包、仓库
  `third-party/lm-train/onnx/mlm_ft_v2/model.onnx` **SHA256 完全一致**（mask_index 图，
  35,190,753 字节），**不是** `model_full.onnx`；不存在"全量 logits → 每次 1.4GB"的内存耗尽路径。

### 机制分析（已定位并复现）
客户端所有 IPC 都是**阻塞式事务**，且都在**应用的 UI 线程**上发起（按键路径、LM 轮询）。
把三处无界等待串起来就得到了整机卡死的完整链条：

1. `PipeChannelBase::_WritePipe()` 在 `WriteFile` 之后调用 **`::FlushFileBuffers(pipe)`**——
   对命名管道，它会**一直等到对端把数据读走**（`WeaselIPC/PipeChannel.cpp`）。
2. 服务端是在**持有全局 `g_api_mutex` 的情况下**调用 `resp(result)` 写响应的
   （`WeaselServerImpl::Run` 的 listener，`HandlePipeMessage` 内 `resp(result)`）。
3. 于是只要**有一个客户端不读**（浏览器卡住/被杀/UI 线程被占），服务端那一个连接的工作线程
   就会**持锁阻塞在 flush 上** → 其余所有客户端的请求都拿不到锁 → 它们的 UI 线程卡在
   **无超时的 `ReadFile`**（`_ReceiveResponse`）或**无界的连接等待**（`_Connect` 里
   `while (...) WaitNamedPipe(500)`）→ **浏览器先卡、切焦点时新窗口/Explorer 一起卡** →
   只能断电。日志"处理正常到 10:02:56 后直接停住、无报错、无转储"完全符合"持锁阻塞"。

**已实施修复（本轮）**
| 位置 | 改动 |
|---|---|
| `WeaselIPC/PipeChannel.cpp` | 删除 `_WritePipe` 里的 `::FlushFileBuffers(pipe)`：消息模式管道每条 `WriteFile` 就是一条完整消息，不需要 flush；留着它等于"等服务端消费"，对端一卡就无界等待 |
| `WeaselIPC/PipeChannel.cpp` | `_Connect()` 的连接等待加上与读超时同源的**截止时间**（原来是 `while(...) WaitNamedPipe(500)` 永不放弃）；新增 `IpcReadTimeoutMs()`（默认 3000ms，可用 `RIME_WEASEL_IPC_TIMEOUT_MS` 覆盖） |
| `include/PipeChannel.h` | `_ReceiveResponse()` 改为先 `_WaitReadable()`（`PeekNamedPipe` 轮询 + 截止时间）再读；超时则关闭连接并抛 `ERROR_TIMEOUT`，由 `ClientImpl::_SendMessage` 捕获为失败 → `Echo()` 失败 → 客户端重连，**UI 线程不再被永久占用** |
| `WeaselIPCServer/WeaselServerImpl.cpp` | 响应的实际写出**移出 `g_api_mutex`**：锁内只计算（librime 非线程安全），`resp(result)` 在锁外执行 → **单个卡住的客户端只阻塞它自己的连接线程**，不再拖死所有会话 |

### 验证（可复现）
隔离运行时（`RIME_WEASEL_PIPE_NAME` + `RIME_WEASEL_USER_DIR`）+ 把隔离服务端**所有线程挂起**
模拟停顿，再用 IPC 端到端客户端（`third-party/lm-test/weasel_ipc_lm_e2e.exe`）测：

| 场景 | 修复前 | 修复后 |
|---|---|---|
| 正常路径（真实服务端） | `lm_candidate=yes` | `lm_candidate=yes`，1.0s，exit=0 |
| 服务端停顿，`RIME_WEASEL_IPC_TIMEOUT_MS=1000` | **挂死 >300s**（工具超时） | 7.8s 失败返回（多次事务各自超时），exit=2 |
| 服务端停顿，`RIME_WEASEL_IPC_TIMEOUT_MS=4000` | 同上 | 4.0s 失败返回，exit=2 |

> 修复前的挂死点正是 `FlushFileBuffers`（在连接成功、请求已写出之后），这也解释了
> 为什么之前的"无响应"分析只看到"读"这一侧是不够的。

### 仍需实施（后续）
1. **服务端只刷新有组合的会话**（当前每次 LM 通知都遍历全部会话），并合并同一时刻的多次通知——
   进一步缩短持锁时间。
2. 客户端轮询加"上一次未返回则跳过"，并加会话/上下文有效性守护（问题 A 的防御）。
3. `EndMaintenance()` 不再无条件清空整个会话表。
4. 分析 09-17 14:44:36/40 的两个 `WeaselServer.exe` 崩溃转储（`%TEMP%\rime.weasel\`）。

---

## 六、待办（按用户指定顺序）

0. **整机卡死**（第五节）：先做防御 1（客户端超时）→ 再做 2/3；并分析 09-17 的两个
   `WeaselServer.exe` 转储以定位真正的停顿点。
1. 字词重排时，把**模型认为最高概率**的字/词也标记 `[LM]`（当前只在整句/同名候选上标记）。
2. 拼音无响应的防御代码（问题 A：轮询跳过、会话/上下文守护、`EndMaintenance` 修正）。
3. 延迟优化：字词重排延迟应显著低于整句；评估用**小型 causal Transformer 专门做整句**，
   以替代当前 beam search（保留精度、降低延迟）。

---

## 七、复现/验证命令

```powershell
# 构建
cd D:\Workspace\rime\weasel
cmd /c "build.bat rime"      # 如需刷新 rime.dll（含 lm_ranker）
cmd /c "build.bat weasel"    # 0 error
# 打包
deps\nsis310\makensis.exe /DWEASEL_VERSION=0.18.0 /DWEASEL_BUILD=0 `
  /DPRODUCT_VERSION=0.18.0.0 output\install.nsi
# 无头端到端冒烟
cd D:\Workspace\rime
$env:RIME_LM_RANKER_DEBUG="1"
.\third-party\lm-test\e2e-check\weasel_lm_e2e.exe `
  --shared D:\Workspace\rime\weasel\output\data `
  --user D:\Workspace\rime\third-party\lm-test\e2e-check\user `
  --context "..." --keys beijingdaxue --timeout-ms 15000 --expect-lm
# 现场日志
%TEMP%\rime.weasel\*.INFO.*        # glog 按进程启动时刻命名
```

## 八、相关文件
| 文件 | 作用 |
|---|---|
| `weasel/RimeWithWeasel/RimeWithWeasel.cpp` | 通知过滤（黑框根因修复）、`_ShowMessage` 空提示保护 |
| `weasel/WeaselUI/WeaselPanel.cpp/.h` | DIB 表面、内容标记、定时器下限 |
| `weasel/WeaselUI/WeaselUI.cpp`、`include/WeaselUI.h` | `Show()` 幂等/内容门控、定时器状态查询 |
| `weasel/WeaselTSF/KeyEventSink.cpp`、`WeaselTSF/CandidateList.*` | LM 刷新定时器状态、轮询入口 |
| `tools/diag_black_box.ps1` | 窗口身份诊断脚本（排查 UI 问题时抓类名/进程/位置） |

---

## 九、第二轮（2026-09-18）：服务端为什么持续阻塞

用户提问："为什么服务端会持续阻塞？如果不修复这个问题，客户端就会可以恢复，也会无法输入。"
完整回答与修复记录在仓库内 **`weasel/doc/server-blocking-fix.md`**，要点：

- 客户端超时只能断开管道（唤醒对端 I/O），**无法**释放服务端的 `g_api_mutex`、
  无法把消息线程从 `lock()` 中救出、无法打断 `join_maintenance_thread()`，所以
  "应用不再卡死"与"输入法仍然不能打字"可以同时成立。
- 可证实的服务端阻塞源：① 消息线程经 `PostRime` 无限期等锁（此后 `WM_TIMER`/托盘/
  `WM_QUIT` 全停）；② 窗口操作在管道工作线程上执行（面板归消息线程所有）；
  ③ 部署/恢复在请求内、锁内做（分钟级）；④ `m_disabled` 无回报时永久禁用输入
  （锁空闲、秒回 FALSE，看起来就是"没反应"）；⑤ LM 通知洪水；⑥ IPC 卫生问题。
- 本轮修复：有界可重入 API 锁（`include/ApiLock.h`）、UI 全部投递回消息线程、
  延迟 rime 任务队列（`try_lock` + 定时器重试）、异步维护恢复 + `m_disabled` 自愈看门狗、
  LM 通知合并且只刷新组字会话、服务端 `_Send` 不再走客户端重连、接受
  `ERROR_PIPE_CONNECTED`、`/q` 改走 `WM_CLOSE` 逃生通道、锁持有看门狗日志
  （`%TEMP%\rime.weasel\weasel-watchdog.log`）。
- 验证：`weasel/tools/` 下 stall / maintenance / stress / apilock 自检全部通过；
  其中"冻结消息线程"用例在旧构建上**同样通过**，因此它只是回归护栏，不作为旧缺陷的证据
  （诚实记录在该文档第四节）。

---

## 十、第三轮（2026-09-18）：字词 [LM] 标记 + 无响应防御

仓库内文档：**`weasel/doc/lm-top-mark-and-ipc-defense.md`**；插件代码提交在
`weasel/librime/plugins/lm_ranker`（该插件是**独立 git 仓库**，不属于 librime 或 weasel 仓库）。

- **字词 `[LM]`**：字/词重排命中缓存时，把模型排第一的候选包一层 `ShadowCandidate`
  （类型 `lm_ranker`、注释 `[LM]`，text/start/end/quality 不变 → 上屏行为不变），
  已带 `[LM]` 或整句替换候选不会重复标记；配置 `mark_top`（默认 true）可关闭。
  验证：无头 `--keys beijing` 输出 `1 北京 [lm_ranker] [LM]`；IPC 端到端对 2 音节输入
  （低于整句阈值 4，故整句路径不可能产生标记）出现 `[LM]`，旧 `rime.dll` 为 0 次。
- **无响应防御**：新增 `Client::ProcessKeyEventChecked`（区分"服务端没处理"与"服务端没回答"）
  与 `DiscardConnection`；LM 轮询遇事务失败立即取消整条链；`_EnsureServerConnected` 失败后
  1 秒冷却；连续 3 次失败执行 `_RecoverStalledSession()`（仅本地复位，等价于"切换输入法再切回来"）。
- **顺带修掉真实缺陷**：`_WaitReadable` 原来只累加 `Sleep(2)`、不计 `PeekNamedPipe` 开销，
  预算被放大 ~7.8 倍（默认 3000ms 实测阻塞 23.4 秒）；改为墙钟判定后为 999ms/2999ms。
  新增 `RIME_WEASEL_IPC_TRACE=1` 记录慢 IPC 阶段到 `weasel-watchdog.log`。

---

## 十一、第四轮（2026-09-18）：延迟评估与优化

仓库内文档：**`weasel/doc/lm-latency-evaluation.md`**（含全部实测数据与复现命令）。

- **评估**（隔离环境、按 TSF 节奏轮询、每次运行用独立上下文）：优化前 单字 684ms /
  词组 826ms（且有超时）/ 整句 279ms。归因发现瓶颈**不是轮询**，而是"每次按键都做一遍重排、
  且正在打分的那条不可中断"——作废的作业把要看的结果挡在后面 0.1~1.0 秒。
- **优化 1（插件）**：可取消打分（批次边界检查，批次 64→16）+ `max_context` 真实生效
  （此前它只截断上下文字符串，编码窗口恒为 256）。
- **优化 2（客户端+服务端+插件）**：`lm_pending` 信号 + 默认轮询节奏 120/120 → 60/60，
  结果就绪即停止轮询（每键 12 次 → 1~3 次；所有运行 `unsafe_early_stop=0`）。
- **优化 3（插件，按用户要求）**：**自适应上下文窗口**
  `clamp(round_up_8(实际字数+8), min_context, max_context)`，两个边界都可在
  `lm_ranker.yaml` 配置（默认 32/256；两值相等即固定窗口 = 旧行为）。
  实测单条作业：窗口 32 → 15ms，96 → 40ms，200 → 80ms，固定 256 → 127~137ms；
  模型最终选中与固定 256 一致 26/28、与固定 32 一致 24/28（差异源于绝对位置编码，
  自适应窗口不丢真实上文）。
- **最终延迟**（自适应默认）：轮询 60ms 下单字/词组/整句均 **78ms**（1~3 次轮询），
  轮询 20ms 下 **47ms**。
- **附带修复**：隔离实例（私有管道名）不再弹出通知提示——此前测试夹具的方案列表含不存在的
  `quick5`，部署失败会在用户桌面**左上角**弹"有错误"提示；同时失败提示改为显示真实日志目录。
