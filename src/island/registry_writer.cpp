/**
 * @file registry_writer.cpp
 * @brief 注册文件读写实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "island/registry_writer.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace island {

namespace {

constexpr const char* kSessionsKey = "sessions";

agent::Error io_error(const std::filesystem::path& path, const std::string& what) {
    return agent::Error(agent::Error::Code::InternalError, "registry 文件操作失败: " + what,
                        path.string());
}

/// @brief 原子写：临时文件 + rename（失败时清理临时文件）
agent::ResultV2<void> atomic_write(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    const auto dir = path.parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec) return agent::ResultV2<void>::err(io_error(path, ec.message()));
    }

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return agent::ResultV2<void>::err(io_error(path, "无法打开临时文件"));
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs.good()) return agent::ResultV2<void>::err(io_error(path, "写入临时文件失败"));
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return agent::ResultV2<void>::err(io_error(path, ec.message()));
    }
    return agent::ResultV2<void>::ok();
}

}  // namespace

RegistryWriter::RegistryWriter(std::filesystem::path path) : m_path(std::move(path)) {}

agent::ResultV2<void> RegistryWriter::write(const RegistryEntry& entry) {
    auto existing = read_all(m_path);
    std::vector<RegistryEntry> entries =
        existing.is_ok() ? std::move(existing.value()) : std::vector<RegistryEntry>{};
    // 替换同 pid 记录，保留其他会话（多 TUI 共存）
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const RegistryEntry& e) { return e.pid == entry.pid; }),
                  entries.end());
    entries.push_back(entry);

    const std::string content = to_json(entries).dump();
    return atomic_write(m_path, content);
}

agent::ResultV2<void> RegistryWriter::remove(uint32_t pid) {
    auto existing = read_all(m_path);
    if (existing.is_err()) return agent::ResultV2<void>::ok();
    std::vector<RegistryEntry> entries = std::move(existing.value());
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const RegistryEntry& e) { return e.pid == pid; }),
                  entries.end());
    return atomic_write(m_path, to_json(entries).dump());
}

agent::ResultV2<std::vector<RegistryEntry>> RegistryWriter::read_all(
    const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return agent::ResultV2<std::vector<RegistryEntry>>::ok({});
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return agent::ResultV2<std::vector<RegistryEntry>>::err(io_error(path, "无法打开"));
    std::ostringstream oss;
    oss << ifs.rdbuf();
    if (ifs.bad())
        return agent::ResultV2<std::vector<RegistryEntry>>::err(io_error(path, "读取失败"));
    return parse(oss.str());
}

std::filesystem::path RegistryWriter::default_registry_path() {
    std::filesystem::path home;
    const char* home_env = std::getenv("USERPROFILE");
#ifdef _WIN32
    if (!home_env) home_env = std::getenv("HOMEDRIVE");  // 兜底（不处理 HOMEPATH 拼接，罕见）
#else
    home_env = std::getenv("HOME");
#endif
    if (home_env && *home_env) {
        home = home_env;
    } else {
        home = std::filesystem::current_path();
    }
    return home / ".workx" / "island.registry";
}

nlohmann::json RegistryWriter::to_json(const std::vector<RegistryEntry>& entries) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries) {
        arr.push_back({
            {"pid", e.pid},
            {"endpoint", e.endpoint},
            {"project_root", e.project_root},
            {"started_at", e.started_at},
            {"model", e.model},
            {"last_heartbeat", e.last_heartbeat},
        });
    }
    return nlohmann::json{{kSessionsKey, std::move(arr)}};
}

agent::ResultV2<std::vector<RegistryEntry>> RegistryWriter::parse(const std::string& text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        return agent::ResultV2<std::vector<RegistryEntry>>::err(
            agent::Error(agent::Error::Code::ConfigParseFailed,
                         std::string("registry json 解析失败: ") + e.what()));
    }
    if (!j.is_object() || !j.contains(kSessionsKey) || !j[kSessionsKey].is_array()) {
        return agent::ResultV2<std::vector<RegistryEntry>>::err(agent::Error(
            agent::Error::Code::ConfigParseFailed, "registry 格式错误：缺少 sessions 数组"));
    }

    std::vector<RegistryEntry> entries;
    for (const auto& item : j[kSessionsKey]) {
        if (!item.is_object()) continue;
        RegistryEntry e;
        e.pid = item.value("pid", static_cast<uint32_t>(0));
        e.endpoint = item.value("endpoint", "");
        e.project_root = item.value("project_root", "");
        e.started_at = item.value("started_at", static_cast<int64_t>(0));
        e.model = item.value("model", "");
        e.last_heartbeat = item.value("last_heartbeat", static_cast<int64_t>(0));
        if (e.pid == 0 || e.endpoint.empty()) continue;  // 跳过损坏条目
        entries.push_back(std::move(e));
    }
    return agent::ResultV2<std::vector<RegistryEntry>>::ok(std::move(entries));
}

}  // namespace island