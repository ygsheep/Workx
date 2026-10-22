#include "widgets/project_tree.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "theme/icons.h"
#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {

/// @brief 树区可视行数（终端高度 − 头部/分隔线/底栏常量：文件查看器同款估算）
int visible_row_count() {
    const int term_h = ftxui::Terminal::Size().dimy;
    return std::max(1, term_h - 7);
}

/// @brief 取文件扩展名（小写、无点；目录为空）
std::string file_ext(const std::string& name) {
    const auto slash = name.find_last_of("/\\");
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

/// @brief git 状态码 → 状态点颜色（'M'蓝 / 'A'绿 / 'D'红 / 'R'青 / 其余灰）
Color status_color(char st) {
    switch (st) {
        case 'M':
        case 'T':
            return theme::T::Accent;  // 修改
        case 'A':
            return theme::T::DiffAdd;  // 新增
        case 'D':
            return theme::T::DiffDel;  // 删除
        case 'R':
        case 'C':
            return ftxui::Color::RGB(0x4e, 0xc9, 0xb0);  // 重命名/复制
        default:
            return theme::T::TextFaint;  // 未跟踪/合并冲突等
    }
}

/// @brief 按 rel_path（'/' 分隔）在树中查找可变节点（展开折叠用；name 匹配子树）
ProjectNode* find_node_mutable(std::vector<ProjectNode>& root, const std::string& rel_path) {
    std::string rem = rel_path;
    std::vector<ProjectNode>* level = &root;
    ProjectNode* hit = nullptr;
    for (;;) {
        const auto idx = rem.find('/');
        const std::string seg = rem.substr(0, idx);
        ProjectNode* n = nullptr;
        for (auto& cand : *level)
            if (cand.name == seg) {
                n = &cand;
                break;
            }
        if (!n) break;
        hit = n;
        if (idx == std::string::npos) break;
        rem = rem.substr(idx + 1);
        if (!n->is_dir) break;
        level = &n->children;
    }
    return hit;
}

/// @brief 单行渲染：目录（chevron + 文件夹 + 名字 + /）或文件（缩进 + 图标 + 名字 + 状态点）
Element build_row(const ProjectNode& node, int depth, bool is_dir) {
    const std::string indent(static_cast<std::size_t>(depth), ' ');
    Elements parts;
    parts.push_back(ftxui::text("  "));  // 左侧内边距
    parts.push_back(ftxui::text(indent) | ftxui::color(theme::T::TextFaint));

    if (is_dir) {
        // 展开态文件夹：图标 + 名字统一主题强调色（Accent 蓝），收起态用默认主题色
        const Color dir_color = node.expanded ? theme::T::Accent : theme::T::TextDim;
        const Color name_color = node.expanded ? theme::T::Accent : theme::T::Text;
        // 引导列：展开/收起 chevron
        parts.push_back(ftxui::text(std::string(node.expanded ? theme::icon_chevron_down()
                                                              : theme::icon_chevron_right())) |
                        ftxui::color(theme::T::TextFaint));
        parts.push_back(ftxui::text(" "));
        parts.push_back(ftxui::text(std::string(node.expanded ? theme::icon_folder_open()
                                                              : theme::icon_folder())) |
                        ftxui::color(dir_color));
        parts.push_back(ftxui::text(" "));
        parts.push_back(ftxui::text(node.name) | ftxui::color(name_color));
        // 目录名后缀提示（ASCII 下仍可辨认目录）
        parts.push_back(ftxui::text(std::string(str::kProjectsDirSuffix)) |
                        ftxui::color(theme::T::TextFaint));
        parts.push_back(ftxui::flex(ftxui::text("")));
    } else {
        parts.push_back(ftxui::text(" ") | ftxui::color(theme::T::TextFaint));  // 对齐 chevron 列
        parts.push_back(ftxui::text(" "));
        parts.push_back(ftxui::text(std::string(theme::icon_file_by_ext(file_ext(node.name)))) |
                        ftxui::color(theme::T::TextFaint));
        parts.push_back(ftxui::text(" "));
        parts.push_back(ftxui::flex(ftxui::text(node.name) | ftxui::color(theme::T::TextDim)));
        // git 状态点（右对齐）
        if (node.has_status) {
            parts.push_back(ftxui::text(" "));
            parts.push_back(ftxui::text(std::string(theme::icon_dot())) |
                            ftxui::color(status_color(node.status)));
        }
    }
    return ftxui::hbox(std::move(parts));
}

/// @brief 头部：根目录名 + 改动数（git 仓库时）
Element build_header(const ProjectTreeState& p, int dirty_count) {
    std::string root_name = p.root.empty() ? std::string() : p.root;
    const auto slash = root_name.find_last_of("/\\");
    if (slash != std::string::npos) root_name = root_name.substr(slash + 1);
    Elements parts;
    parts.push_back(ftxui::text(root_name) | ftxui::color(theme::T::Text));
    if (p.is_git && dirty_count > 0) {
        parts.push_back(
            ftxui::text(std::to_string(dirty_count) + std::string(str::kProjectsChanges)) |
            ftxui::color(theme::T::DiffAdd));
    }
    parts.push_back(ftxui::flex(ftxui::text("")));
    return ftxui::hbox({
        ftxui::text(" "),
        ftxui::hbox(std::move(parts)),
        ftxui::text(" "),
    });
}

}  // namespace

