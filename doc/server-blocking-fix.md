# WeaselServer 持续阻塞的定位与修复（2026-09-18 第二轮）

承接 `lm-ranker-ui-fixes.md`。上一轮解决了整机卡死（`FlushFileBuffers` 持锁 +
客户端无界等待），但用户提出关键问题：

> 为什么服务端会持续阻塞？如果不修复这个问题，客户端就算能（靠超时）恢复，也依然无法输入。

本文档回答这个问题，并记录本轮的服务端修改与验证。

---

## 一、为什么"客户端超时"救不了服务端

客户端超时后做的是 `_FinalizePipe` + `CloseHandle`（`include/PipeChannel.h`）。关句柄
只能唤醒**对端的 I/O 等待**（`ReadFile`/`WriteFile` 立刻返回），它**不能**：

1. 释放服务端持有的 `g_api_mutex`（持锁者不是在等 I/O）；
2. 把服务端消息线程从 `lock()` 里救出来（它既不泵消息、也不处理 `WM_QUIT`）；
3. 打断 `join_maintenance_thread()` 这类同步等待。

于是现象正好是用户描述的那样：**应用不再卡死，但每次按键都变成"等一个超时 → 失败返回 0"**
（`ClientImpl::_SendMessage` 返回 0 → `ProcessKeyEvent` 返回 false → 不上屏、不出候选），
托盘"退出"与 `WeaselServer.exe /q` 也走同一条管道，所以只能任务管理器结束进程。

## 二、阻塞源清单（按代码事实）

| # | 阻塞源 | 事实依据 | 本轮处置 |
|---|---|---|---|
| 1 | 消息线程会**无限期**等这把锁 | `PostRime` = `Post` + `lock(g_api_mutex)`，任务在消息循环里执行（`WeaselServerImpl.cpp`）→ LM 每完成一次解码就可能把消息线程钉在锁上；此期间 `WM_TIMER`、托盘刷新、`WM_QUIT` 全部停摆 | 改为**有界获取 + 定时器重试队列**（`_DrainDeferredRimeTasks`，`try_lock` + 5s 预算），消息线程永不阻塞 |
| 2 | **窗口操作在管道工作线程上执行**（同时持锁） | `ProcessKeyEvent→_UpdateUI→m_ui->Hide()/Update()/UpdateInputPosition()`、`OnUpdateInputPosition→MoveTo→SetWindowPos`、`UIImpl::Show/Hide→ShowWindow`、`DoPaint→UpdateLayeredWindow`。而面板窗口由**消息线程**创建并拥有（`WeaselServerApp.cpp:30/41`） | UI 全部**投递回消息线程**执行（`_UpdateUI`/`_HideUI`/`ApplyInputPosition` + `OnPostUIToServerThread`），工作线程只留"锁内计算响应体" |
| 3 | 部署/恢复在**请求内、锁内**执行 | `AddSession`/`EndMaintenance` → `Initialize()` → `start_maintenance()` + **`join_maintenance_thread()`**（一次部署可达分钟级） | 改为后台线程恢复（`_ResumeMaintenanceAsync`，带 2s 节流），请求立即返回 |
| 4 | `m_disabled` 可能永久卡住 | 部署若没回报"完成"，`ProcessKeyEvent` 永远返回 FALSE：**锁是空闲的、服务端秒回，但完全不能打字** | 新增 `OnMaintenanceWatchdog`（消息线程，每秒）：disabled 超过 5s 且 `WeaselDeployerMutex` 未被占用 → 自动后台恢复 |
| 5 | LM 通知洪水 | 每次解码完成 → 一个 PostRime 任务 → 持锁遍历**所有**会话 `refresh_non_confirmed_composition` + 全量 `_UpdateUI` | 通知**合并**（`m_lm_refresh_posted`，最多一个在途）+ 只刷新**正在组字**的会话 |
| 6 | IPC 卫生问题 | ① 服务端 `_Send` 失败时会走**客户端重连**逻辑 → 连到自己的管道、泄漏实例与工作线程；② `_ConnectServerPipe` 把 `ERROR_PIPE_CONNECTED`（Create 与 Connect 之间的竞态）当错误丢弃**已连上的连接** | ① `_ReconnectOnSendFailure()` 虚函数，服务端不重连、直接断连；② 接受 `ERROR_PIPE_CONNECTED` |
| 7 | 无法优雅退出 | 消息线程被钉住时 `WM_CLOSE`/托盘退出都无效 | `/q` 改为向服务端窗口 `PostMessage(WM_CLOSE)`（不走管道、不持锁）；无窗口时回退旧路径 |

另外澄清一处上一轮文档的表述：**响应的实际写出上一轮就已经移出锁了**
（`HandlePipeMessage` 内的 `eat` 只是写线程本地缓冲区，真正的 `_Send` 在锁外的
`resp(result)` 里），本轮未改动这一点。

