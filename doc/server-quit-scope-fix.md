# `/q` 误杀用户服务端：定位与修复

## 一、现象（用户报告）

> 使用五笔有时会突然无法打字，只能输入英文字母，需要重启算法服务才能继续打字。
> 请检查一下日志，是你在隔离环境影响了生产环境的 server，还是 server 由于某种原因意外退出了。

## 二、结论

**不是"意外退出"，也不是隔离实例的干扰：是我（开发侧）的每一次构建都把它关掉了。**

- 今天（09-18）服务端重启 6 次，每次都在我执行 `build.bat` 之后（见第三节的秒级对应）。
- 今天**没有任何崩溃转储 / WER 报告** → 它是被"正常关闭"的，不是崩的。
- 关闭它的正是 `build.bat:129-133` 开头那句 `output\weaselserver.exe /q`。

顺带发现两件**独立**的事（第四节、第五节）：`WeaselServer.exe` 的所有命令行开关
（`/q /quit /userdir /weaseldir /ascii /nascii /update`）**从来没有生效过**；
以及 09-17 生产服务端确实崩溃过两次（`rime.dll`，确定性地址）。

## 三、证据

### 1. 服务端生命周期与构建产物的秒级对应

`build.bat` 在**编译之前**先执行 `/q`，所以"上一次构建产物写盘时刻"应当略晚于
"服务端被杀时刻"。生产会话日志的文件名就是服务端启动时刻：

| 上一实例最后一条日志 | 我这一侧的构建产物时刻 | 新实例启动（TSF 自动拉起） |
|---|---|---|
| 16:12:34 | `weaselx64.dll` 16:13:06、`weasel.dll` 16:13:47 | `...161415.16540` → 16:14:15 |
| **16:22:11** | **`WeaselServer.exp` 16:22:13**（链接器写盘） | `...162254.35632` → 16:22:54 |
| 16:41:48 | 16:42 前后的构建 | `...164302.34844` → 16:43:02 |
| 17:13:42 | 17:13/17:14 的构建 | `...171452.7120` → 17:14:52 |
| **17:17:39** | **`output\rime.dll` 17:17:45** | `...171928.36552` → 17:19:28 |

新实例总是比"被杀"晚 **40–110 秒**出现，这正是 TSF 那套"6 次失败 + 1 秒冷却后才拉起
`start_service.bat`"的节奏——也就是用户看到的"只能打英文字母"的那段真空期。

### 2. 今天没有崩溃

