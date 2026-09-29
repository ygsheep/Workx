/**
 * @file wizard.cpp
 * @brief 首次运行设置向导实现（自绘组件，独立全屏界面）
 */

#include "wizard.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "agent/config/app_config.h"
#include "agent/model/provider_preset.h"
#include "core/config/config_manager.h"
#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

namespace {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;
using ftxui::Event;

/// @brief 自绘首次运行向导组件
class FirstRunWizard : public ftxui::ComponentBase {
   public:
    FirstRunWizard(agent::ConfigManager& cfg, std::filesystem::path config_path,
                   ftxui::ScreenInteractive& screen, bool& completed)
        : m_cfg(cfg),
          m_config_path(std::move(config_path)),
          m_screen(screen),
          m_completed(completed) {
        for (const auto name : agent::list_preset_names()) {
            if (const auto* p = agent::find_preset(name)) {
                m_names.push_back(std::string(p->name));
                m_labels.push_back(p->display_name);
                m_models.push_back(p->default_model);
                m_urls.push_back(p->default_url);
                m_contexts.push_back(p->default_context_length);
            }
        }
        if (!m_names.empty()) m_context_len = std::to_string(m_contexts[0]);
    }

    bool OnEvent(Event event) override {
        if (m_finished) return true;
        if (event == Event::Escape) {
            m_finished = true;
            m_screen.Exit();
            return true;
        }

        switch (m_step) {
            case 0:  // 选择服务提供商
                if (event == Event::ArrowUp) {
                    m_sel = (m_sel + static_cast<int>(m_names.size()) - 1) %
                            static_cast<int>(m_names.size());
                    on_provider_changed();
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Tab) {
                    m_sel = (m_sel + 1) % static_cast<int>(m_names.size());
                    on_provider_changed();
                    return true;
                }
                if (event == Event::Return) {
                    m_step = 1;
                    m_field = 0;
                    return true;
                }
                break;
            case 1:  // API Key（自定义 URL 预设额外有 URL 字段）
                if (event == Event::Tab || event == Event::Return) {
                    if (next_field()) return true;
                    m_step = 2;
                    return true;
                }
                if (event == Event::TabReverse) {
                    m_step = 0;
                    return true;
                }
                if (event == Event::Backspace) {
                    edit_backspace(current_field());
                    return true;
                }
                if (event.is_character()) {
                    edit_char(current_field(), event.character());
                    return true;
                }
                break;
            case 2:  // 上下文长度 + 完成
                if (event == Event::Tab || event == Event::Return) {
                    finish();
                    return true;
                }
                if (event == Event::TabReverse) {
                    m_step = 1;
                    m_field = 0;
                    return true;
                }
                if (event == Event::Backspace) {
                    edit_backspace(m_context_len);
                    return true;
                }
                if (event.is_character()) {
                    edit_char(m_context_len, event.character());
                    return true;
                }
                break;
        }
        return true;
    }

    Element OnRender() override {
        using namespace ftxui;
        Elements body;
        body.push_back(text(std::string(str::kWizardTitle)) | color(theme::T::Accent) | bold);
        body.push_back(separatorEmpty());

        // 步骤指示
        const std::string step_label = step_title();
        body.push_back(hbox({
            text(std::string(str::kWizardStepPrefix)) | color(theme::T::TextDim),
            text(std::to_string(m_step + 1)) | color(theme::T::Accent) | bold,
            text(std::string(str::kWizardStepSep)) | color(theme::T::TextDim),
            text("3") | color(theme::T::TextDim),
            text("  ·  ") | color(theme::T::TextFaint),
            text(step_label) | color(theme::T::TextDim),
        }));
        body.push_back(separatorEmpty());

        switch (m_step) {
            case 0:
                body.push_back(render_provider_step());
                break;
            case 1:
                body.push_back(render_api_key_step());
                break;
            case 2:
                body.push_back(render_context_step());
                break;
        }
        body.push_back(separatorEmpty());
        body.push_back(text(std::string(str::kWizardHint)) | color(theme::T::TextFaint));

        auto content = vbox(std::move(body));
        const int term_w = ftxui::Terminal::Size().dimx;
        const int panel_w = std::max(48, term_w * 70 / 100);
        auto panel = vbox({
                         text(" "),
                         hbox({text("  "), content | flex, text("  ")}),
                         text(" "),
                     }) |
                     size(WIDTH, EQUAL, panel_w) | bgcolor(theme::T::Panel) | border;
        return ftxui::clear_under(panel);
    }