---

## 三、实现要点

### 1. 有界 API 锁（新文件 `weasel/include/ApiLock.h`）
- `std::timed_mutex` + **同线程可重入**（`owner_tid_` + 深度），避免 `Initialize()` 在
  `EndMaintenance()` 已持锁时自锁死；
- 记录持有者操作标签与持有时长（`Inspect()`），供看门狗报告；
- `ApiLockGuard` RAII；管道工作线程预算 `IpcReadTimeoutMs()-200`（默认 2800ms，**短于客户端
  超时**，因此客户端总能收到"未处理(0)"而不是超时）；消息线程预算 3000ms，超时则跳过本次
  窗口刷新（下一次按键/轮询会重新应用最新状态）。

### 2. UI 回消息线程（`RimeWithWeasel.{h,cpp}`、`WeaselServerApp.cpp`）
- `OnPostUIToServerThread(m_server.Post)`：只投递、不加锁；
- `_UpdateUI(ipc_id, add_session)` 锁内只**投递**，`_ApplyUILocked` 在消息线程上取锁后
  执行原 `_UpdateUI` 逻辑（`_ApplyUI`）；无消息循环的宿主（测试）退回同步执行；
- `RemoveSession`/`FocusOut` 的 `Hide()`、`UpdateInputPosition` 同样投递；
- 好处：窗口只被属主线程触碰；重复请求**自动合并**（每次都在消息线程重新读取最新状态）。

### 3. 消息线程的有界等待 + 延迟任务队列（`WeaselServerImpl.{h,cpp}`）
- `PostRime` 不再直接 `lock()`：任务进入 `m_deferred_rime_tasks`（上限 8 条，5s 预算），
  由窗口定时器（500ms）用 `try_lock` 执行，失败则留待下次；超预算丢弃并写诊断日志；
- 看门狗**独立线程**（`watchdogThread`）：窗口定时器**看不到消息线程自己的持锁**，
  而这正是"整个服务端像死了"的那种持锁，所以持锁超时监控必须放在别的线程上；
- 阈值默认 5000ms，可用 `RIME_WEASEL_LOCK_WARN_MS`（50..600000）调整，
  `RIME_WEASEL_WATCHDOG_VERBOSE=1` 时会记录"看门狗已启动"。

### 4. 诊断输出（`ApiLock.h::WatchdogLog`）
Weasel 自己的 `LOG()` 宏在未定义 `WEASEL_ENABLE_LOGGING` 时是**空操作**
（`include/logging.h` → `no_logging.h`），所以关键诊断写到用户已知的日志目录：
`%TEMP%\rime.weasel\weasel-watchdog.log`（同时 `OutputDebugString`）。记录内容：
- `api lock held for N ms by '<op>'`（每个持有段最多一条）
- `api lock timeout while handling <cmd> ...; replying 0`
- `candidate window refresh skipped: api lock busy ...`
- `service disabled for N ms with no deployer running; resuming`
- `dropping deferred rime task ...`

---

## 四、验证

隔离运行方式（私有管道 + 私有用户目录，不影响正在使用的输入法）：
`RIME_WEASEL_PIPE_NAME`、`RIME_WEASEL_USER_DIR`、`RIME_WEASEL_IPC_TIMEOUT_MS`。

| 测试 | 脚本 | 结果（最新构建） |
|---|---|---|
| 消息线程冻结下的请求路径 | `weasel/tools/ipc_stall_test.ps1` | 冻结前 0.9s / **冻结中 0.4s，`lm_candidate=yes`** / 恢复后 0.4s；优雅退出（`WM_CLOSE`）成功 |
| 维护往返（Start→End→恢复打字） | `weasel/tools/ipc_maintenance_test.ps1` | `EndMaintenance` **15ms** 返回，**329ms** 后自动恢复打字（`[LM]` 正常），无告警 |
| 5 客户端并发 × 2 轮 | `weasel/tools/ipc_stress_test.ps1` | 10 次全部 `lm_candidate=yes`、exit=0，每轮约 3.3–3.6s，**无锁持有告警** |
| 锁/看门狗自检 | `weasel/tools/apilock_selftest.cmd` | 互斥、可重入、跨线程有界失败、持有时长统计、日志落盘全部通过 |

**必须诚实说明的一点**：`ipc_stall_test.ps1` 的"冻结消息线程"用例在**旧构建上同样通过**——
也就是说它**没有**复现出"跨线程窗口操作会被挂起的属主线程拖住"这一具体机制（隐藏/位置未变的
窗口操作未必需要属主线程参与）。该用例因此是**回归护栏**（证明请求路径不再依赖消息线程），
而不是旧代码缺陷的证明。旧代码可证实的缺陷是第二节表格里那些有明确代码依据的项
（消息线程无限期等锁、请求内做分钟级部署、UI 跨线程、通知洪水、IPC 卫生）。

