# lm_ranker 配置参考

配置文件：`%APPDATA%\Rime\lm_ranker.yaml`（不存在时使用内置默认值）。
**配置只在 WeaselServer 启动时读取一次**——修改后需要重启服务（托盘菜单退出「小狼毫算法服务」，或任务管理器结束 WeaselServer.exe，它会自动重启；也可 `WeaselServer.exe /quit` 后手动启动）。

## 全部可用项（等号后为默认值）

```yaml
lm_ranker:
  enabled: true
  # model_dir: 缺省 %AppData%\Rime\lm-mlm（首次运行自动从安装目录 data\lm-mlm 复制）
  model_file: model.onnx        # 或 model.int8.onnx（小 25MB，精度略低）
  prior_beta: 0.25              # 候选重排融合 z(LM)+beta*z(IME 先验)
  max_candidates: 50            # 每次送 LM 打分的候选上限（按 IME quality 取 top-K）
  max_candidate_length: 8       # 超过该字数的候选不重排
  full_span_only: true          # 只重排消费完整输入的候选
  max_context: 256              # 上下文最多取光标前多少字（32~256，超出自动钳制）
  ui_refresh:
    enabled: true               # 自动把后台 LM 结果刷新到候选窗
    initial_ms: 120             # 停止输入后首次读取异步候选的延迟
    interval_ms: 120            # 后续读取候选快照的间隔
    timeout_ms: 1500            # 整次后台刷新窗口上限；不阻塞输入
  chars:
    rerank: true
  words:
    rerank: true
  sentence:
    enabled: true
    min_syllables: 4            # 触发整句生成的最少音节数
    debounce_ms: 120            # 输入停顿多久后开始解码
    beam: 4                     # 束宽（1=贪心；更大更准更慢）
    topk: 8                     # 每步字符候选数
    promote: smart              # always / smart / off
    promote_margin: 0.5         # smart 阈值 T（只影响顺序，见下）
    always_show: true           # smart 未过阈值时整句仍插入（排在 IME 首选之后）
    word_bonus: 0.0             # 词边界加分（实验特性，默认关）
    pron_weight: 0.5            # 每字打分 + pron_weight*log P(读音|字)
```

## 异步刷新与手动部署

候选重排和整句生成都在后台执行；输入停止后，Weasel 会在
`ui_refresh.initial_ms` 后开始读取最新候选，并以 `interval_ms` 轮询，直到
`timeout_ms`。因此不应再需要输入一个额外字符再删除来触发刷新。把
`ui_refresh.enabled` 设为 `false` 可关闭这条自动刷新链路。

Windows 的 TSF 客户端 DLL 由系统加载，而不是由 `WeaselServer.exe` 加载。
在 64 位 Windows 上，`WeaselSetup`/安装器会部署为：

- 64 位应用：`C:\Windows\System32\weasel.dll`，来源是构建产物
  `weaselx64.dll`（部署时改名）。
- 32 位应用：`C:\Windows\SysWOW64\weasel.dll`，来源是构建产物
  `weasel.dll`。

所以手动更新时，只替换安装目录中的 `weaselx64.dll` / `weasel.dll` 不会让
记事本、VS Code 等应用加载新 TSF 代码。优先使用安装器或
`WeaselSetup.exe` 部署；若必须手动替换，需要管理员权限，先关闭正在使用
输入法的应用，再替换上述系统目录中的 DLL 并重新打开应用。服务端则仍从
安装目录加载，替换 `WeaselServer.exe` 后重启“小狼毫算法服务”。

## smart 置顶判据（2026-09-16 起：阈值只决定顺序）

```
margin = mean_logprob(MLM 句 | 上下文) - mean_logprob(IME 首选句 | 上下文)
margin >  promote_margin  → MLM 整句插到第 1 位（IME 首选退到第 2 位）
margin <= promote_margin  → MLM 整句插到第 2 位（IME 首选保持第 1 位）
```

- 整句**始终**进入候选列表（只要生成成功且与现有候选不完全重复）
- `always_show: false` 可恢复旧行为（未过阈值时完全不插入）
- `promote: always` 无条件插到第 1 位；`promote: off` 不生成整句

600 例标定（`smart-promote-calib.md`）：T=0.5 综合最优；调低会增加置顶但也会置顶更多错误句子。

| T | hard 100 例 | words 100 例 | hard 保持 300 | words 保持 300 |
|---|---:|---:|---:|---:|
| -1.0（≈always） | 42.4% | 78.0% | 51.0% | 73.3% |
| 0.0 | 48.5% | 85.0% | 53.3% | 80.3% |
| **0.5（默认）** | **55.6%** | **86.0%** | **51.7%** | **85.0%** |
| 0.7 | 55.6% | 85.0% | 50.0% | 85.7% |
| 1.0 | 52.5% | 85.0% | 48.3% | 84.7% |

（总体准确率 = 置顶时取 MLM 整句、未置顶时取 IME 首选的命中率；修复后重新标定，T=0.4~0.7 为平台，0.5 综合最优）

## 已知口径（2026-09-16 修复）

- **OOV 上下文编码**：插件曾把词表外字符映射成 `[UNK]`（英文/标点多的上下文会劣化解码，如 `daikuanfangmian → 贷款方面`）；已改为与评测一致的"丢弃 OOV 字符"，修复后同一上下文可正确解出 `带宽方面`
- 上述两表为修复后重新标定的结果；T=0.5 仍为综合最优
