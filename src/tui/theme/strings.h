/**
 * @file strings.h
 * @brief 用户可见文案集中层（A5 可访问性/本地化）
 * @details 所有面向用户的文本字面量集中于此，渲染层不再散落中文字符串。
 *          动态拼接（std::format / + 操作符）保留在调用处，字面量单一来源。
 */

#pragma once

#include <string_view>

namespace ftxtui::str {

// ----------------------------------------------------------------------------
// 通用 / 消息
// ----------------------------------------------------------------------------
inline constexpr std::string_view kErrorHeader      = "✖ 错误";
inline constexpr std::string_view kGaugeNA          = "n/a";
inline constexpr std::string_view kThinkingRunning  = " 思考中…";
inline constexpr std::string_view kThinkingLabel    = " 思考 ";
inline constexpr std::string_view kMsgCopy          = "复制";
inline constexpr std::string_view kMsgRetry         = "重试";

// ----------------------------------------------------------------------------
// 侧栏
// ----------------------------------------------------------------------------
inline constexpr std::string_view kSidebarNewSession = "新会话";
inline constexpr std::string_view kSidebarProject    = "项目";
inline constexpr std::string_view kSidebarBranch     = "分支";
inline constexpr std::string_view kSidebarContext    = "上下文";
inline constexpr std::string_view kSidebarCost       = "成本";
inline constexpr std::string_view kSidebarModel      = "模型";
inline constexpr std::string_view kSidebarCache      = "DS 缓存";
inline constexpr std::string_view kSidebarToken      = "Token";
inline constexpr std::string_view kSidebarPrompt     = "Prompt";
inline constexpr std::string_view kSidebarGenerated  = "生成";
inline constexpr std::string_view kSidebarHitRate    = "命中";
inline constexpr std::string_view kSidebarMCP        = "MCP";
inline constexpr std::string_view kSidebarTODO       = "TODO";
inline constexpr std::string_view kDash               = "—";
inline constexpr std::string_view kSlash              = " / ";
inline constexpr std::string_view kInfinity           = "∞";

// ----------------------------------------------------------------------------
// 侧边栏 tab（任务调度 | 项目 | 变更记录 | 文件）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kTabTasks           = "任务调度";
inline constexpr std::string_view kTabProjects        = "项目";
inline constexpr std::string_view kTabChanges         = "变更记录";
inline constexpr std::string_view kTabFiles           = "文件";
inline constexpr std::string_view kTabClose           = "✕";
inline constexpr std::string_view kTasksStatusBusy    = "● 生成中";
inline constexpr std::string_view kTasksStatusIdle    = "空闲";
inline constexpr std::string_view kTasksTool          = "当前工具";
inline constexpr std::string_view kTasksSteps         = "步骤";
inline constexpr std::string_view kTasksSubAgents     = "子 Agent";
inline constexpr std::string_view kTasksBackground    = "后台任务";
inline constexpr std::string_view kSubStatusDone      = "完成";
inline constexpr std::string_view kSubStatusFailed     = "失败";
inline constexpr std::string_view kSubStepLabel       = "步骤 ";
inline constexpr std::string_view kTabFilesEmpty      = "暂无打开的文件（/view <file>）";
inline constexpr std::string_view kTabChangesEmpty    = "暂无文件修改";

// ----------------------------------------------------------------------------
// 项目文件树 tab（常驻：文件列表 + git 状态点）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kProjectsLoading   = "扫描项目文件中…";
inline constexpr std::string_view kProjectsNoGit     = "未检测到 git 仓库";
inline constexpr std::string_view kProjectsEmpty     = "（项目为空）";
inline constexpr std::string_view kProjectsChanges   = " 处改动";
inline constexpr std::string_view kProjectsDirSuffix = "/";
inline constexpr std::string_view kProjectsHint      = "点击目录展开/收起 · ↑↓ 滚动 · 点击文件查看";

// ----------------------------------------------------------------------------
// 变更记录 tab（P5：修改点 Menu + hunk + 目的展开）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kChangeCountSuffix   = " 处修改";
inline constexpr std::string_view kChangeIcon          = "⚑ ";
inline constexpr std::string_view kChangeNoPurpose     = "（无目的）";
inline constexpr std::string_view kChangePurposeLabel  = "目的：";
inline constexpr std::string_view kChangeHint          = "↑↓ 选择 · e 展开 · Enter 跳转 · Esc 关闭";

// ----------------------------------------------------------------------------
// 文件查看器（/view 只读）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kCmdViewDesc       = "打开文件只读查看器";
inline constexpr std::string_view kViewUsage         = "用法：`/view <file>`\n";
inline constexpr std::string_view kViewNotFound      = "（文件不存在：";
inline constexpr std::string_view kViewTooLarge      = "（文件过大，已截断显示）\n";
inline constexpr std::string_view kViewLineSuffix    = " 行";
inline constexpr std::string_view kViewLangSep       = " · ";
inline constexpr std::string_view kViewScrollHint    = "↑↓ 滚动 · Esc 关闭";
inline constexpr std::string_view kViewDiffHint      = "diff 视图 · ↑↓ 滚动 · Esc 关闭";
inline constexpr std::string_view kViewImageHint     = "图片预览 · Esc 关闭";

// ----------------------------------------------------------------------------
// 内嵌 nvim 编辑（/edit 方案 B）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kCmdEditDesc      = "用 nvim 编辑文件";
inline constexpr std::string_view kCmdNvimDesc      = "启动 nvim（可带文件路径）";
inline constexpr std::string_view kEditUsage        = "用法：`/edit <file>`\n";
inline constexpr std::string_view kEditNoNvim       = "（未找到 nvim，请安装 Neovim 并加入 PATH）\n";
inline constexpr std::string_view kEditIsDir        = "（不能编辑目录：";
inline constexpr std::string_view kEditNewFile      = "（新建文件，nvim 保存后自动重读）\n";
inline constexpr std::string_view kEditSaved        = "（编辑完成，已重读文件）\n";
inline constexpr std::string_view kEditAborted      = "（编辑未正常保存，已按磁盘内容重读）\n";
inline constexpr std::string_view kEditFailed       = "（编辑器启动失败，未重读文件）\n";
inline constexpr std::string_view kEditChangePurpose = "手动编辑";
inline constexpr std::string_view kEditChangeReason  = "用户通过 /edit 命令手动修改";

// ----------------------------------------------------------------------------
// 状态行（模式置顶：标准 / 计划 / 极简 · 权限：手动审批 / 完全访问）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kStatusStandard   = "标准模式";
inline constexpr std::string_view kStatusPlan       = "计划模式";
inline constexpr std::string_view kStatusMinimal    = "极简模式";
inline constexpr std::string_view kStatusFullAccess = "完全访问";
inline constexpr std::string_view kStatusManual     = "手动审批";
inline constexpr std::string_view kStatusCtrlC      = "再次按 Ctrl+C 退出";

// ----------------------------------------------------------------------------
// 模式选择面板（Ctrl+P → 切换模式，与 /model 同款悬浮选择）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kPaletteModeTitle   = "切换模式";
inline constexpr std::string_view kModeStandardDesc   = "全部工具可用，权限独立切换";
inline constexpr std::string_view kModePlanDesc       = "只读规划：禁止写文件与执行命令";
inline constexpr std::string_view kModeMinimalDesc    = "仅支持 Skill / Bash / Read / Write / Edit";

// ----------------------------------------------------------------------------
// composer / 输入
// ----------------------------------------------------------------------------
inline constexpr std::string_view kComposerPlaceholder =
    "输入消息，Enter 发送 · Ctrl+P 全局搜索 · / 命令 · @ 文件";

// ----------------------------------------------------------------------------
// 聚合搜索面板（Ctrl+P）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kPaletteSearchHint =
    "搜索：会话 / 文件 / 设置…";
inline constexpr std::string_view kPaletteNoMatch     = "  无匹配结果";
inline constexpr std::string_view kPaletteMorePrefix  = "  ··· 还有 ";
inline constexpr std::string_view kPaletteMoreSuffix  = " 项";
inline constexpr std::string_view kPaletteHint =
    "↑↓/Tab 选择 · Enter 运行 · Esc 清除/关闭";
inline constexpr std::string_view kPaletteModelTitle   = "切换模型";
inline constexpr std::string_view kPaletteResumeTitle  = "恢复会话";
inline constexpr std::string_view kPaletteProviderTitle = "供应商管理";
inline constexpr std::string_view kCatFeature = "> 功能";
inline constexpr std::string_view kCatFile    = "@ 文件";
inline constexpr std::string_view kCatSession = "# 会话";
inline constexpr std::string_view kCatSetting = "⚙ 设置";
inline constexpr std::string_view kCatModel    = "◆ 模型";
inline constexpr std::string_view kCatProvider = "◈ 供应商";

// ----------------------------------------------------------------------------
// 输入栏提示面板（/ 命令 · @ 文件）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kSuggestNoCommand = "  无匹配命令";
inline constexpr std::string_view kSuggestNoFile    = "  无匹配文件";
inline constexpr std::string_view kSuggestIndexing  = "  索引构建中…";

// ----------------------------------------------------------------------------
// 设置条目（聚合搜索面板「设置」类）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kSettingPerm     = "切换权限模式";
inline constexpr std::string_view kSettingPermDesc = "手动审批 / 完全访问";
inline constexpr std::string_view kSettingMode     = "切换模式";
inline constexpr std::string_view kSettingModeDesc = "标准 / 计划 / 极简";
inline constexpr std::string_view kSettingModel    = "切换模型";
    inline constexpr std::string_view kSettingModelDesc = "打开模型选择器";
    inline constexpr std::string_view kSettingProvider    = "切换供应商";
inline constexpr std::string_view kSettingProviderDesc = "打开供应商切换面板";
inline constexpr std::string_view kSettingNewSession  = "新建会话";
inline constexpr std::string_view kSettingNewSessionDesc = "等价 /new";
inline constexpr std::string_view kSettingCompact     = "压缩上下文";
inline constexpr std::string_view kSettingCompactDesc = "手动触发上下文压缩";
inline constexpr std::string_view kSettingClear    = "清空会话";
inline constexpr std::string_view kSettingClearDesc = "等价 /clear";
inline constexpr std::string_view kSettingExit     = "退出";
inline constexpr std::string_view kSettingExitDesc = "等价 /exit";
inline constexpr std::string_view kSettingSidebar     = "切换侧边栏位置";
inline constexpr std::string_view kSettingSidebarDesc = "侧边栏居右 / 居左";

// ----------------------------------------------------------------------------
// AskUser 模态
// ----------------------------------------------------------------------------
inline constexpr std::string_view kAskTitleFallback  = "（请作答）";
inline constexpr std::string_view kAskProgressPrefix = "问题 ";
inline constexpr std::string_view kAskProgressSep    = "/";
inline constexpr std::string_view kAskIcon           = "❓ ";
inline constexpr std::string_view kAskCustomHint     = "输入答案（Enter 确认 · Esc 返回）";
inline constexpr std::string_view kAskChecked         = "☑ ";
inline constexpr std::string_view kAskUnchecked       = "☐ ";
inline constexpr std::string_view kAskCursor          = "❯ ";
inline constexpr std::string_view kAskCustomOption    = "✎ 自定义输入...";
inline constexpr std::string_view kAskMultiHint       =
    "  ↑↓ 移动 · 空格 勾选 · Enter 确认 · Esc 取消";
inline constexpr std::string_view kAskSingleHint      =
    "  ↑↓ 选择 · Enter 确认 · Esc 取消";
inline constexpr std::string_view kAskInputPlaceholder = "输入答案…";

// ----------------------------------------------------------------------------
// 消息队列卡片（模型忙碌时前端入队的用户消息）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kQueueIcon          = "⏸ ";
inline constexpr std::string_view kQueueTitlePrefix   = "条消息排队中";
inline constexpr std::string_view kQueueCtrlHint      = "Ctrl+Enter 立即发送";
inline constexpr std::string_view kQueueRemove        = " ✕";
inline constexpr std::string_view kQueueEmpty         = "（队列已空）";

// ----------------------------------------------------------------------------
// 命令 / 会话操作（App 提示与内置命令描述）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kNoBackendModels    = "（无后端，无法列出模型）\n";
inline constexpr std::string_view kModelListFailed    = "（模型列表获取失败）";
inline constexpr std::string_view kNoSessionBackend   = "（无会话后端，无法恢复历史会话）\n";
inline constexpr std::string_view kNoHistorySessions  = "（当前项目暂无历史会话）\n";
inline constexpr std::string_view kResumeHeader       = "历史会话（`/resume <编号>` 恢复）：\n";
inline constexpr std::string_view kMessageCountSuffix = " 条消息）";
inline constexpr std::string_view kMsgCountKeyword    = "条消息";
inline constexpr std::string_view kResumeBadIndex     = "（编号超出范围，`/resume` 查看列表）\n";
inline constexpr std::string_view kResumeFailedPrefix = "（会话切换失败：";
inline constexpr std::string_view kCloseParenNl       = "）\n";
inline constexpr std::string_view kProcessorUnavailable = "（命令处理器不可用）\n";
inline constexpr std::string_view kResumedPrefix      = "已恢复会话：**";
inline constexpr std::string_view kRenameUsage        = "用法：`/rename <新标题>`\n";
inline constexpr std::string_view kRenamedPrefix      = "会话标题已更新：**";
inline constexpr std::string_view kMdBoldEnd          = "**\n";
inline constexpr std::string_view kNoProviderConfig   = "（无配置管理，无法读取供应商列表）\n";
inline constexpr std::string_view kNoProvidersConfigured = "（未配置任何供应商，请先运行配置向导）\n";
inline constexpr std::string_view kProviderBusy       = "（正在生成中，无法切换供应商：";
inline constexpr std::string_view kProviderSwitchFailedPrefix = "（供应商切换失败：";
inline constexpr std::string_view kProviderSwitchedPrefix = "已切换供应商：**";

// ---- 压缩上下文（搜索面板「压缩上下文」 / /compact）----
inline constexpr std::string_view kCompactBusy       = "（正在生成中，暂无法压缩上下文）\n";
inline constexpr std::string_view kCompactNoNeed     = "（上下文无需压缩）\n";
inline constexpr std::string_view kCompactSoft       = "（上下文已达软水位，暂未折叠）\n";
inline constexpr std::string_view kCompactStuck      = "（压缩已触发卡死保护，暂被暂停）\n";
inline constexpr std::string_view kCompactDonePrefix = "上下文已压缩：**";
inline constexpr std::string_view kCompactTokensArrow = " → ";
inline constexpr std::string_view kCompactTokensSuffix = " tokens**\n";

inline constexpr std::string_view kCmdHelpDesc     = "显示可用命令列表";
inline constexpr std::string_view kHelpIntro        = "可用命令：\n";
inline constexpr std::string_view kCmdExitDesc     = "退出程序";
inline constexpr std::string_view kCmdQuitDesc     = "退出程序（别名）";
inline constexpr std::string_view kCmdClearDesc    = "删除当前会话并新建";
inline constexpr std::string_view kCmdNewDesc      = "新建会话并切换";
inline constexpr std::string_view kCmdCompactDesc  = "压缩上下文";
inline constexpr std::string_view kCmdModelDesc    = "切换模型";
inline constexpr std::string_view kCmdProviderDesc = "切换供应商";
inline constexpr std::string_view kCmdResumeDesc   = "恢复历史会话";
inline constexpr std::string_view kCmdRenameDesc   = "重命名会话";
inline constexpr std::string_view kCmdTestAskUserDesc = "测试 AskUser 提问弹窗（/Test: 测试命令）";

// ---- AskUser 测试命令回显 ----
inline constexpr std::string_view kTestAskUserPrefix   = "[Test:askuser] 返回值\n";
inline constexpr std::string_view kTestAskUserCancelled = "[Test:askuser] 已取消/超时";

// ----------------------------------------------------------------------------
// 事件回显（ViewModel prompt_echo）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kCachePrefixChanged =
    "[cache] 前缀变化 (";
inline constexpr std::string_view kCacheMissSep    = ") | 未命中 ";
inline constexpr std::string_view kCacheTokensUnit = " tokens";
inline constexpr std::string_view kCompactPausedPrefix =
    "[compact] 压缩已暂停（卡死守卫触发，连续 ";
inline constexpr std::string_view kCompactPausedSuffix = " 次）";
inline constexpr std::string_view kCompactResumed =
    "[compact] 压缩已恢复（占用比回落至 soft 阈值以下）";
inline constexpr std::string_view kSubAgentPrefix   = "[subagent ";
inline constexpr std::string_view kSubAgentSep      = "] ";
inline constexpr std::string_view kSubStepPrefix    = "step ";
inline constexpr std::string_view kSubStepOpen      = " (";
inline constexpr std::string_view kSubStepClose     = ")";
inline constexpr std::string_view kSubFailed        = "✗ 子任务失败";
inline constexpr std::string_view kSubCompleted     = "✓ 子任务完成";
inline constexpr std::string_view kSubDuration      = " ({:.1f}s)";

// ----------------------------------------------------------------------------
// 首次运行设置向导（#66）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kWizardTitle        = "首次运行设置向导";
inline constexpr std::string_view kWizardStepPrefix   = "步骤 ";
inline constexpr std::string_view kWizardStepSep      = " / ";
inline constexpr std::string_view kWizardStepProvider = "选择服务提供商";
inline constexpr std::string_view kWizardStepApiKey   = "填写 API Key";
inline constexpr std::string_view kWizardStepContext  = "确认上下文长度";
inline constexpr std::string_view kWizardWelcome      = "欢迎使用 Workx！首次运行需要配置 LLM 服务。";
inline constexpr std::string_view kWizardProviderLabel = "服务提供商：";
inline constexpr std::string_view kWizardApiKeyLabel   = "API Key：";
inline constexpr std::string_view kWizardApiKeyHint    = "粘贴 API Key（留空可稍后在 /provider 中设置）";
inline constexpr std::string_view kWizardUrlLabel      = "API 地址：";
inline constexpr std::string_view kWizardUrlHint       = "自定义 URL 的完整 API 端点";
inline constexpr std::string_view kWizardContextLabel  = "上下文长度（token）：";
inline constexpr std::string_view kWizardContextHint   = "留空使用预设默认值";
inline constexpr std::string_view kWizardModelPreview  = "默认模型：";
inline constexpr std::string_view kWizardBtnNext       = "下一步";
inline constexpr std::string_view kWizardBtnFinish     = "完成";
inline constexpr std::string_view kWizardBtnSkip       = "跳过";
inline constexpr std::string_view kWizardHint          = "↑↓ 选择 · Enter 下一步 · Shift+Tab 上一步 · Esc 跳过";
inline constexpr std::string_view kWizardDone          = "配置已保存，即将启动 Workx…";
inline constexpr std::string_view kWizardSkipped       = "已跳过配置，可稍后使用 /provider 设置";

// ----------------------------------------------------------------------------
// 输出区域层级（标题栏下子列表导航）
// ----------------------------------------------------------------------------
inline constexpr std::string_view kOutputMain        = "主会话";
inline constexpr std::string_view kOutputSubAgent    = "子 Agent";
inline constexpr std::string_view kOutputSep         = " > ";
inline constexpr std::string_view kOutputHint        = "点击返回主会话";
inline constexpr std::string_view kSubHeader         = "子 Agent 记录";
inline constexpr std::string_view kSubStepThought    = "思考";
inline constexpr std::string_view kSubStepAction     = "工具";
inline constexpr std::string_view kSubStepObservation = "观察";
inline constexpr std::string_view kSubStepFinal      = "最终";
inline constexpr std::string_view kSubStatusRunning  = "运行中";
inline constexpr std::string_view kSubFinalAnswer    = "最终答复";
inline constexpr std::string_view kSubNoRecord       = "（无子 Agent 记录）";
inline constexpr std::string_view kSubBackToMain     = "返回主会话";

}  // namespace ftxtui::str