同理，`ipc_maintenance_test.ps1` 在旧构建上也通过（工作区已是最新时 `Initialize()` 很快）；
它证明的差异是**行为**：禁用期内的会话请求在旧构建里被**同步**恢复（`disabled session=1`，
62ms），新构建**立即返回 0** 并由后台线程恢复（0ms + 329ms 后可用）。

---

## 五、现场如何取证（下次复现时）

1. 看 `%TEMP%\rime.weasel\weasel-watchdog.log`：若有 `api lock held for N ms by '<op>'`，
   `<op>` 直接指出持锁操作（如 `process_key_event`/`start_session`/`initialize`/`apply_ui`）。
2. 想更灵敏：设 `RIME_WEASEL_LOCK_WARN_MS=200`（仅诊断用）。
3. 卡住时先尝试 `WeaselServer.exe /q`（现在走 `WM_CLOSE`，不依赖管道）；再不行才结束进程，
   结束后的下一次按键会由 TSF 自动重新拉起服务。
4. 若仍无告警而输入失效，重点转向第四节表格之外的两处：
   `%TEMP%\rime.weasel\*.INFO.*` 是否停住、以及客户端侧 `_lm_refresh_pending` 状态。

---

## 六、仍待完成

1. **IPC 写侧截止时间**：`WriteFile` 仍可能在对端"活着但不读"时阻塞（现在它发生在锁外，
   只影响该连接，因此优先级下降）；彻底解决需 `FILE_FLAG_OVERLAPPED` + `CancelIoEx`，
   会同时改动读写两侧，作为独立一轮更稳妥。
2. 客户端 LM 轮询（每 120ms）与 `_ScheduleLmRefresh` 的"上一次未返回则跳过"（用户待办 2）。
3. 字词重排也给最高概率候选标 `[LM]`（用户待办 1）。
4. 延迟优化 / 小型 causal Transformer 整句模型（用户待办 3）。
5. 分析 09-17 14:44:36/40 的两个 `WeaselServer.exe` 转储。

## 七、本轮改动文件
| 文件 | 改动 |
|---|---|
| `weasel/include/ApiLock.h`（新增） | 有界/可重入全局锁、持有者诊断、`WatchdogLog` 落盘 |
| `weasel/WeaselIPCServer/WeaselServerImpl.{h,cpp}` | 有界请求锁、延迟 rime 任务队列、看门狗线程、托盘/主题路径加锁、`WM_TIMER` 看门狗钩子 |
| `weasel/include/WeaselIPC.h` | `RequestHandler::OnMaintenanceWatchdog()` |
| `weasel/include/RimeWithWeasel.h`、`weasel/RimeWithWeasel/RimeWithWeasel.cpp` | UI 投递回消息线程、`Initialize/Finalize/UpdateColorTheme` 加有界锁、异步维护恢复、`m_disabled` 原子化、LM 通知合并与只刷新组字会话、`add_session` 由全局改为参数 |
| `weasel/WeaselServer/WeaselServerApp.cpp` | 注册 UI 投递器 |
| `weasel/WeaselServer/WeaselServer.cpp` | `/q` 走 `WM_CLOSE` 逃生通道 |
| `weasel/include/PipeChannel.h`、`weasel/WeaselIPC/PipeChannel.cpp` | 服务端不重连、接受 `ERROR_PIPE_CONNECTED` |
| `weasel/tools/ipc_stall_test.ps1`、`ipc_maintenance_test.ps1`、`ipc_stress_test.ps1`、`apilock_selftest.{cc,cmd}`（新增） | 隔离回归测试 |
| `third-party/lm-test/weasel_ipc_maintenance_e2e.cc`、`build_weasel_ipc_maintenance_e2e.cmd`（新增） | 维护往返端到端客户端 |

## 八、提交与打包

- 提交：`ac965de` "Bound the server api lock and move window work to the message thread"
  （分支 `weasel` 工作树干净，仅剩 `? librime` 子模块标记）。
- 构建：`build.bat weasel`（x64 + Win32，0 error）→
  `deps\nsis310\makensis.exe /DWEASEL_VERSION=0.18.0 /DWEASEL_BUILD=0 /DPRODUCT_VERSION=0.18.0.0 output\install.nsi`
  → `output/archives/weasel-0.18.0.0-installer.exe`（50,637,635 字节，11:26）。
- 包内校验：内含 x64 `WeaselServer.exe` 2,342,912 字节（11:22:14 构建）与 Win32
  1,995,264 字节（11:22:20 构建）；`rime.dll` 仍为 09-17 17:37（本轮未改 librime）。
  用 7-Zip 解包时同名文件会被折叠（只剩 Win32 副本），因此逐项哈希只对 Win32 有效
  （`WeaselServer.exe`/`rime.dll` 均 MATCH），x64 以归档内的体积+时间戳与构建产物比对。
