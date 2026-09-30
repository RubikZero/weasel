# lm_ranker 插件接口审计（M0，2026-09-15）

## 现状
- 插件骨架：`weasel/librime/plugins/lm_ranker/`（占位 Filter，仅按 quality 排序，注册名 `lm_ranker`）
- librime 扩展：`src/rime_api.h` + `src/rime_api_impl.h`（`RimeCandidate` 增加 quality/type，供抽取器使用）
- Weasel 源码：**未发现光标上下文修改**（`git status` 仅 librime 子模块被改动 + `include/afxres.h` 为本地新文件）

## 可用接口（librime 1.13.1，源码核对）
| 需求 | 接口 | 备注 |
|---|---|---|
| 候选后处理 | `Filter::Apply` + `engine_`（`ticket.engine`） | Filter 内可访问 `engine_->context()/schema()` |
| 会话上下文（已上屏） | `Context::commit_history()` | 最多 20 条 `CommitRecord`，可拼出会话内已提交文本（弱上下文） |
| 组合输入/拼音分段 | `Composition`/`Segmentation`（`Segment.start/end/prompt` + `input()`） | 可据此得到音节边界做约束解码 |
| UI 刷新 | `Context::update_notifier()` | Weasel IPC 为请求-响应：通知在**下一次请求**才被带回（`RimeWithWeasel.cpp:551/675`），无服务端→客户端异步推送 |
| 用户目录 | `RimeApi::get_user_data_dir_s` / `Deployer::user_data_dir` | 放模型与配置（`%AppData%\Rime`） |
| 配置读取 | yaml-cpp（librime 依赖，插件内可直接用） | 读 `%AppData%\Rime\lm_ranker.yaml` |

## 关键结论与风险
1. **光标上下文（应用内光标前文本）必须由 TSF 侧获取**（`ITfContext`/`ITfRange`），当前仓库无实现；建议实现链路：TSF 取上下文 → WeaselIPC 新消息 → `RimeWithWeasel` → librime 新 API（建议 `RimeSetSurroundingText(session_id, text)`）。
2. **异步置顶的可见性**：由于无服务端推送，置顶在"选择/提交"时会生效（用户停顿后按空格/数字即选中 MLM 句），但候选列表的视觉刷新通常要等下一次按键。若要即时刷新，需要在 Weasel IPC 增加服务端→客户端推送。
3. **集成点**：Filter 必须出现在 schema 的 `engine.filters` 中才生效；需为支持的 schema（如 luna_pinyin_simp）提供 custom 补丁或直接改随包共享 schema 数据。
4. 现有 `Context::GetTextBefore`/`commit_history` 只能覆盖"经本输入法提交的文本"，无法覆盖用户在其他场景已存在的光标前文本 → 与训练数据（真实文档上下文）分布有差距，上下文越短效果越接近字级重排。

## 下一步（M2）
- 插件：配置 + ORT 会话 + 上下文提供者（commit_history 兜底 + 预留 TSF 上下文 API）+ 字/词重排 + 异步整句（debounce 120ms，min 4 音节）+ smart 置顶（margin > 0.5）+ 线程/超时/取消，缺模型时静默禁用
- 依赖准备：ONNX Runtime C/C++（win-x64 zip、win-x86 NuGet、win-arm64 zip）
