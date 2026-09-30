# M3：x86 构建与安装包（2026-09-16）

## 产物
- `weasel/output/archives/weasel-0.18.0.0-installer.exe`（50.6 MB，未签名，NSIS 3.10）
- 内置：x64 + Win32 双架构 `rime.dll`（含 lm_ranker 与 librime-lua 插件）、`WeaselServer/Deployer/Setup`、x64/Win32 `onnxruntime.dll`（1.20.1，CPU）、fp32 模型（`data/lm-mlm/model.onnx` 35.2 MB + `vocab.txt` + `syllable_readings.tsv` + `lm_config.json`）、许可文件

## 构建链
```powershell
# 0) 一次性：拉取 lua 插件（缺失时 wubi98 等 lua 方案会报 translator 错误）
lm-test\setup_lua_plugin.cmd
# 1) 补全 x86 Boost（首次需要；b2 缓存可能误判 up-to-date，用 -a 强制）
cmd /c "cd /d D:\Workspace\rime\third-party\boost_1_84_0 && ^
  b2 toolset=msvc-14.3 architecture=x86 address-model=32 link=static ^
  runtime-link=static --with-filesystem --with-json --with-locale --with-regex ^
  --with-serialization --with-system --with-thread -j16 -a stage"
# 2) librime x64 + Win32（插件自动编入 rime.dll；dist_x64 / dist_Win32）
cmd /c "build.bat rime"      # 工作目录 D:\Workspace\rime\weasel
# 3) Weasel x64 + Win32
cmd /c "build.bat weasel"
# 4) 安装包（NSIS 3.10 解包到 deps\nsis310，或安装版；WEASEL_VERSION 用于版本号）
cmd /c "set WEASEL_VERSION=0.18.0&& set VERSION_MAJOR=0&& set VERSION_MINOR=18&& set VERSION_PATCH=0&& build.bat installer"
# 或直接：deps\nsis310\makensis.exe /DWEASEL_VERSION=0.18.0 /DWEASEL_BUILD=0 /DPRODUCT_VERSION=0.18.0.0 output\install.nsi
```

