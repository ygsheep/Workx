/**
 * @file provider_manager.cpp
 * @brief 供应商管理面板实现（两层 TUI 移植到 FTXUI）
 * @details 语义对齐 legacy/tui 分支的 src/app/ui/provider_form.cpp：
 *          - 列表层：Enter/空格/Tab = 设为使用中；Ctrl+N / a = 添加；
 *            e = 编辑；Ctrl+D / d = 删除（二次确认）；底部操作按钮行可点击。
 *          - 表单层：Enter 依次切换字段（最后字段 Enter = 保存）；
 *            ↑↓ 在名称建议上移动/否则切字段；Tab 补全名称；Ctrl+S 保存；
 *            Esc 放弃（新增回滚 / 编辑还原快照）。
 *          - 模型 ID 输入时经 ModelCatalog/能力表自动填充上下文窗口。
 */

#include "widgets/provider_manager.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/string.hpp>

#include "agent/model/config.h"
#include "agent/model/provider_preset.h"
#include "theme/icons.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;
using ftxui::Event;

namespace {

// 主题色（对应 theme/theme.h）
const Color kPanel = theme::T::Panel;
const Color kAccent = theme::T::Accent;
const Color kSelBg = theme::T::Selection;
const Color kText = theme::T::Text;
const Color kTextDim = theme::T::TextDim;
const Color kTextFaint = theme::T::TextFaint;
const Color kGreen = theme::T::DiffAdd;
const Color kRed = theme::T::DiffDel;

/// Custom URL 特殊预设内部名（无默认 URL/Model）
constexpr const char* kCustomProviderId = "openai-compatible";
/// 上下文窗口输入上限（与 backend.context_length 一致）
constexpr int64_t kMaxContext = 2000000;
/// 列表最多同时显示项数
constexpr int kMaxVisible = 8;
/// 名称建议最多显示项数
constexpr int kMaxSuggest = 5;

/// 表单字段
enum Field : int {
    F_NAME = 0,  // 供应商名称
    F_URL,       // Base URL
    F_MODEL,     // 模型 ID
    F_CONTEXT,   // 上下文窗口
    F_KEY,       // API Key
    F_COUNT
};

/// 建议条目（命令面板风格）
struct SuggestItem {
    std::string name;     // 预设内部名（如 deepseek；自定义为 openai-compatible）
    std::string display;  // 显示名（如 DeepSeek）
    std::string desc;     // 描述（默认 URL）
    bool is_custom = false;
};

const char* field_label(Field f) {
    switch (f) {
        case F_NAME:
            return "名称";
        case F_URL:
            return "Base URL";
        case F_MODEL:
            return "模型 ID";
        case F_CONTEXT:
            return "上下文窗口";
        case F_KEY:
            return "API Key";
        default:
            return "";
    }
}

std::string* field_value(agent::ProviderConfigEntry& e, Field f) {
    switch (f) {
        case F_NAME:
            return &e.name;
        case F_URL:
            return &e.base_url;
        case F_MODEL:
            return &e.model;
        case F_KEY:
            return &e.api_key;
        default:
            return nullptr;  // F_CONTEXT 无字符串存储
    }
}

/// 模型 ID → 上下文窗口自动匹配（catalog 优先，静态能力表兜底）
int32_t match_context_window(const agent::ModelCatalog* catalog, const std::string& model) {
    if (model.empty()) return 0;
    if (catalog) {
        int32_t v = catalog->context_window_for(model);
        if (v > 0) return v;
    }
    if (const auto* cap = agent::find_model_capability(model)) return cap->context_window;
    return 0;
}

/// 模型输入能力图标串（文本恒支持；图像取决于能力表 supports_vision）
std::string input_type_glyphs(const std::string& model) {
    std::string out(theme::icon_input_text());
    bool vision = false;
    if (!model.empty())
        if (const auto* cap = agent::find_model_capability(model)) vision = cap->supports_vision;
    if (vision) out += " " + std::string(theme::icon_input_vision());
    return out;
}

/// API Key 掩码显示
std::string mask_key(const std::string& key) {
    if (key.empty()) return "(空)";
    if (key.size() <= 8) return "****";
    return key.substr(0, 4) + "****";
}

/// 内置预设建议列表（自定义项标记 is_custom）
std::vector<SuggestItem> build_suggestions() {
    std::vector<SuggestItem> out;
    for (auto name : agent::list_preset_names()) {
        const agent::ProviderPreset* p = agent::find_preset(name);
        if (!p) continue;
        out.push_back(SuggestItem{
            .name = std::string(p->name),
            .display = p->display_name,
            .desc = p->default_url.empty() ? std::string(p->name) + " 协议" : p->default_url,
            .is_custom = (p->name == kCustomProviderId),
        });
    }
    return out;
}

/// 前缀过滤建议（不区分大小写；按 display / name 匹配，保持原顺序）
std::vector<SuggestItem> filter_suggestions(const std::vector<SuggestItem>& all,
                                            const std::string& prefix) {
    std::vector<SuggestItem> out;
    auto ic_startswith = [](const std::string& s, const std::string& p) {
        if (p.size() > s.size()) return false;
        for (size_t i = 0; i < p.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[i])) !=
                std::tolower(static_cast<unsigned char>(p[i])))
                return false;
        return true;
    };
    for (const auto& item : all)
        if (ic_startswith(item.display, prefix) || ic_startswith(item.name, prefix))
            out.push_back(item);
    return out;
}