   private:
    std::string step_title() const {
        switch (m_step) {
            case 0:
                return std::string(str::kWizardStepProvider);
            case 1:
                return std::string(str::kWizardStepApiKey);
            default:
                return std::string(str::kWizardStepContext);
        }
    }

    bool is_custom() const {
        return m_sel >= 0 && static_cast<size_t>(m_sel) < m_names.size() &&
               m_names[static_cast<size_t>(m_sel)] == "openai-compatible";
    }

    /// @brief 当前可编辑字段（step 1：自定义预设为 URL→API Key 两个字段）
    std::string& current_field() {
        if (m_field == 0 && is_custom()) return m_custom_url;
        return m_api_key;
    }

    /// @brief 前进到下一个字段；返回 false 表示已到本步最后一个字段
    bool next_field() {
        if (is_custom() && m_field == 0) {
            m_field = 1;
            return true;
        }
        return false;
    }

    static void edit_char(std::string& field, const std::string& ch) {
        if (ch.size() == 1 && static_cast<uint8_t>(ch[0]) < 0x20) return;
        field += ch;
    }

    static void edit_backspace(std::string& field) {
        if (field.empty()) return;
        // UTF-8 安全退格
        size_t i = field.size();
        while (i > 0) {
            --i;
            if ((static_cast<unsigned char>(field[i]) & 0xC0) != 0x80) break;
        }
        field.erase(i);
    }

    void on_provider_changed() {
        if (m_sel >= 0 && static_cast<size_t>(m_sel) < m_contexts.size())
            m_context_len = std::to_string(m_contexts[static_cast<size_t>(m_sel)]);
    }

    Element render_provider_step() const {
        using namespace ftxui;
        Elements rows;
        rows.push_back(text(std::string(str::kWizardWelcome)) | color(theme::T::Text));
        rows.push_back(separatorEmpty());
        rows.push_back(text(std::string(str::kWizardProviderLabel)) | color(theme::T::TextDim));
        rows.push_back(separatorEmpty());
        for (size_t i = 0; i < m_labels.size(); ++i) {
            const bool sel = (static_cast<int>(i) == m_sel);
            std::string marker = sel ? "  ❯ " : "    ";
            auto row = hbox({
                text(marker) | color(sel ? theme::T::Accent : Color::Default),
                text(m_labels[i]),
                flex(text("")),
                text(m_models[i].empty() ? "" : "  " + m_models[i]) | color(theme::T::TextFaint),
                text(" "),
            });
            if (sel) row = row | bgcolor(theme::T::Selection) | bold;
            rows.push_back(row);
        }
        return vbox(std::move(rows));
    }

    Element render_field(const std::string& label, const std::string& value,
                         const std::string& hint, bool focused) const {
        using namespace ftxui;
        Element value_elem = value.empty()
                                 ? text(hint) | color(theme::T::TextFaint)
                                 : hbox({text(value), text("▎") | color(theme::T::Accent)});
        if (focused) value_elem = ftxui::focusCursorBar(std::move(value_elem));
        return vbox({
            text(label) | color(theme::T::TextDim),
            hbox({text("  "), value_elem | flex, text("  ")}) | bgcolor(theme::T::Surface) | border,
        });
    }