`%TEMP%\rime.weasel\` 下 09-18 没有任何 `*.dmp`，WER 也没有今天的 `WeaselServer` 报告。

### 3. 为什么构建会杀掉**用户安装的**服务端：两个缺陷

**缺陷 A（真正的杀手）：`/q` 根本没被解析。**

```cpp
bool quit = !wcscmp(L"/q", lpstrCmdLine) || !wcscmp(L"/quit", lpstrCmdLine);
```

CRT 传给 `_tWinMain` 的 `lpstrCmdLine` 仍带着与程序名之间的空白（某些启动方式甚至给的是
整条命令行），所以 `wcscmp` 永远不相等 → `quit == false` → 代码掉进下面这段：

```cpp
// restart if already running
weasel::Client client;
if (client.Connect()) {      // 连的是"每用户共享"的那条管道
  client.ShutdownServer();   // ← 把用户的服务端关了
  ...
}
...
app.Run();                   // ← 该进程自己变成服务端，于是 /q 永不返回
```

实测（无任何服务端在跑时）`output\WeaselServer.exe` 加 `/q`、`/nascii`、`/weaseldir`
**都不退出**，而是变成服务端并一直挂着（`Start-Process -Wait` 因此卡了 120 秒）。

**缺陷 B（为什么管道会命中用户的服务端）：所有安装共用同一身份。**
`WEASEL_IPC_WINDOW = L"WeaselIPCWindow_1.0"` 是固定类名，管道名默认也是"每用户"而非
"每安装"。于是"谁应答管道/谁的窗口"并不等于"构建目录的服务端"。

> 影响面：`/userdir`、`/weaseldir`、`/ascii`、`/nascii`、`/update` 同样从未生效，
> 连带 `install.nsi:164/217/410` 的 `ExecWait '"...\WeaselServer.exe" /quit'`
> 和 `install.nsi:383-385` 的三个开始菜单快捷方式（用户文件夹 / 程序文件夹 / 检查更新）
> 都是坏的。

## 四、修复

1. **按 token 解析命令行**（`HasCommandLineOption()`）：容忍前导空白与引号；
   `/q`、`/quit`、`/userdir`、`/weaseldir`、`/ascii`、`/nascii`、`/update` 全部改用它。
2. **按可执行文件路径识别身份**：`FindServerProcesses()` 枚举所有 `WeaselServer.exe`
   并读取其镜像路径；**并把调用者自身排除**——否则 `/q` 会把自己算成"我的服务端"，
   于是"拒绝接管管道"的守卫失效（这正是缺陷 A 之外让缺陷 B 生效的原因）。
3. **`/q` 只杀自己**：只对镜像路径与自身相同的窗口 `PostMessage(WM_CLOSE)`；
   只有在"自己的服务端在跑、且没有任何外部服务端"时才回退到管道；其余情况一律
   **不碰外部服务端**。关闭后 `WaitForOwnServersToExit(5s)`，使安装脚本的
   `ExecWait ... /quit` 真正同步（返回时文件已可替换）。
4. **裸启动不再抢管道**：若管道当前由**其它目录**的服务端持有，直接返回 1，
   不再把用户的服务端踢下线。
5. **TSF 自愈**：`_EnsureServerConnected()` 在"根本没有任何 `WeaselServer.exe` 进程"时
   立即（5 秒节流）拉起服务，而不是等 6 次失败；服务端崩溃或被外部停止后，
   用户不必再手动重启。

## 五、另一个独立发现：09-17 生产服务端真的崩过两次

- 转储：`WeaselServer.exe.23032.dmp`（09-17 14:44:36）、`WeaselServer.exe.892.dmp`（14:44:40）。
- 模块表指向 `D:\Software\Rime\weasel-0.18.0\`（用户的安装目录），且加载了
  `onnxruntime.dll`（说明当时 LM 插件在跑）。
- 解析 minidump：异常 `0xC0000005`（访问违例），**两次地址完全相同**：
  `rime.dll + 0x439E4`（模块基址 0x7FFD3E720000）→ 确定性缺陷，不是随机内存踩踏。
- **无法定位到函数**：崩溃时那份 `rime.dll` 是 09-17 的构建，其 PDB 已被后续构建覆盖
  （`librime\dist_x64\lib\rime.pdb` 现为 09-18 17:17）；两个 WER 报告目录 ACL 拒绝读取
  （需管理员）。这条线索**尚未闭环**：若再出现，请保留 `.dmp` 与**当时的 `rime.dll`**，
  用配套 pdb 解析 `+0x439E4` 即可定位。

## 六、验证

`weasel/tools/ipc_quit_scope_test.ps1`（新增）：

| 用例 | 期望 | 结果 |
|---|---|---|
| 1a 从构建目录、**干净环境**执行 `/q` | 外部（隔离）服务端存活 | ✅ `exit=0 alive=True` |
| 1b 同 1a 但带隔离环境（覆盖管道回退路径） | 外部服务端存活 | ✅ `exit=0 alive=True` |
| 2 从**自身目录**执行 `/q` | 隔离服务端退出 | ✅ `exit=0 stopped=True` |

`RESULT ipc_quit_scope_test=pass`

端到端复现用户场景：**在用户已安装的服务端运行期间执行一次完整 `build.bat weasel`**
（该构建会真的执行新的 `/q`）→ 监视 110 秒，服务端 PID/启动时间不变，
且没有多出 `output\` 目录的服务端。✅

> 如实说明：本轮最初那次测试（当时用的还是"只修了一半"的二进制：路径守卫已加、
> 但 `/q` 仍未解析 → 仍走管道）确实把用户的服务端停掉过一次；随后按
> `start_service.bat` 的方式恢复，并用上面这组用例回归确认不再发生。

## 七、改动文件

| 文件 | 改动 |
|---|---|
| `weasel/WeaselServer/WeaselServer.cpp` | 命令行按 token 解析；按镜像路径识别服务端并排除自身；`/q` 只关自己的服务端并等待其退出；裸启动拒绝抢占外部服务端的管道 |
| `weasel/WeaselTSF/WeaselTSF.{h,cpp}` | 服务端进程完全不存在时立即（5 秒节流）自愈拉起 |
| `weasel/tools/ipc_quit_scope_test.ps1`（新增） | 上述 `/q` 作用域回归测试 |