std::vector<ProjectRow> flatten_project_rows(const std::vector<ProjectNode>& tree) {
    std::vector<ProjectRow> rows;
    std::function<void(const ProjectNode&, int)> walk = [&](const ProjectNode& node, int depth) {
        rows.push_back(ProjectRow{&node, depth});
        if (!node.is_dir || !node.expanded) return;
        for (const auto& child : node.children) walk(child, depth + 1);
    };
    for (const auto& node : tree) walk(node, 0);
    return rows;
}

Element build_project_tree(const ProjectTreeState& p) {
    // 加载中 / 空 / 非 git 占位
    if (p.loading) {
        return ftxui::vbox({
            ftxui::text(" "),
            ftxui::hbox({
                ftxui::text("  "),
                ftxui::text(std::string(str::kProjectsLoading)) | ftxui::color(theme::T::TextFaint),
            }),
        });
    }
    if (!p.ready) {
        return ftxui::vbox({
            ftxui::text(" "),
            ftxui::hbox({
                ftxui::text("  "),
                ftxui::text(std::string(str::kProjectsNoGit)) | ftxui::color(theme::T::TextFaint),
            }),
        });
    }
    if (!p.is_git && p.tree.empty()) {
        return ftxui::vbox({
            ftxui::text(" "),
            ftxui::hbox({
                ftxui::text("  "),
                ftxui::text(std::string(str::kProjectsNoGit)) | ftxui::color(theme::T::TextFaint),
            }),
        });
    }
    if (p.tree.empty()) {
        return ftxui::vbox({
            ftxui::text(" "),
            ftxui::hbox({
                ftxui::text("  "),
                ftxui::text(std::string(str::kProjectsEmpty)) | ftxui::color(theme::T::TextFaint),
            }),
        });
    }

    const auto rows = flatten_project_rows(p.tree);
    int dirty_count = 0;
    for (const auto& r : rows)
        if (r.node->has_status) ++dirty_count;

    // 虚拟化滚动：只渲染可视切片（行高固定 1）
    const int visible = visible_row_count();
    const int scroll =
        std::clamp(p.scroll, 0, std::max(0, static_cast<int>(rows.size()) - visible));

    Elements line_els;
    line_els.reserve(static_cast<std::size_t>(visible));
    for (int i = 0; i < visible; ++i) {
        const int idx = scroll + i;
        if (idx >= static_cast<int>(rows.size())) {
            line_els.push_back(ftxui::text(""));
            continue;
        }
        const auto& r = rows[static_cast<std::size_t>(idx)];
        line_els.push_back(build_row(*r.node, r.depth, r.node->is_dir));
    }

    return ftxui::vbox({
        build_header(p, dirty_count),
        ftxui::separator() | ftxui::color(theme::T::TextFaint),
        ftxui::vbox(std::move(line_els)) | ftxui::yflex,
        ftxui::separator() | ftxui::color(theme::T::TextFaint),
        ftxui::hbox({
            ftxui::text("  "),
            ftxui::text(std::string(str::kProjectsHint)) | ftxui::color(theme::T::TextFaint),
        }),
    });
}

namespace {
class ProjectTree : public ftxui::ComponentBase {
   public:
    ProjectTree(ProjectTreeState* project, std::function<void(const std::string&)> on_open_file)
        : m_project(project), m_on_open_file(std::move(on_open_file)) {}

    /// @brief 树扁平行数（供滚动钳制与命中行计算）
    int row_count() const {
        if (!m_project) return 0;
        return static_cast<int>(flatten_project_rows(m_project->tree).size());
    }

    /// @brief 点击行：目录切换展开/收起，文件回调打开（App 侧转发鼠标事件）
    bool OnEvent(ftxui::Event event) override {
        if (!m_project || !event.is_mouse()) return false;
        const auto& m = event.mouse();
        if (m.button != ftxui::Mouse::Left || m.motion != ftxui::Mouse::Pressed) return false;
        // 列表区 = m_box 去掉头部(1)+分隔(1) 与底部分隔(1)+提示(1)
        const int top = m_box.y_min + 2;
        const int bottom = m_box.y_max - 2;
        if (m.y < top || m.y > bottom) return false;
        const int row = (m.y - top) + m_project->scroll;
        if (row < 0 || row >= row_count()) return false;
        const auto rows = flatten_project_rows(m_project->tree);
        const auto node_rel = rows[static_cast<std::size_t>(row)].node->rel_path;
        if (rows[static_cast<std::size_t>(row)].node->is_dir) {
            if (ProjectNode* n = find_node_mutable(m_project->tree, node_rel))
                n->expanded = !n->expanded;
        } else if (m_on_open_file) {
            m_on_open_file(node_rel);
        }
        return true;
    }

    Element OnRender() override {
        if (!m_project) return ftxui::emptyElement();
        return build_project_tree(*m_project) | ftxui::reflect(m_box);
    }

   private:
    ProjectTreeState* m_project;
    std::function<void(const std::string&)> m_on_open_file;
    ftxui::Box m_box;  ///< 组件渲染区域（点击命中用）
};
}  // namespace

ftxui::Component make_project_tree(ProjectTreeState* project,
                                   std::function<void(const std::string&)> on_open_file) {
    return ftxui::Make<ProjectTree>(std::move(project), std::move(on_open_file));
}

}  // namespace ftxtui