#include "render/image_view.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include "stb_image.h"
#include "theme/theme.h"

namespace ftxtui {

namespace {

/// @brief 解码后最大边长（像素），控制内存（1024² × 4 ≈ 4MB）
constexpr int kImageMaxEdge = 1024;

/// @brief 小写化扩展名（含点，如 ".png"）
std::string lower_ext(const std::string& path) {
    std::string ext;
    const auto dot = path.find_last_of('.');
    if (dot != std::string::npos) ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

/// @brief RGBA → 终端前景/背景色；透明像素返回面板背景色
ftxui::Color rgba_color(const uint8_t* p) {
    if (p[3] < 128) return theme::T::Panel;
    return ftxui::Color::RGB(p[0], p[1], p[2]);
}

/// @brief 计算缩放后终端尺寸（像素列宽 cols、终端行数 rows、采样比例 sw/sh）
void fit_size(const ImageData& img, int avail_w, int avail_h, int* cols, int* rows, float* sw,
              float* sh) {
    const int iw = img.width, ih = img.height;
    if (iw <= 0 || ih <= 0) {
        *cols = 1;
        *rows = 1;
        *sw = 1.0f;
        *sh = 1.0f;
        return;
    }
    const int avail_px_h = std::max(2, avail_h * 2);  // 可视像素高（每行 2 像素）
    const int avail_wc = std::max(1, avail_w);
    float scale = std::min(static_cast<float>(avail_wc) / static_cast<float>(iw),
                           static_cast<float>(avail_px_h) / static_cast<float>(ih));
    scale = std::min(scale, 1.0f);  // 不放大
    const int dw = std::max(1, static_cast<int>(static_cast<float>(iw) * scale));
    const int dh = std::max(2, static_cast<int>(static_cast<float>(ih) * scale));
    *cols = dw;
    *rows = (dh + 1) / 2;
    *sw = static_cast<float>(iw) / static_cast<float>(dw);
    *sh = static_cast<float>(ih) / static_cast<float>(dh);
}

/// @brief 半块渲染节点：每个终端单元格显示 2 个源像素（▀ 上=前景、下=背景）
class ImageNode : public ftxui::Node {
   public:
    ImageNode(const ImageData& img, int cols, int rows, int scroll, float sw, float sh)
        : img_(img), cols_(cols), rows_(rows), scroll_(scroll), sw_(sw), sh_(sh) {}

    void ComputeRequirement() override {
        requirement_.min_x = cols_;
        requirement_.min_y = rows_;
        requirement_.flex_grow_x = 0;
        requirement_.flex_grow_y = 0;
        requirement_.flex_shrink_x = 0;
        requirement_.flex_shrink_y = 0;
    }

    void Render(ftxui::Screen& screen) override {
        const int iw = img_.width;
        const int ih = img_.height;
        if (iw <= 0 || ih <= 0 || img_.rgba.empty()) return;
        const uint8_t* px = img_.rgba.data();

        const int x0 = box_.x_min;
        const int y0 = box_.y_min;
        const int x1 = box_.x_max;
        const int y1 = box_.y_max;
        const int box_w = x1 - x0 + 1;
        const int x_off = std::max(0, (box_w - cols_) / 2);  // 水平居中

        for (int y = y0; y <= y1; ++y) {
            const int cell_y = y - y0;
            const int pix_y0 = (cell_y + scroll_) * 2;  // 缩放空间像素行（上）
            const int sy0 = static_cast<int>(static_cast<float>(pix_y0) * sh_);
            if (sy0 >= ih) break;  // 已到底部
            int sy1 = static_cast<int>(static_cast<float>(pix_y0 + 1) * sh_);
            if (sy1 >= ih) sy1 = sy0;  // 底越界：回退上像素
            for (int x = x0; x <= x1; ++x) {
                const int cell_x = x - x0 - x_off;
                if (cell_x < 0 || cell_x >= cols_) continue;  // 两侧留白
                const int sx = std::min(iw - 1, static_cast<int>(static_cast<float>(cell_x) * sw_));
                const uint8_t* top = &px[(sy0 * iw + sx) * 4];
                const uint8_t* bot = &px[(sy1 * iw + sx) * 4];
                ftxui::Cell& cell = screen.CellAt(x, y);
                cell.character = "\u2580";  // ▀ UPPER HALF BLOCK
                cell.foreground_color = rgba_color(top);
                cell.background_color = rgba_color(bot);
            }
        }
    }

   private:
    const ImageData& img_;
    int cols_;
    int rows_;
    int scroll_;
    float sw_;
    float sh_;
};

}  // namespace

bool is_image_file(const std::string& path) {
    const std::string ext = lower_ext(path);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif" ||
           ext == ".webp" || ext == ".tga";
}

std::shared_ptr<ImageData> decode_image_file(const std::string& path, std::string* err) {
    const auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return std::shared_ptr<ImageData>();
    };

    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("无法读取文件");
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    if (buf.empty()) return fail("文件为空");

    int w = 0, h = 0, ch = 0;
    unsigned char* raw = stbi_load_from_memory(buf.data(), static_cast<int>(buf.size()), &w, &h,
                                               &ch, 4);  // 强制 RGBA
    if (!raw) {
        const char* reason = stbi_failure_reason();
        return fail(reason ? reason : "解码失败");
    }

    // 最近邻下采样（最大边 kImageMaxEdge）
    int nw = w, nh = h;
    float k = 1.0f;
    const int mx = std::max(w, h);
    if (mx > kImageMaxEdge) {
        k = static_cast<float>(kImageMaxEdge) / static_cast<float>(mx);
        nw = std::max(1, static_cast<int>(static_cast<float>(w) * k));
        nh = std::max(1, static_cast<int>(static_cast<float>(h) * k));
    }

    auto img = std::make_shared<ImageData>();
    img->width = nw;
    img->height = nh;
    img->rgba.resize(static_cast<std::size_t>(nw) * nh * 4);

    if (nw == w && nh == h) {
        std::memcpy(img->rgba.data(), raw, img->rgba.size());
    } else {
        const float inv_k = 1.0f / k;
        for (int y = 0; y < nh; ++y) {
            const int sy = std::min(w - 1, static_cast<int>(static_cast<float>(y) * inv_k));
            for (int x = 0; x < nw; ++x) {
                const int sx = std::min(w - 1, static_cast<int>(static_cast<float>(x) * inv_k));
                std::memcpy(&img->rgba[(static_cast<std::size_t>(y) * nw + x) * 4],
                            &raw[(static_cast<std::size_t>(sy) * w + sx) * 4], 4);
            }
        }
    }
    stbi_image_free(raw);
    return img;
}

int image_view_rows(const ImageData& img, int avail_width, int avail_height) {
    int cols, rows;
    float sw, sh;
    fit_size(img, avail_width, avail_height, &cols, &rows, &sw, &sh);
    return rows;
}

ftxui::Element build_image_content(const ImageData& img, int avail_width, int avail_height,
                                   int scroll) {
    int cols, rows;
    float sw, sh;
    fit_size(img, avail_width, avail_height, &cols, &rows, &sw, &sh);
    auto node = std::make_shared<ImageNode>(img, cols, rows, scroll, sw, sh);
    const int view_rows = std::min(rows, std::max(1, avail_height));
    return node | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, view_rows);
}

}  // namespace ftxtui