## 安装包行为（本 fork 的新增点）
1. **ORT 运行时**：x64 装到 `$INSTDIR\onnxruntime.dll`，32 位系统装 `$INSTDIR\Win32\onnxruntime.dll`（`rime.dll` 动态解析同目录 DLL；缺失时功能自动关闭，不影响打字）
2. **模型**：装到共享数据目录 `$INSTDIR\data\lm-mlm\`
3. **首次运行模型复制**：插件发现 `%AppData%\Rime\lm-mlm\model.onnx` 缺失时，从共享数据目录 `lm-mlm` 复制（多用户机/升级兜底）；已实测复制一次并正常置顶
4. **默认启用**：安装器在 `%APPDATA%\Rime\luna_pinyin.custom.yaml` **不存在时**写入 `engine/filters/+: [lm_ranker]`；luna 系全部变体（`luna_pinyin_simp` / `luna_quanpin` / `luna_pinyin_fluency`）都 `__include: luna_pinyin.schema` 并继承其 custom 补丁，**只需这一个文件**（写多个会导致同一 filter 在 simp 里重复两次）
5. **更新检查**：本版固定 `CheckForUpdates=0`（不再询问，无 WinSparkle 弹窗）
6. **卸载**：清理 `data\lm-mlm`（含升级安装路径）；用户数据目录与自定义 custom.yaml 保留
7. **TSF 客户端部署**：`WeaselSetup` 将 x64 `weaselx64.dll` 以
   `weasel.dll` 名称安装至 `C:\Windows\System32`，将 x86 `weasel.dll`
   安装至 `C:\Windows\SysWOW64`。应用进程从这两个系统路径加载 TSF，
   因此更新安装目录内同名 DLL 不足以更新正在测试的客户端。

## librime-lua 插件（必须）
- 官方 Weasel 自带 librime-lua（用户可能使用 `lua_translator`，如本机 wubi98 的 `wubi98_wildcard`）；自建 rime.dll 不含它时日志出现 `error creating translator: 'lua_translator'`，相关功能静默失效
- 修复：`lm-test/setup_lua_plugin.cmd`（克隆 librime-lua 到 `librime/plugins/lua` 并拉取内置 Lua 源码）→ `build.bat rime` 自动编入 rime.dll（日志 `registering components from module 'lua'`）
- 校验：在 rime.dll 中查找 `lua_translator` 字符串；wubi98 引擎创建无 translator 错误

## 验证结果
| 项目 | 结果 |
|---|---|
| x64 `rime.dll` / Win32 `rime.dll` 机器类型 | x64 / x86（dumpbin） |
| `onnxruntime.dll` 导入 | 无（动态加载，缺 DLL 可降级） |
| x64 回归（hard/words 100 例） | 55 / 86（与手动构建一致） |
| x86 回归（32 位抽取器，x64 Windows 运行） | 55 / 100（`--wait 2000`；`--wait 800` 时因解码慢而置顶较少，属测试窗口问题） |
| Win32 WeaselServer/Deployer | 链接成功（补齐 `lib\rime.lib` 后） |
| 安装包内容 | 双架构 rime/ORT、模型 4 文件、LICENSE-ORT / THIRD-PARTY-NOTICES / ORT ThirdPartyNotices 均在包内；x64 `rime.dll` 含 lua_translator 与 lm_ranker |
| 首次运行模型复制 | 空目录启动 → 复制 4 文件 → 冒烟 20 例置顶 10 例 |

## 本机实装验证（2026-09-16 晚）
| 项目 | 结果 |
|---|---|
| 安装 | `D:\Software\Rime\weasel-0.18.0`，Server 运行中，注册表 `WeaselRoot` 已更新 |
| 插件日志 | `registering components from module 'lm_ranker'` → `model copied ... to %APPDATA%\Rime\lm-mlm` → `model loaded: model.onnx, vocab 21128, syllables 416` |
| 配置 | 无 `lm_ranker.yaml` 时按默认值运行（日志 WARNING，预期） |
| 发现并修复① | 安装器写了 4 个 custom.yaml，simp 编译出**两个** lm_ranker filter（重复打分/重复请求解码）；删除多余 3 个 → 重新部署后编译 schema 仅 1 个 |
| 发现并修复② | 自建 rime.dll 缺 lua → wubi98 报 translator 错误；补 lua 重建并热替换 rime.dll，日志 0 错误 |
| 发现并修复③（2026-09-17） | 异步 LM 结果仅在“额外输入再删除”后才显示：服务端已更新但应用仍从 `System32` / `SysWOW64` 加载旧 TSF DLL。用 WeaselSetup 部署对应 DLL 并重启应用后，候选可自行重排/显示 `[LM]` |
| 安装包 | 已重打包（含 lua、仅写 1 个 custom.yaml），50.6 MB |

## 安装验证清单
1. 双击安装包 → 选择目录 → 完成后确认托盘出现小狼毫（`%APPDATA%\Rime\lm_ranker.yaml` 可不存在，默认值生效）
2. `%APPDATA%\Rime\luna_pinyin.custom.yaml` 已生成且含 `lm_ranker`（其余 luna 变体继承，无需额外文件）
3. 记事本输入拼音（≥4 音节、有上文时）：约 0.2~1 s 后整句候选出现在首位；候选应自行刷新，不需要追加再删除一个字符
4. 32 位应用内输入：功能正常（32 位 TSF + IPC 到 x64 服务）
5. 删除 `$INSTDIR\onnxruntime.dll`：重启服务后输入不崩溃、无置顶（降级）
6. 删除 `%AppData%\Rime\lm-mlm\*`：首次输入自动重建
7. wubi98（或其他 lua 方案）：无 `error creating translator: 'lua_translator'` 日志
8. 卸载：`$INSTDIR` 与 `data\lm-mlm` 清理；`%APPDATA%\Rime` 用户数据保留
9. 覆盖安装（升级）：旧 `data` 备份恢复、模型目录保留用户覆盖文件
10. 用 Process Explorer 或同类工具检查：64 位应用加载
    `C:\Windows\System32\weasel.dll`，32 位应用加载
    `C:\Windows\SysWOW64\weasel.dll`；哈希应与本次构建产物一致

## 未做/待办
- ARM64X（`build.bat arm64` 路径未验证；install.nsi 已带 `/nonfatal` 引用）
- 安装包代码签名（SmartScreen 会提示）
- 本机实装已完成；清洁机/多用户场景的安装测试仍建议后续补做
- `smart` 阈值已在 600 例上复校（见 `smart-promote-calib.md`），真实使用中如有偏差再调 `promote_margin`
