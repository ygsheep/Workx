/**
 * @file main.cpp
 * @brief workx 入口 — FTXUI 双栏 TUI
 * @details 复用 workx_agent 的会话装配（agent::create_session，B1 统一）：
 *          Backend + 全量工具集 + 系统提示词 + 会话持久化。
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <liblogger/logger.h>

#include "agent/api/i_backend_admin.h"
#include "agent/api/remote/http_client.h"  // models.dev 目录后台刷新
#include "agent/audit/audit_logger.h"
#include "agent/command/inclaude/registry.h"
#include "agent/config/app_config.h"
#include "agent/core/chat_session.h"
#include "agent/factory.h"
#include "agent/model/context_resolver.h"  // 上下文窗口解析（侧栏进度条分母）
#include "agent/model/model_catalog.h"
#include "agent/model/provider_preset.h"
#include "agent/session/session_store.h"
#include "core/config/config_manager.h"
#include "core/events/event_bus.h"
#include "core/task/task_manager.h"
#include "core/utils/file_index.h"

#include "island/balance_fetcher.h"
#include "island/cost_accumulator.h"
#include "island/island_event_bridge.h"
#include "island/island_server.h"
#include "island/pricing_table.h"
#include "island/registry_writer.h"

#include "app.h"
#include "crash_reporter.h"
#include "wizard.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

#if defined(_WIN32)
/// @brief Windows 端确保控制台以 UTF-8 输出/输入代码页（65001），使中文字符能正常加载显示。
///        FTXUI 在 Screen 构造时也会设置，这里在 FTXUI 首次输出前显式检测并确保：
///        若当前代码页不是 UTF-8（如 GBK/936），中文会乱码或显示为问号。
void ensure_console_utf8() {
    const UINT prev_out = ::GetConsoleOutputCP();
    const UINT prev_in = ::GetConsoleCP();
    if (prev_out != CP_UTF8) ::SetConsoleOutputCP(CP_UTF8);
    if (prev_in != CP_UTF8) ::SetConsoleCP(CP_UTF8);
    (void)prev_out; (void)prev_in;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    crash::InstallHandlers();
#if defined(_WIN32)
    // 检测/加载 UTF-8 中文字符集（见 ensure_console_utf8），放在向导/主界面
    // 创建任何 Screen 之前，确保早期中文输出也不走 GBK 转码。
    ensure_console_utf8();
#endif
    bool mock_mode = false;
    bool smoke_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mock") mock_mode = true;
        if (arg == "--smoke") smoke_mode = true;
        if (arg == "--version" || arg == "-v") {
            // 仅打印版本与简介后退出，不触发配置向导/索引/Island/TUI
            std::cout << "workx " << WORKX_VERSION;
#ifdef WORKX_BUILD_INFO
            std::cout << " (build " << WORKX_BUILD_INFO << ")";
#endif
            // NDEBUG 为 release 配置（CMAKE_BUILD_TYPE=Release/RelWithDebInfo/MSVC Release）
            // 统一设置的标准宏，未定义则为 debug 配置；跨 MSVC/GCC/Clang 可靠。
            // 注意 build 信息里的 "-dirty" 是 git 工作区脏标记，与构建类型无关。
#ifdef NDEBUG
            std::cout << " [release]\n";
#else
            std::cout << " [debug]\n";
#endif
            std::cout << "Workx — 一个现代化的终端 Code Agent / Work Agent\n"
                      << "用法:\n"
                      << "  workx                  启动 TUI\n"
                      << "  workx --version | -v  显示版本与简介\n";
            return 0;
        }
    }
    // 冒烟（B5）依赖 mock 流：无后端也能在 CI 无头管道下跑通全链路
    if (smoke_mode) mock_mode = true;
    namespace fs = std::filesystem;
    auto& cfg = agent::ConfigManager::instance();
    agent::register_config_defaults(cfg);

    // 载入默认配置文件（存在则读）
    auto config_path = agent::default_config_path();
    const bool first_run = !fs::exists(config_path);
    if (!first_run) agent::load_from_config_file(cfg, config_path);
    agent::load_from_env(cfg);

    // 首次运行（#66）：配置文件不存在时启动设置向导（mock 模式跳过）。
    // 向导直接写入 ConfigManager 内存并 save_to_file，无需重读。
    if (first_run && !mock_mode) {
        ftxtui::run_first_run_wizard(cfg, config_path);
    }

    // 日志（Debug/Release 统一到 ~/.workx/logs）
    // 级别 / 文件路径 / 保留天数全部由配置接管；删除过期历史日志后再打开文件。
    {
        // logging.level：字符串 → LogLevel（平缓回退到 info）
        const std::string level_str = cfg.get_or<std::string>(agent::keys::LOG_LEVEL, "info");
        agent::log::LogLevel level = agent::log::LogLevel::LOG_INFO;
        if (level_str == "trace") level = agent::log::LogLevel::LOG_TRACE;
        else if (level_str == "debug") level = agent::log::LogLevel::LOG_DEBUG;
        else if (level_str == "warn") level = agent::log::LogLevel::LOG_WARN;
        else if (level_str == "error") level = agent::log::LogLevel::LOG_ERROR;
        else if (level_str == "fatal") level = agent::log::LogLevel::LOG_FATAL;
        agent::log::Logger::get_instance().set_level(level);

        // logging.file：空 = 默认单一固定文件 workx.log（按大小轮转）；
        // 启动时清理旧版时间戳历史日志 workx_*.log
        std::string log_file = cfg.get_or<std::string>(agent::keys::LOG_FILE, "");
        size_t max_size_mb = static_cast<size_t>(cfg.get_or<int>(agent::keys::LOG_MAX_SIZE_MB, 10));
        size_t max_files = static_cast<size_t>(cfg.get_or<int>(agent::keys::LOG_MAX_FILES, 5));
        if (log_file.empty()) {
            agent::cleanup_expired_logs(cfg.get_or<int>(agent::keys::LOG_RETENTION_DAYS, 7));
            const auto& def = agent::default_log_path();
            if (!def.empty()) log_file = def.string();
        }
        if (!log_file.empty()) {
            auto& logger = agent::log::Logger::get_instance();
            logger.set_rotation(max_size_mb * 1024 * 1024, max_files);
            logger.enable_file_output(log_file, true);
        }

        // 审计日志（大小轮转 + 天数清理）：启用后记录工具调用与安全事件
        if (cfg.get_or<bool>(agent::keys::AUDIT_ENABLED, true)) {
            std::string audit_file = cfg.get_or<std::string>(agent::keys::AUDIT_FILE, "");
            if (audit_file.empty()) audit_file = (agent::log_dir() / "workx_audit.jsonl").string();
            agent::audit::AuditLogger::instance().init(
                audit_file,
                static_cast<size_t>(cfg.get_or<int>(agent::keys::AUDIT_MAX_SIZE_MB, 10)),
                static_cast<size_t>(cfg.get_or<int>(agent::keys::AUDIT_RETENTION_DAYS, 30)));
        } else {
            agent::audit::AuditLogger::instance().set_enabled(false);
        }
    }

    auto& bus = agent::EventBus::instance();
    auto& tm = agent::TaskManager::instance();

    std::string model_name;
    std::string session_dir;
    std::unique_ptr<agent::ChatSession> session;
    agent::IBackendAdmin* backend_admin = nullptr;
    std::shared_ptr<agent::mcp::McpClientManager> mcp_manager;  // #27 M4：MCP server 状态
    auto command_registry = std::make_shared<agent::command::CommandRegistry>();
    // Island 灵动岛 IPC：GUI（WhaleDock）通过 named pipe/socket 发现并连接本 TUI
    std::unique_ptr<island::RegistryWriter> island_registry;
    std::unique_ptr<island::PricingTable> island_pricing;
    std::unique_ptr<island::IslandServer> island_server;
    std::unique_ptr<island::CostAccumulator> island_cost;
    std::unique_ptr<island::BalanceFetcher> island_balance;
    std::unique_ptr<island::IslandEventBridge> island_bridge;
    // 上下文窗口（token）：启动时经 resolve_context_length 解析，注入侧栏进度条分母
    int32_t context_limit = 0;
    // models.dev 目录：堆上原子指针，后台 detach 线程按值捕获，前台 load() 并发安全
    auto model_catalog = std::make_shared<std::atomic<std::shared_ptr<const agent::ModelCatalog>>>();

    if (!mock_mode) {
        // B1：与 workx 主程序共用同一会话装配（全量工具集 + 系统提示词 + 持久化）
        const std::string provider = cfg.get_or<std::string>(agent::keys::PROVIDER, "");
        const agent::ProviderPreset* preset = provider.empty() ? nullptr
                                                               : agent::find_preset(provider);
        auto result = agent::create_session(cfg, preset, tm, bus);
        session = std::move(result.session);
        model_name = result.model_name;
        backend_admin = result.backend_admin;
        mcp_manager = std::move(result.mcp_manager);  // #27 M4：MCP server 状态（侧栏展示）

        // 会话持久化目录（/resume 列出历史用）
        if (session) {
            auto config_dir = agent::default_config_path().parent_path();
            session_dir = agent::session::get_project_session_dir(
                config_dir, fs::current_path().string()).string();
        }

        // 上下文窗口：统一通过 resolver 解析（provider→user cfg→catalog→capability→preset→default）
        // 启动初始化时无 selector 返回值，sel_context_length 传 0；对齐 src/app/main.cpp
        auto catalog_cache_path = agent::default_config_path().parent_path() / "models_cache.json";
        if (auto cached = agent::ModelCatalog::load_cache(catalog_cache_path); cached.is_ok()) {
            model_catalog->store(std::make_shared<const agent::ModelCatalog>(std::move(cached.value())));
        }
        // 后台线程拉取（不阻塞启动）；24h 内已拉取过则跳过（离线命中）
        // detach 安全性：model_catalog（shared_ptr）按值捕获保证生命周期；
        // catalog_cache_path 按值捕获（静态路径 ~/.config/workx/），不依赖栈帧。
        // try-catch 防止网络/文件异常未捕获导致 std::terminate。
        std::thread catalog_refresh_thread([model_catalog, catalog_cache_path]() {
            try {
                constexpr auto kCacheTtl = std::chrono::hours(24);
                std::error_code ec;
                auto mtime = std::filesystem::last_write_time(catalog_cache_path, ec);
                if (!ec && std::filesystem::file_time_type::clock::now() - mtime < kCacheTtl) return;
                agent::HttpClient http;
                auto resp = http.get("https://models.dev/api.json", {}, /*timeout_ms=*/30000);
                if (resp.is_err() || !resp.value().is_success()) return;
                auto parsed = agent::ModelCatalog::from_api_json(resp.value().body);
                if (parsed.is_err()) return;
                parsed.value().save_cache(catalog_cache_path);
                model_catalog->store(std::make_shared<const agent::ModelCatalog>(std::move(parsed.value())));
            } catch (const std::exception&) {
                // 后台刷新失败不影响启动；下次启动会重试
            }
        });
        catalog_refresh_thread.detach();

        auto resolution = agent::resolve_context_length(
            model_name,
            /*sel_context_length=*/0,
            cfg.get_or<int>(agent::keys::CONTEXT_LENGTH, 0),
            preset,
            model_catalog->load());
        context_limit = resolution.value;
    }

    // ---- 文件索引异步构建（@ 补全面板数据源）----
    // 后台线程扫描工作目录，不阻塞 TUI 出现；索引未就绪时 @ 面板返回空，
    // 就绪后自动显示文件列表。从用户主目录启动时跳过，避免扫描海量文件卡顿。
    {
        std::string cwd = fs::current_path().string();
        bool is_home_dir = false;
        const char* home_envs[] = {
#ifdef _WIN32
            "USERPROFILE", "APPDATA"
#else
            "HOME"
#endif
        };
        for (const char* env : home_envs) {
            if (const char* home = getenv(env)) {
                std::error_code ec;
                if (fs::equivalent(cwd, home, ec)) {
                    is_home_dir = true;
                    break;
                }
            }
        }
        if (!is_home_dir) {
            agent::global_file_index().build_async(cwd);  // 后台线程构建，不阻塞启动
        }
    }

    // ---- Island 灵动岛 IPC 服务端（GUI 发现本 TUI 并订阅事件流）----
    // 受 island.enabled 控制（默认 true）；mock 模式（CI 冒烟）不启动，避免测试副作用
    if (!mock_mode && cfg.get_or<bool>(agent::keys::ISLAND_ENABLED, true)) {
        island_registry = std::make_unique<island::RegistryWriter>(
            island::RegistryWriter::default_registry_path());

        // 单价表：DeepSeek 官方定价 fallback + 用户 ~/.workx/pricing.json 覆盖
        island_pricing = std::make_unique<island::PricingTable>(
            island::PricingTable::deepseek_default());
        {
            const auto pricing_path =
                agent::default_config_path().parent_path() / "pricing.json";
            std::error_code ec;
            if (std::filesystem::exists(pricing_path, ec)) {
                std::ifstream ifs(pricing_path);
                if (ifs) {
                    std::string text((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                    if (auto loaded = island::PricingTable::load_from_json(text);
                        loaded.is_ok()) {
                        *island_pricing = std::move(loaded.value());
                    }
                }
            }
        }

        island::IslandServerConfig isc;
        isc.project_root = fs::current_path().string();
        isc.model = model_name;
        island_server = std::make_unique<island::IslandServer>(
            std::move(isc), nullptr, island_registry.get());

        // 余额拉取（DeepSeek /user/balance）：api_key 非空才启用低频定时拉取
        const std::string api_key = cfg.get_or<std::string>(agent::keys::API_KEY, "");
        if (!api_key.empty()) {
            std::string base_url = cfg.get_or<std::string>(agent::keys::REMOTE_URL, "");
            if (base_url.empty()) base_url = "https://api.deepseek.com";
            island_balance = std::make_unique<island::BalanceFetcher>(
                bus, api_key, base_url,
                cfg.get_or<double>(agent::keys::ISLAND_USD_CNY_RATE, 7.2));
            island_balance->start();
        }

        // 费用累积（订阅 StreamDone/UserInput/AgentDone → CostUpdatedEvent）
        island_cost = std::make_unique<island::CostAccumulator>(
            bus, *island_pricing, model_name);
        if (island_balance) {
            island_cost->set_on_task_completed(
                [b = island_balance.get()] { b->trigger_refresh(); });
        }

        // 事件桥：EventBus → JSONL 推送给 GUI
        island_bridge = std::make_unique<island::IslandEventBridge>(bus, *island_server);

        // 请求处理：refresh_balance / get_model_pricing / get_session_summary
        island_server->set_request_handler(
            [b = island_balance.get(), p = island_pricing.get(), s = session.get()](
                const std::string& type, const nlohmann::json&) -> nlohmann::json {
                if (type == "refresh_balance") {
                    if (!b) return nlohmann::json(nullptr);
                    const auto r = b->refresh_and_wait(std::chrono::milliseconds(3000));
                    return nlohmann::json{{"success", r.success},
                                          {"balance_usd", r.balance_usd},
                                          {"cny_balance", r.cny_balance},
                                          {"fetched_at", r.fetched_at},
                                          {"error", r.error},
                                          {"source", r.source}};
                }
                if (type == "get_model_pricing") {
                    if (!p) return nlohmann::json(nullptr);
                    return p->to_json();
                }
                if (type == "get_session_summary") {
                    nlohmann::json j{{"session_id", s ? s->session_id() : ""}};
                    if (s) j["message_count"] = static_cast<int>(s->get_messages().size());
                    return j;
                }
                return nlohmann::json(nullptr);
            });

        island_server->start();
    }

    ftxtui::AppDeps deps;
    deps.session = session.get();
    deps.backend_admin = backend_admin;
    deps.event_bus = &bus;
    deps.config_manager = &cfg;
    deps.task_manager = &tm;
    deps.mock_mode = mock_mode;
    deps.smoke_mode = smoke_mode;
    deps.model_name = model_name;
    deps.session_dir = session_dir;
    deps.context_limit = context_limit;
    deps.model_catalog = model_catalog;
    deps.command_registry = command_registry;
    deps.mcp_manager = mcp_manager;  // #27 M4：MCP server 状态（侧栏展示）
    deps.project = fs::current_path().filename().string();
    // B3：侧栏 Agent 显示真实会话 ID（非硬编码 "default"），
    //     与审计日志 / 事件流的 session_id 一致，便于对照
    deps.agent_name = (session && !session->session_id().empty())
                          ? session->session_id()
                          : "default";
    deps.on_submit = [&](const std::string& text, const std::vector<std::string>& images) {
        if (session) session->send_message(text, images);
    };
    // /provider 热切换：以目标供应商条目自身配置为准创建后端，
    // 自定义条目（非 preset）也能正确使用其 base_url/api_key，避免依赖旧全局 cfg
    deps.create_provider = [&cfg, &bus](const agent::ProviderConfigEntry& entry) {
        return agent::create_backend_for_entry(cfg, entry, bus);
    };
    // /provider 热切换落盘：apply_provider_switch 只改内存，这里 save 到磁盘，
    // 否则重启会读取旧配置还原为上一供应商
    deps.save_config = [&cfg, &config_path] { (void)cfg.save_to_file(config_path); };

    ftxtui::App app(std::move(deps));
    app.run();
    const int exit_code = app.exit_code();

    // 清理
    tm.cancelAll();
    tm.waitForAll();
    // Island 清理：先停 server（断开 GUI 连接、移除注册记录），再退订事件桥/费用/余额
    if (island_server) island_server->stop();
    island_bridge.reset();
    island_cost.reset();
    island_balance.reset();
    island_server.reset();
    island_pricing.reset();
    island_registry.reset();
    if (session) {
        auto store = session->session_store();
        if (store) store->close();
    }
    session.reset();
    bus.clear();
    return exit_code;
}