/// 删除字符串末尾一个完整 UTF-8 字符
void pop_utf8_back(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.erase(i);
}

}  // namespace

class ProviderManager : public ftxui::ComponentBase {
   public:
    ProviderManager(ProviderManagerOptions&& opts, bool& open)
        : m_providers(opts.providers),
          m_active_id(opts.active_id),
          m_catalog(std::move(opts.catalog)),
          m_on_activate(std::move(opts.on_activate)),
          m_on_commit(std::move(opts.on_commit)),
          m_on_close(std::move(opts.on_close)),
          m_title(std::move(opts.title)),
          m_open(open),
          m_suggest_all(build_suggestions()) {}

    bool OnEvent(Event event) override {
        if (event == Event::Escape) {
            return on_escape();
        }

        if (m_layer == Layer::Form) {
            if (handle_form_event(event)) return true;
            if (event.is_character()) {
                handle_form_char(event.character());
                return true;
            }
            return false;
        }
        return handle_list_event(event);
    }

    Element OnRender() override {
        // 打开边沿：重置交互状态、就近定位当前使用中供应商
        if (m_open && !m_last_open) {
            reset_on_open();
        }
        m_last_open = m_open;
        if (!m_open) return ftxui::emptyElement();
        m_item_boxes.clear();
        m_item_rows.clear();
        m_btn_boxes.clear();

        Element content = (m_layer == Layer::Form) ? render_form() : render_list();

        const int term_w = ftxui::Terminal::Size().dimx;
        const int panel_w = std::max(48, term_w * 62 / 100);
        auto panel = ftxui::vbox({
                         ftxui::text(" "),
                         ftxui::hbox({ftxui::text("  "), content | ftxui::flex, ftxui::text("  ")}),
                         ftxui::text(" "),
                     }) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, panel_w) |
                     ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 26) | ftxui::bgcolor(kPanel) |
                     ftxui::border;
        return ftxui::clear_under(panel) | ftxui::reflect(m_box);
    }

   private:
    enum class Layer { List, Form };

    // ---------------- 状态 ----------------
    std::vector<agent::ProviderConfigEntry>& m_providers;
    std::string& m_active_id;
    std::shared_ptr<const agent::ModelCatalog> m_catalog;
    std::function<void(int)> m_on_activate;
    std::function<void()> m_on_commit;
    std::function<void()> m_on_close;
    std::string m_title;
    bool& m_open;
    bool m_last_open = false;
    std::vector<SuggestItem> m_suggest_all;
    std::vector<SuggestItem> m_suggest;  // 过滤后的名称建议
    int m_suggest_sel = 0;

    Layer m_layer = Layer::List;
    int m_sel = 0;               // 列表选中项（providers 下标）
    bool m_confirm_del = false;  // 删除二次确认
    std::string m_status;        // 状态提示行

    // 表单状态
    Field m_field = F_NAME;
    bool m_dirty = false;                   // 表单有未保存修改
    bool m_editing_new = false;             // 表单是否处于新增流程
    bool m_escape_armed = false;            // Esc 双击确认放弃
    bool m_context_auto = true;             // 上下文窗口是否由模型匹配自动填充
    agent::ProviderConfigEntry m_snapshot;  // 进入表单前快照（Esc 放弃时还原）

    // 渲染交互命中
    ftxui::Box m_box;
    std::deque<ftxui::Box> m_item_boxes;
    std::deque<int> m_item_rows;
    std::deque<ftxui::Box> m_btn_boxes;

    // ---------------- 逻辑 ----------------
    void reset_on_open() {
        m_layer = Layer::List;
        m_status.clear();
        m_confirm_del = false;
        m_sel = 0;
        if (!m_providers.empty())
            for (size_t i = 0; i < m_providers.size(); ++i)
                if (m_providers[i].id == m_active_id) {
                    m_sel = static_cast<int>(i);
                    break;
                }
    }

    bool on_escape() {
        if (m_layer == Layer::Form) {
            discard_form();
            return true;
        }
        if (m_confirm_del) {
            m_confirm_del = false;
            m_status.clear();
            return true;
        }
        m_open = false;
        if (m_on_close) m_on_close();
        return true;
    }

    void activate(int index) {
        if (index < 0 || index >= static_cast<int>(m_providers.size())) return;
        m_open = false;
        if (m_on_activate) m_on_activate(index);
    }

    // ---------------- 列表层 ----------------
    bool handle_list_event(Event event) {
        // 空格/回车/Tab = 设为使用中
        if (event == Event::Return || event == Event::Tab) {
            activate(m_sel);
            return true;
        }
        if (event.is_character() && event.character() == " ") {
            activate(m_sel);
            return true;
        }
        if (event == Event::ArrowUp) {
            move_sel(-1);
            return true;
        }
        if (event == Event::ArrowDown) {
            move_sel(+1);
            return true;
        }
        // a / Ctrl+N = 添加
        if (event == Event::CtrlN ||
            (event.is_character() && (event.character() == "a" || event.character() == "A"))) {
            begin_new();
            return true;
        }
        // e = 编辑
        if (event.is_character() && (event.character() == "e" || event.character() == "E")) {
            begin_edit();
            return true;
        }
        // d / Ctrl+D = 删除（二次确认）
        if (event == Event::CtrlD ||
            (event.is_character() && (event.character() == "d" || event.character() == "D"))) {
            if (m_confirm_del)
                do_delete();
            else {
                if (m_providers.empty()) {
                    m_status = "当前无供应商可删除";
                    return true;
                }
                m_confirm_del = true;
                m_status = "确认删除「" + m_providers[static_cast<size_t>(m_sel)].name +
                           "」？再按 d / 点击删除执行，Esc 取消";
            }
            return true;
        }
        if (event.is_mouse()) {
            const auto& m = event.mouse();
            if (m.button == ftxui::Mouse::Left && m.motion == ftxui::Mouse::Pressed) {
                // 操作按钮
                for (size_t k = 0; k < m_btn_boxes.size(); ++k)
                    if (m_btn_boxes[k].Contain(m.x, m.y)) {
                        on_btn(static_cast<Action>(k));
                        return true;
                    }
                // 列表项：仅选中（避免误触切换）
                for (size_t k = 0; k < m_item_boxes.size(); ++k)
                    if (m_item_boxes[k].Contain(m.x, m.y)) {
                        m_sel = m_item_rows[k];
                        m_status.clear();
                        return true;
                    }
                if (m_box.Contain(m.x, m.y)) return true;  // 面板内空白：消费
                return false;
            }
            if (m.button == ftxui::Mouse::WheelUp) {
                move_sel(-1);
                return true;
            }
            if (m.button == ftxui::Mouse::WheelDown) {
                move_sel(+1);
                return true;
            }
            return false;
        }
        return false;
    }

    void move_sel(int delta) {
        if (m_providers.empty()) return;
        const int n = static_cast<int>(m_providers.size());
        m_sel += delta;
        if (m_sel < 0) m_sel = n - 1;
        if (m_sel >= n) m_sel = 0;
    }

    enum class Action { Activate, Edit, Add, Delete };

    void on_btn(Action a) {
        switch (a) {
            case Action::Activate:
                activate(m_sel);
                break;
            case Action::Edit:
                begin_edit();
                break;
            case Action::Add:
                begin_new();
                break;
            case Action::Delete:
                if (m_providers.empty()) {
                    m_status = "当前无供应商可删除";
                    break;
                }
                if (m_confirm_del)
                    do_delete();
                else {
                    m_confirm_del = true;
                    m_status = "确认删除「" + m_providers[static_cast<size_t>(m_sel)].name +
                               "」？再按 d / 点击删除执行，Esc 取消";
                }
                break;
        }
    }

    void do_delete() {
        if (m_sel < 0 || m_sel >= static_cast<int>(m_providers.size())) {
            m_confirm_del = false;
            return;
        }
        const std::string name = m_providers[static_cast<size_t>(m_sel)].name;
        m_providers.erase(m_providers.begin() + m_sel);
        if (m_sel >= static_cast<int>(m_providers.size()))
            m_sel = static_cast<int>(m_providers.size()) - 1;
        m_confirm_del = false;
        if (m_on_commit) m_on_commit();
        m_status = "已删除供应商 " + name;
    }

    void begin_new() {
        std::string base = "new-provider-";
        for (int n = 1;; ++n) {
            std::string cand = base + std::to_string(n);
            bool taken = false;
            for (const auto& e : m_providers)
                if (e.id == cand) {
                    taken = true;
                    break;
                }
            if (!taken) {
                agent::ProviderConfigEntry ne;
                ne.id = std::move(cand);
                ne.name = "New Provider";
                m_providers.push_back(std::move(ne));
                m_sel = static_cast<int>(m_providers.size()) - 1;
                break;
            }
        }
        m_field = F_NAME;
        m_dirty = true;
        m_editing_new = true;
        m_context_auto = true;
        m_escape_armed = false;
        m_snapshot = {};
        refresh_suggest();
        m_layer = Layer::Form;
        m_status = "新增配置：输入名称（可参考下方提示）";
    }

    void begin_edit() {
        if (m_providers.empty()) {
            m_status = "当前无供应商可编辑";
            return;
        }
        m_snapshot = m_providers[static_cast<size_t>(m_sel)];
        m_field = F_NAME;
        m_dirty = false;
        m_editing_new = false;
        m_context_auto = true;
        m_escape_armed = false;
        refresh_suggest();
        m_layer = Layer::Form;
        m_status = "编辑配置：输入名称（可参考下方提示）";
    }

    void discard_form() {
        if (m_dirty && !m_escape_armed) {
            m_escape_armed = true;
            m_status = "有未保存修改，再按一次 Esc 放弃（不保存）";
            return;
        }
        if (m_editing_new) {
            m_providers.erase(m_providers.begin() + m_sel);
            if (m_sel >= static_cast<int>(m_providers.size()))
                m_sel = static_cast<int>(m_providers.size()) - 1;
        } else if (m_sel >= 0 && m_sel < static_cast<int>(m_providers.size())) {
            m_providers[static_cast<size_t>(m_sel)] = m_snapshot;
        }
        m_dirty = false;
        m_escape_armed = false;
        m_suggest.clear();
        m_layer = Layer::List;
        m_status = "已放弃修改，返回配置列表";
    }

    // ---------------- 表单层 ----------------
    bool handle_form_event(Event event) {
        // 表单回车：建议填充 / 下一字段 / 最后字段保存
        if (event == Event::Return) return form_enter();
        // ↑↓：建议内移动，否则切字段
        if (event == Event::ArrowUp) {
            form_move(-1);
            return true;
        }
        if (event == Event::ArrowDown) {
            form_move(+1);
            return true;
        }
        // Tab：补全名称 / 切换下一字段
        if (event == Event::Tab) {
            if (suggest_visible()) {
                fill_suggest(true);
                refresh_suggest();
                return true;
            }
            next_field();
            return true;
        }
        if (event == Event::CtrlS) {
            save_form();
            return true;
        }
        if (event == Event::Backspace || event == Event::Delete) {
            handle_backspace();
            return true;
        }
        if (event.is_mouse()) {
            const auto& m = event.mouse();
            if (m.button == ftxui::Mouse::WheelUp || m.button == ftxui::Mouse::WheelDown)
                if (m_box.Contain(m.x, m.y)) return true;  // 消费滚轮
        }
        return false;
    }

    void handle_form_char(const std::string& ch) {
        // 上下文窗口：仅接收数字
        if (m_field == F_CONTEXT) {
            if (ch.size() == 1 && std::isdigit(static_cast<unsigned char>(ch[0])))
                append_ctx_digit(ch[0] - '0');
            else
                m_status = "上下文窗口仅支持输入数字";
            return;
        }
        agent::ProviderConfigEntry& cur = m_providers[static_cast<size_t>(m_sel)];
        std::string* vp = field_value(cur, m_field);
        if (!vp || vp->size() >= 512) return;
        *vp += ch;
        m_dirty = true;
        m_escape_armed = false;
        after_field_edit(cur);
    }

    void after_field_edit(agent::ProviderConfigEntry& cur) {
        if (m_field == F_MODEL) {
            const int32_t auto_ctx = match_context_window(m_catalog.get(), cur.model);
            if (auto_ctx > 0) {
                cur.context_length = auto_ctx;
                m_context_auto = true;
            } else
                m_context_auto = false;
        } else if (m_field == F_NAME) {
            refresh_suggest();
        }
    }

    void append_ctx_digit(int d) {
        const int64_t next =
            static_cast<int64_t>(m_providers[static_cast<size_t>(m_sel)].context_length) * 10 + d;
        if (next > kMaxContext) {
            m_status = "上下文窗口已达上限 2000000";
            return;
        }
        if (m_providers[static_cast<size_t>(m_sel)].context_length == 0) m_context_auto = false;
        m_providers[static_cast<size_t>(m_sel)].context_length = static_cast<int32_t>(next);
        m_context_auto = false;
        m_dirty = true;
        m_escape_armed = false;
    }

    void handle_backspace() {
        if (m_field == F_CONTEXT) {
            int32_t d = m_providers[static_cast<size_t>(m_sel)].context_length;
            d /= 10;
            m_providers[static_cast<size_t>(m_sel)].context_length = d;
            if (d == 0) {
                m_context_auto = true;
                m_status.clear();
            }
            m_dirty = true;
            return;
        }
        agent::ProviderConfigEntry& cur = m_providers[static_cast<size_t>(m_sel)];
        std::string* vp = field_value(cur, m_field);
        if (vp && !vp->empty()) {
            pop_utf8_back(*vp);
            m_dirty = true;
            m_escape_armed = false;
            after_field_edit(cur);
        }
    }

    bool suggest_visible() const { return m_field == F_NAME && !m_suggest.empty(); }

    void form_move(int delta) {
        if (suggest_visible()) {
            if (m_suggest.empty()) return;
            const int n = static_cast<int>(m_suggest.size());
            m_suggest_sel += delta;
            if (m_suggest_sel < 0) m_suggest_sel = n - 1;
            if (m_suggest_sel >= n) m_suggest_sel = 0;
            return;
        }
        m_field = static_cast<Field>((static_cast<int>(m_field) + delta + F_COUNT) % F_COUNT);
        m_status.clear();
    }

    void next_field() {
        m_field = static_cast<Field>((static_cast<int>(m_field) + 1) % F_COUNT);
        if (m_field == F_NAME) m_status = "已循环到第一个字段";
    }

    bool form_enter() {
        if (suggest_visible()) {
            fill_suggest(false);
            return true;
        }
        if (m_field == F_KEY) {
            save_form();
            return true;
        }
        next_field();
        m_escape_armed = false;
        return true;
    }

    /// @brief 填充名称建议项 @param keep_suggest true=Tab 补全（保留建议继续过滤）
    void fill_suggest(bool keep_suggest) {
        const auto& item = m_suggest[static_cast<size_t>(m_suggest_sel)];
        agent::ProviderConfigEntry& cur = m_providers[static_cast<size_t>(m_sel)];
        cur.name = item.display;
        cur.id = item.name;
        if (item.is_custom) {
            // Custom URL：清空默认继承字段，交用户填 URL
            cur.name = "Custom URL";
            cur.id = kCustomProviderId;
            cur.base_url.clear();
            cur.model.clear();
            cur.context_length = 0;
            m_context_auto = true;
        } else {
            const agent::ProviderPreset* p = agent::find_preset(item.name);
            if (p) {
                cur.name = p->display_name;
                cur.id = std::string(p->name);
                cur.base_url = p->default_url;
                cur.model = p->default_model;
                cur.context_length = p->default_context_length;
                m_context_auto = true;
            }
        }
        m_dirty = true;
        m_escape_armed = false;
        if (!keep_suggest) {
            m_suggest.clear();
            m_field = F_URL;
            m_status = "已填入预设，可编辑 Base URL";
        } else {
            m_status = "已补全为 " + item.display;
        }
    }

    void refresh_suggest() {
        const std::string name = m_providers[static_cast<size_t>(m_sel)].name;
        m_suggest = filter_suggestions(m_suggest_all, name);
        m_suggest_sel = 0;
    }

    void save_form() {
        agent::ProviderConfigEntry& cur = m_providers[static_cast<size_t>(m_sel)];
        // 新条目 id 兜底（未选预设、未改动占位 id 时也应始终唯一）
        if (cur.id.empty()) cur.id = "provider-" + std::to_string(m_sel + 1);
        if (m_on_commit) m_on_commit();
        const bool was_new = m_editing_new;
        m_editing_new = false;
        m_dirty = false;
        m_escape_armed = false;
        m_suggest.clear();
        m_layer = Layer::List;
        m_status = was_new ? ("已添加供应商 " + cur.name) : ("已保存供应商 " + cur.name);
    }

    // ---------------- 渲染 ----------------
    Element label_bar(int item_count) {
        Element title = ftxui::text("  " + m_title) | ftxui::bold;
        std::string right = std::to_string(item_count) + " 个";
        return ftxui::hbox({title | ftxui::color(kAccent), ftxui::flex(ftxui::text("")),
                            ftxui::text(right) | ftxui::color(kTextFaint)});
    }

    Element render_list() {
        Elements rows;
        rows.push_back(label_bar(static_cast<int>(m_providers.size())));
        rows.push_back(ftxui::separatorEmpty());

        if (m_providers.empty()) {
            rows.push_back(ftxui::text("  暂无供应商，按 a 或点击「添加」新增") |
                           ftxui::color(kTextFaint));
        } else {
            const int n = static_cast<int>(m_providers.size());
            int start = std::min(m_sel, std::max(0, n - kMaxVisible));
            int count = std::min<int>(n - start, kMaxVisible);
            for (int v = 0; v < count; ++v) {
                const int idx = start + v;
                const agent::ProviderConfigEntry& e = m_providers[static_cast<size_t>(idx)];
                const bool active = (e.id == m_active_id);
                const bool sel = (idx == m_sel);
                std::string marker = active ? "  ● " : (sel ? "  ❯ " : "    ");
                auto mark = ftxui::text(marker) |
                            ftxui::color(active ? kGreen : (sel ? kAccent : ftxui::Color::Default));
                std::string name = e.name.empty() ? "(未命名)" : e.name;
                std::string sub = e.base_url.empty() ? "" : e.base_url;
                auto row = ftxui::hbox({
                    mark,
                    ftxui::text(name),
                    e.model.empty() ? ftxui::emptyElement()
                                    : (ftxui::text("  " + input_type_glyphs(e.model)) |
                                       ftxui::color(kTextFaint)),
                    ftxui::flex(ftxui::text("")),
                    active ? (ftxui::text("使用中") | ftxui::color(kGreen)) : ftxui::emptyElement(),
                    sub.empty() ? ftxui::emptyElement()
                                : (ftxui::text("  " + sub) | ftxui::color(kTextFaint)),
                    ftxui::text("  "),
                });
                if (sel) row = row | ftxui::bgcolor(kSelBg) | ftxui::color(kText) | ftxui::bold;
                m_item_boxes.emplace_back();
                m_item_rows.push_back(idx);
                rows.push_back(row | ftxui::reflect(m_item_boxes.back()));
            }
        }

        rows.push_back(ftxui::separatorEmpty());

        // 状态提示
        rows.push_back(ftxui::text("  " + m_status) | ftxui::color(kRed));
        rows.push_back(ftxui::separatorEmpty());

        // 底部操作按钮行
        auto add_btn = [&](const std::string& label, Action) {
            m_btn_boxes.emplace_back();
            return ftxui::hbox({ftxui::text(" "), ftxui::text(" " + label) | ftxui::color(kText),
                                ftxui::text(" ")}) |
                   ftxui::reflect(m_btn_boxes.back());
        };
        auto btn_hint = [&](const std::string& key, ftxui::Element e) {
            return ftxui::hbox({
                e,
                ftxui::text(" "),
                ftxui::text("[" + key + "]") | ftxui::color(kTextFaint),
            });
        };
        Element btns = ftxui::hbox({
            ftxui::text("  "),
            btn_hint("↵", add_btn("设为使用中", Action::Activate)),
            ftxui::text("   "),
            btn_hint("e", add_btn("编辑", Action::Edit)),
            ftxui::text("   "),
            btn_hint("a", add_btn("添加", Action::Add)),
            ftxui::text("   "),
            btn_hint("d", add_btn("删除", Action::Delete)),
            ftxui::flex(ftxui::text("")),
            ftxui::text(" "),
        });
        rows.push_back(btns | ftxui::color(kAccent));
        rows.push_back(ftxui::separatorEmpty());
        rows.push_back(
            ftxui::text("  ↑↓ 选择 · 回车 设为使用中 · e 编辑 · a/d 添加/删除 · Esc 关闭") |
            ftxui::color(kTextFaint));

        return ftxui::vbox(std::move(rows));
    }

    /// 标签列宽度（按显示列数，CJK 计 2），用于对齐各输入框起点
    int label_width() const {
        int w = 0;
        for (int i = 0; i < F_COUNT; ++i)
            w = std::max(w, ftxui::string_width(field_label(static_cast<Field>(i))));
        return w;
    }

    Element render_field(Field f, bool focused) {
        agent::ProviderConfigEntry& cur = m_providers[static_cast<size_t>(m_sel)];
        std::string value;
        if (f == F_CONTEXT) {
            value = ctx_display(cur);
        } else {
            std::string* vp = field_value(cur, f);
            value = vp ? *vp : "";
            if (f == F_KEY && !value.empty() && !focused) value = mask_key(value);
            if (value.empty()) value = "(空)";
        }
        // 标签左对齐到统一列，标签区定宽后接 │ 分割线，使各输入框起点（│）对齐
        const std::string lbl_text = "  " + std::string(field_label(f));
        const int label_col = label_width() + 3;  // 左 2 空格 + 最长标签 + 右 1 空格
        const int gap = std::max(0, label_col - ftxui::string_width(lbl_text));
        auto lbl = ftxui::text(lbl_text + std::string(gap, ' ')) |
                   ftxui::color(focused ? kAccent : kTextFaint);
        auto val = ftxui::text(focused ? std::string(" ") + value + " ▎" : " " + value) |
                   ftxui::color(kText) | (focused ? ftxui::bold : ftxui::nothing);
        auto row =
            ftxui::hbox({lbl, ftxui::text("│") | ftxui::color(focused ? kAccent : kTextFaint),
                         val | ftxui::flex});
        if (focused) row = row | ftxui::bgcolor(kSelBg);
        return row | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1);
    }

    /// 当前聚焦字段的引导提示（渲染在字段下方状态区，避免挤占输入框宽度）
    std::string field_hint() const {
        switch (m_field) {
            case F_NAME:
                return m_suggest.empty() ? std::string("输入名称，可参考下方建议")
                                         : (std::to_string(m_suggest.size()) + " 个匹配，↑↓ 选择");
            case F_URL:
                return "自定义供应商请输入完整 URL";
            case F_MODEL:
                return model_field_hint(m_providers[static_cast<size_t>(m_sel)].model);
            case F_CONTEXT:
                return "可直接输入数字覆盖";
            case F_KEY:
                return "粘贴 API Key 后按 Enter 保存";
            default:
                return "";
        }
    }

    std::string ctx_display(const agent::ProviderConfigEntry& e) {
        const int32_t auto_ctx = match_context_window(m_catalog.get(), e.model);
        if (m_context_auto && auto_ctx > 0) return std::to_string(auto_ctx) + " (auto)";
        if (e.context_length > 0)
            return std::to_string(e.context_length) + (m_context_auto ? " (auto)" : " (manual)");
        return "(未匹配，可直接输入数字)";
    }

    /// @brief 模型字段提示：输入能力（Nerd Font 图标）+ 自动匹配上下文说明
    std::string model_field_hint(const std::string& model) const {
        const std::string glyphs = input_type_glyphs(model);
        if (model.empty()) return "输入模型 ID 自动匹配上下文窗口";
        return glyphs + "  输入模型 ID 自动匹配上下文窗口";
    }

    Element render_form() {
        std::string head = m_editing_new ? "新增供应商" : "编辑供应商";

        Elements rows;
        rows.push_back(ftxui::hbox({
            ftxui::text("  " + m_title + " · " + head) | ftxui::color(kAccent) | ftxui::bold,
            ftxui::flex(ftxui::text("")),
            ftxui::text(std::to_string(m_sel + 1) + "/" + std::to_string(m_providers.size())) |
                ftxui::color(kTextFaint),
        }));
        rows.push_back(ftxui::separatorEmpty());

        const bool fname = (m_field == F_NAME);
        rows.push_back(render_field(F_NAME, fname));
        if (fname && !m_suggest.empty()) rows.push_back(render_suggest());

        rows.push_back(render_field(F_URL, m_field == F_URL));
        rows.push_back(render_field(F_MODEL, m_field == F_MODEL));
        rows.push_back(render_field(F_CONTEXT, m_field == F_CONTEXT));
        rows.push_back(render_field(F_KEY, m_field == F_KEY));

        rows.push_back(ftxui::separatorEmpty());
        // 当前字段引导（与输入框同一起点，避免输入框随提示伸缩）
        const std::string hint = field_hint();
        if (!hint.empty()) rows.push_back(ftxui::text("  " + hint) | ftxui::color(kTextFaint));
        rows.push_back(ftxui::text("  " + m_status) |
                       ftxui::color(m_status.empty() ? kText : kRed));
        rows.push_back(ftxui::separatorEmpty());
        rows.push_back(
            ftxui::text("  Enter 下一字段 · ↑↓ 字段/提示 · Tab 补全 · Ctrl+S 保存 · Esc 返回") |
            ftxui::color(kTextFaint));

        return ftxui::vbox(std::move(rows));
    }

    Element render_suggest() {
        Elements items;
        const int count = std::min<int>(kMaxSuggest, static_cast<int>(m_suggest.size()));
        for (int i = 0; i < count; ++i) {
            const auto& it = m_suggest[static_cast<size_t>(i)];
            const bool sel = (i == m_suggest_sel);
            std::string marker = sel ? "» " : "  ";
            std::string line = marker + it.display;
            if (!it.desc.empty()) line += "  " + it.desc;
            if (it.is_custom) line += "  (自定义地址)";
            auto row = ftxui::text("    " + line) | ftxui::color(sel ? kAccent : kTextDim);
            if (sel) row = row | ftxui::bgcolor(kSelBg) | ftxui::bold;
            items.push_back(row);
        }
        return ftxui::hbox({
            ftxui::text("  "),
            ftxui::vbox(std::move(items)) | ftxui::color(kTextFaint),
        });
    }
};

ftxui::Component make_provider_manager(ProviderManagerOptions&& opts, bool& open) {
    return ftxui::Make<ProviderManager>(std::move(opts), open);
}

}  // namespace ftxtui