    Element render_api_key_step() const {
        using namespace ftxui;
        Elements rows;
        if (is_custom()) {
            rows.push_back(render_field(std::string(str::kWizardUrlLabel), m_custom_url,
                                        std::string(str::kWizardUrlHint), m_field == 0));
            rows.push_back(separatorEmpty());
        }
        rows.push_back(render_field(std::string(str::kWizardApiKeyLabel), m_api_key,
                                    std::string(str::kWizardApiKeyHint),
                                    m_field != 0 || !is_custom()));
        return vbox(std::move(rows));
    }

    Element render_context_step() const {
        using namespace ftxui;
        Elements rows;
        if (m_sel >= 0 && static_cast<size_t>(m_sel) < m_models.size() &&
            !m_models[static_cast<size_t>(m_sel)].empty()) {
            rows.push_back(hbox({
                text(std::string(str::kWizardModelPreview)) | color(theme::T::TextDim),
                text(m_models[static_cast<size_t>(m_sel)]) | color(theme::T::Text),
            }));
            rows.push_back(separatorEmpty());
        }
        rows.push_back(render_field(std::string(str::kWizardContextLabel), m_context_len,
                                    std::string(str::kWizardContextHint), true));
        rows.push_back(separatorEmpty());
        rows.push_back(hbox({
            text("  "),
            text(std::string(str::kWizardBtnFinish)) | color(theme::T::Accent) | bold,
            flex(text("")),
        }));
        return vbox(std::move(rows));
    }

    void finish() {
        if (m_sel < 0 || static_cast<size_t>(m_sel) >= m_names.size()) {
            m_finished = true;
            m_screen.Exit();
            return;
        }
        WizardConfig wc;
        wc.provider = m_names[static_cast<size_t>(m_sel)];
        wc.api_key = m_api_key;
        wc.custom_url = is_custom() ? m_custom_url : "";
        wc.context_len = m_context_len;
        apply_wizard_config(m_cfg, m_config_path, wc);
        m_completed = true;
        m_finished = true;
        m_screen.Exit();
    }

    agent::ConfigManager& m_cfg;
    std::filesystem::path m_config_path;
    ftxui::ScreenInteractive& m_screen;
    bool& m_completed;
    std::vector<std::string> m_names;   ///< 预设内部名
    std::vector<std::string> m_labels;  ///< 显示名
    std::vector<std::string> m_models;  ///< 默认模型
    std::vector<std::string> m_urls;    ///< 默认 URL
    std::vector<int> m_contexts;        ///< 默认上下文长度
    int m_step = 0;                     ///< 0=供应商 1=API Key 2=上下文
    int m_sel = 0;                      ///< 选中供应商下标
    int m_field = 0;                    ///< step1 字段（0=URL[自定义] 1=API Key）
    std::string m_api_key;
    std::string m_custom_url;
    std::string m_context_len;
    bool m_finished = false;
};

}  // namespace

bool apply_wizard_config(agent::ConfigManager& cfg, const std::filesystem::path& config_path,
                         const WizardConfig& wc) {
    const agent::ProviderPreset* preset = agent::find_preset(wc.provider);
    if (!preset) return false;

    cfg.set(agent::keys::PROVIDER, wc.provider);
    if (!wc.api_key.empty()) cfg.set(agent::keys::API_KEY, wc.api_key);
    if (!preset->default_model.empty()) cfg.set(agent::keys::MODEL_NAME, preset->default_model);
    if (wc.provider == "openai-compatible" && !wc.custom_url.empty())
        cfg.set(agent::keys::REMOTE_URL, wc.custom_url);
    int ctx = 0;
    try {
        ctx = std::stoi(wc.context_len);
    } catch (...) {
    }
    if (ctx > 0) cfg.set(agent::keys::CONTEXT_LENGTH, ctx);
    const auto result = cfg.save_to_file(config_path);
    return result.is_ok();
}

bool run_first_run_wizard(agent::ConfigManager& cfg, const std::filesystem::path& config_path) {
    auto screen = ftxui::ScreenInteractive::Fullscreen();
    bool completed = false;
    auto wizard = ftxui::Make<FirstRunWizard>(cfg, config_path, screen, completed);
    screen.Loop(wizard);
    return completed;
}

}  // namespace ftxtui
