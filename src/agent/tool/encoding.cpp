/**
 * @file encoding.cpp
 * @brief 文件编码检测与转换实现
 * @details 实现 BOM 检测、UTF-8 验证、GBK 启发式检测，
 *          以及 UTF-16→UTF-8 和 GBK→UTF-8 编码转换。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/encoding.h"

#include <fstream>
#include <sstream>
#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <iconv.h>
#include <cerrno>
#endif

namespace agent::tool {

namespace fs = std::filesystem;

// ============================================================
// 内部辅助函数
// ============================================================

/// @brief UTF-16 → UTF-8 转换（模板化，LE/BE 共享实现）
/// @tparam LittleEndian true=LE（低字节在前），false=BE（高字节在前）
template <bool LittleEndian>
static std::string utf16_to_utf8_impl(const char* data, size_t size) {
    std::string result;
    result.reserve(size);

    // 按 endian 读取 16-bit code unit
    auto read_cu = [](const char* p) -> uint16_t {
        const uint8_t b0 = static_cast<uint8_t>(p[0]);
        const uint8_t b1 = static_cast<uint8_t>(p[1]);
        if constexpr (LittleEndian) {
            return static_cast<uint16_t>(b0 | (b1 << 8));
        } else {
            return static_cast<uint16_t>((b0 << 8) | b1);
        }
    };

    for (size_t i = 0; i + 1 < size; i += 2) {
        const uint16_t cu = read_cu(data + i);

        // 高代理项（surrogate pair）
        if (cu >= 0xD800 && cu <= 0xDBFF) {
            if (i + 3 < size) {
                const uint16_t lo = read_cu(data + i + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    const uint32_t cp = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
                    result += static_cast<char>(0xF0 | (cp >> 18));
                    result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                    ++i;  // 跳过低代理项
                    continue;
                }
            }
            continue;  // 无效高代理项，跳过
        }

        // 孤立低代理项，跳过
        if (cu >= 0xDC00 && cu <= 0xDFFF) continue;

        // BMP 字符
        if (cu < 0x80) {
            result += static_cast<char>(cu);
        } else if (cu < 0x800) {
            result += static_cast<char>(0xC0 | (cu >> 6));
            result += static_cast<char>(0x80 | (cu & 0x3F));
        } else {
            result += static_cast<char>(0xE0 | (cu >> 12));
            result += static_cast<char>(0x80 | ((cu >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cu & 0x3F));
        }
    }
    return result;
}

/// @brief UTF-16 LE → UTF-8 转换
static std::string utf16le_to_utf8(const char* data, size_t size) {
    return utf16_to_utf8_impl<true>(data, size);
}

/// @brief UTF-16 BE → UTF-8 转换
static std::string utf16be_to_utf8(const char* data, size_t size) {
    return utf16_to_utf8_impl<false>(data, size);
}

/// @brief GBK → UTF-8 转换（平台相关）
static std::string gbk_to_utf8(const char* data, size_t size) {
#ifdef _WIN32
    // Windows: MultiByteToWideChar (GBK = CP_936)
    const int wlen = MultiByteToWideChar(936, 0, data, static_cast<int>(size), nullptr, 0);
    if (wlen == 0) return std::string(data, size);  // 回退

    std::wstring wstr(static_cast<size_t>(wlen), 0);
    MultiByteToWideChar(936, 0, data, static_cast<int>(size), wstr.data(), wlen);

    const int ulen =
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen == 0) return std::string(data, size);  // 回退

    std::string result(static_cast<size_t>(ulen), 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen, result.data(), ulen, nullptr, nullptr);
    return result;
#else
    // Linux: iconv
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == reinterpret_cast<iconv_t>(-1)) return std::string(data, size);

    size_t in_left = size;
    size_t out_left = size * 3;  // 最坏情况
    std::string result(out_left, 0);

    char* in_buf = const_cast<char*>(data);
    char* out_buf = result.data();

    const size_t ret = iconv(cd, &in_buf, &in_left, &out_buf, &out_left);
    iconv_close(cd);

    if (ret == static_cast<size_t>(-1)) return std::string(data, size);  // 回退

    result.resize(result.size() - out_left);
    return result;
#endif
}

/// @brief UTF-8 → UTF-16LE 转换（不含 BOM）
/// @details 处理 1-4 字节 UTF-8 序列，输出小端序 UTF-16 码元。
///          BMP 之外字符（U+10000+）编码为代理对。
static std::string utf8_to_utf16le(const std::string& utf8) {
    std::string result;
    result.reserve(utf8.size() * 2);

    const auto append_le = [&result](uint16_t cu) {
        result += static_cast<char>(cu & 0xFF);
        result += static_cast<char>((cu >> 8) & 0xFF);
    };

    const size_t n = utf8.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0;

        if (c < 0x80) {
            cp = c;
            ++i;
        } else if (c < 0xC2) {
            // 非法首字节，替换为 U+FFFD
            cp = 0xFFFD;
            ++i;
        } else if (c < 0xE0) {
            if (i + 1 >= n) {
                cp = 0xFFFD;
                ++i;
                continue;
            }
            cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F);
            i += 2;
        } else if (c < 0xF0) {
            if (i + 2 >= n) {
                cp = 0xFFFD;
                ++i;
                continue;
            }
            cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(utf8[i + 2]) & 0x3F);
            i += 3;
        } else if (c < 0xF5) {
            if (i + 3 >= n) {
                cp = 0xFFFD;
                ++i;
                continue;
            }
            cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(utf8[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(utf8[i + 3]) & 0x3F);
            i += 4;
        } else {
            cp = 0xFFFD;
            ++i;
        }

        if (cp <= 0xFFFF) {
            append_le(static_cast<uint16_t>(cp));
        } else {
            // 代理对编码
            cp -= 0x10000;
            const uint16_t hi = 0xD800 + static_cast<uint16_t>(cp >> 10);
            const uint16_t lo = 0xDC00 + static_cast<uint16_t>(cp & 0x3FF);
            append_le(hi);
            append_le(lo);
        }
    }
    return result;
}

/// @brief UTF-8 → UTF-16BE 转换（不含 BOM）
static std::string utf8_to_utf16be(const std::string& utf8) {
    std::string result;
    result.reserve(utf8.size() * 2);

    const auto append_be = [&result](uint16_t cu) {
        result += static_cast<char>((cu >> 8) & 0xFF);
        result += static_cast<char>(cu & 0xFF);
    };

    const size_t n = utf8.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0;

        if (c < 0x80) {
            cp = c;
            ++i;
        } else if (c < 0xC2) {
            cp = 0xFFFD;
            ++i;
        } else if (c < 0xE0) {
            if (i + 1 >= n) {
                cp = 0xFFFD;
                ++i;
                continue;
            }
            cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F);
            i += 2;
        } else if (c < 0xF0) {
            if (i + 2 >= n) {
                cp = 0xFFFD;
                ++i;
                continue;
            }
            cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(utf8[i + 2]) & 0x3F);
            i += 3;
        } else if (c < 0xF5) {
            if (i + 3 >= n) {
                cp = 0xFFFD;
                ++i;
                continue;
            }
            cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(utf8[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(utf8[i + 3]) & 0x3F);
            i += 4;
        } else {
            cp = 0xFFFD;
            ++i;
        }

        if (cp <= 0xFFFF) {
            append_be(static_cast<uint16_t>(cp));
        } else {
            cp -= 0x10000;
            const uint16_t hi = 0xD800 + static_cast<uint16_t>(cp >> 10);
            const uint16_t lo = 0xDC00 + static_cast<uint16_t>(cp & 0x3FF);
            append_be(hi);
            append_be(lo);
        }
    }
    return result;
}

/// @brief UTF-8 → GBK 转换（平台相关）
/// @details Windows: WideCharToMultiByte (CP_936)
///          失败时回退为原 UTF-8 字节流（避免数据丢失）。
static std::string utf8_to_gbk(const std::string& utf8) {
#ifdef _WIN32
    // UTF-8 → UTF-16
    const int wlen =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen == 0) return utf8;  // 回退

    std::wstring wstr(static_cast<size_t>(wlen), 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wstr.data(), wlen);

    // UTF-16 → GBK (CP_936)
    const int glen = WideCharToMultiByte(936, 0, wstr.c_str(), wlen, nullptr, 0, nullptr, nullptr);
    if (glen == 0) return utf8;  // 回退

    std::string result(static_cast<size_t>(glen), 0);
    WideCharToMultiByte(936, 0, wstr.c_str(), wlen, result.data(), glen, nullptr, nullptr);
    return result;
#else
    iconv_t cd = iconv_open("GBK", "UTF-8");
    if (cd == reinterpret_cast<iconv_t>(-1)) return utf8;

    size_t in_left = utf8.size();
    size_t out_left = utf8.size() * 2;  // GBK 最坏情况
    std::string result(out_left, 0);

    char* in_buf = const_cast<char*>(utf8.data());
    char* out_buf = result.data();

    const size_t ret = iconv(cd, &in_buf, &in_left, &out_buf, &out_left);
    iconv_close(cd);

    if (ret == static_cast<size_t>(-1)) return utf8;  // 回退

    result.resize(result.size() - out_left);
    return result;
#endif
}

/// @brief 验证字节序列是否为合法 UTF-8
/// @return 0=非UTF-8, 1=纯ASCII, 2=含多字节序列的UTF-8
static int validate_utf8(const unsigned char* buf, size_t len) {
    bool has_multibyte = false;
    for (size_t i = 0; i < len;) {
        const unsigned char c = buf[i];
        if (c < 0x80) {
            ++i;
        } else if (c < 0xC2) {
            return 0;  // 非法首字节
        } else if (c < 0xE0) {
            if (i + 1 >= len || (buf[i + 1] & 0xC0) != 0x80) return 0;
            has_multibyte = true;
            i += 2;
        } else if (c < 0xF0) {
            if (i + 2 >= len || (buf[i + 1] & 0xC0) != 0x80 || (buf[i + 2] & 0xC0) != 0x80)
                return 0;
            has_multibyte = true;
            i += 3;
        } else if (c < 0xF5) {
            if (i + 3 >= len || (buf[i + 1] & 0xC0) != 0x80 || (buf[i + 2] & 0xC0) != 0x80 ||
                (buf[i + 3] & 0xC0) != 0x80)
                return 0;
            has_multibyte = true;
            i += 4;
        } else {
            return 0;
        }
    }
    return has_multibyte ? 2 : 1;
}

/// @brief GBK 启发式检测（增强版：字符频率统计）
/// @details GBK 双字节：首字节 0x81-0xFE，次字节 0x40-0xFE（不含 0x7F）
///          增强策略：
///          1. 全序列校验：任一不合法双字节序列立即否决（原逻辑保留）
///          2. 最小双字节数量：至少 3 个，避免短样本误判
///          3. 双字节占比：双字节字符 / 总字符 ≥ 5%，排除纯 ASCII 偶混高位字节
///          4. 常用汉字占比：首字节落在 0xB0-0xF7（GB2312 一级+二级汉字）的双字节
///             占比 ≥ 20%，用于区分 Shift-JIS（日文假名首字节多在 0x81-0x9F）
static bool is_likely_gbk(const unsigned char* buf, size_t len) {
    size_t total_chars = 0;        // 总字符数（单字节+双字节各算 1）
    size_t double_byte_chars = 0;  // 双字节字符数
    size_t common_chinese = 0;     // 常用汉字（首字节 0xB0-0xF7）

    for (size_t i = 0; i < len;) {
        const unsigned char c = buf[i];
        if (c < 0x80) {
            ++total_chars;
            ++i;
        } else if (c >= 0x81 && c <= 0xFE) {
            if (i + 1 >= len) break;  // 最后一个字节无法判断
            const unsigned char c2 = buf[i + 1];
            if (c2 >= 0x40 && c2 <= 0xFE && c2 != 0x7F) {
                ++total_chars;
                ++double_byte_chars;
                if (c >= 0xB0 && c <= 0xF7) {
                    ++common_chinese;
                }
                i += 2;
            } else {
                return false;  // 不符合 GBK 模式
            }
        } else {
            return false;  // 0x80 / 0xFF 不是合法 GBK 首字节
        }
    }

    if (double_byte_chars < 3) return false;  // 样本太少，不可靠

    const double db_ratio = static_cast<double>(double_byte_chars) / total_chars;
    const double common_ratio = static_cast<double>(common_chinese) / double_byte_chars;

    // 双字节占比过低：可能是纯 ASCII 偶混入高位字节
    if (db_ratio < 0.05) return false;

    // 常用汉字比例过低：可能是 Shift-JIS 等其他 CJK 编码
    // （日文假名首字节多在 0x81-0x9F，不在 0xB0-0xF7 常用汉字范围）
    if (common_ratio < 0.2) return false;

    return true;
}

// ============================================================
// 公共接口实现
// ============================================================

Encoding detect_encoding(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return Encoding::Unknown;

    constexpr size_t CHECK_SIZE = 8192;
    unsigned char buf[CHECK_SIZE];
    file.read(reinterpret_cast<char*>(buf), CHECK_SIZE);
    const size_t n = static_cast<size_t>(file.gcount());

    if (n == 0) return Encoding::Utf8;  // 空文件视为 UTF-8

    // 1. BOM 检测
    if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) return Encoding::Utf8;
    if (n >= 2 && buf[0] == 0xFF && buf[1] == 0xFE) return Encoding::Utf16LE;
    if (n >= 2 && buf[0] == 0xFE && buf[1] == 0xFF) return Encoding::Utf16BE;

    // 2. null 字节检测（二进制判定，UTF-16 已由 BOM 排除）
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == 0) return Encoding::Binary;
    }

    // 3. UTF-8 验证
    const int utf8_type = validate_utf8(buf, n);
    if (utf8_type == 1) return Encoding::Ascii;
    if (utf8_type == 2) return Encoding::Utf8;

    // 4. GBK 启发式检测
    if (is_likely_gbk(buf, n)) return Encoding::Gbk;

    return Encoding::Unknown;
}

std::vector<std::string> read_as_utf8_lines(const fs::path& path, Encoding encoding) {
    std::vector<std::string> lines;

    // UTF-8/ASCII：直接读取（跳过 BOM）
    if (encoding == Encoding::Utf8 || encoding == Encoding::Ascii ||
        encoding == Encoding::Unknown) {
        std::ifstream file(path);
        if (!file.is_open()) return lines;

        // 跳过 UTF-8 BOM（若存在）
        skip_utf8_bom(file);

        std::string line;
        while (std::getline(file, line)) {
            normalize_eol(line);
            lines.push_back(std::move(line));
        }
        return lines;
    }

    // 非 UTF-8：全量读取 + 编码转换
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return lines;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    std::string utf8_content;
    size_t skip = 0;  // 跳过 BOM

    switch (encoding) {
        case Encoding::Utf16LE:
            skip = 2;  // FF FE
            utf8_content = utf16le_to_utf8(content.data() + skip, content.size() - skip);
            break;
        case Encoding::Utf16BE:
            skip = 2;  // FE FF
            utf8_content = utf16be_to_utf8(content.data() + skip, content.size() - skip);
            break;
        case Encoding::Gbk:
            utf8_content = gbk_to_utf8(content.data(), content.size());
            break;
        default:
            utf8_content = content;
            break;
    }

    // 按行分割
    std::istringstream stream(utf8_content);
    std::string line;
    while (std::getline(stream, line)) {
        normalize_eol(line);
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string read_file_as_utf8(const fs::path& path, Encoding encoding) {
    // UTF-8/ASCII：直接读取并跳过 BOM
    if (encoding == Encoding::Utf8 || encoding == Encoding::Ascii ||
        encoding == Encoding::Unknown || encoding == Encoding::Binary) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return {};

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        // 跳过 UTF-8 BOM（若存在）
        if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
            static_cast<unsigned char>(content[1]) == 0xBB &&
            static_cast<unsigned char>(content[2]) == 0xBF) {
            content.erase(0, 3);
        }
        return content;
    }

    // 非 UTF-8：全量读取 + 编码转换（跳过 BOM）
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    switch (encoding) {
        case Encoding::Utf16LE:
            // FF FE BOM (2 字节)
            if (content.size() >= 2) {
                return utf16le_to_utf8(content.data() + 2, content.size() - 2);
            }
            return {};
        case Encoding::Utf16BE:
            // FE FF BOM (2 字节)
            if (content.size() >= 2) {
                return utf16be_to_utf8(content.data() + 2, content.size() - 2);
            }
            return {};
        case Encoding::Gbk:
            return gbk_to_utf8(content.data(), content.size());
        default:
            return content;
    }
}

bool write_file_with_encoding(const fs::path& path, const std::string& utf8_content,
                              Encoding encoding, const std::function<bool()>& is_cancelled) {
    namespace fsx = std::filesystem;

    // 原子写（#23 P2）：先写同目录临时文件，rename 原子替换。
    // 写入失败/取消只清理临时文件，原文件始终保持完整。
    fsx::path tmp_path = path;
    tmp_path += ".workx.tmp";
    std::error_code ec;
    fsx::remove(tmp_path, ec);  // 清理可能残留的旧临时文件

    {
        std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;

        switch (encoding) {
            case Encoding::Utf16LE: {
                // FF FE BOM
                file.put(static_cast<char>(0xFF));
                file.put(static_cast<char>(0xFE));
                const std::string encoded = utf8_to_utf16le(utf8_content);
                file.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
                break;
            }
            case Encoding::Utf16BE: {
                // FE FF BOM
                file.put(static_cast<char>(0xFE));
                file.put(static_cast<char>(0xFF));
                const std::string encoded = utf8_to_utf16be(utf8_content);
                file.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
                break;
            }
            case Encoding::Gbk: {
                const std::string encoded = utf8_to_gbk(utf8_content);
                file.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
                break;
            }
            default:
                // UTF-8/ASCII/Binary/Unknown：原样写入（不加 BOM，对齐 CC 行为）
                file.write(utf8_content.data(), static_cast<std::streamsize>(utf8_content.size()));
                break;
        }

        file.flush();
        if (!file) {
            file.close();
            fsx::remove(tmp_path, ec);
            return false;
        }
    }

    // 取消点：rename 前检查，已取消则不触碰原文件
    if (is_cancelled && is_cancelled()) {
        fsx::remove(tmp_path, ec);
        return false;
    }

    fsx::rename(tmp_path, path, ec);
    if (ec) {
        // rename 失败（跨卷/目标占用等）：清理临时文件，原文件未受影响
        fsx::remove(tmp_path, ec);
        return false;
    }
    return true;
}

const char* encoding_name(Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf8:
            return "UTF-8";
        case Encoding::Utf16LE:
            return "UTF-16LE";
        case Encoding::Utf16BE:
            return "UTF-16BE";
        case Encoding::Gbk:
            return "GBK";
        case Encoding::Ascii:
            return "ASCII";
        case Encoding::Binary:
            return "Binary";
        default:
            return "Unknown";
    }
}

void normalize_eol(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

bool skip_utf8_bom(std::ifstream& file) {
    char bom[3];
    file.read(bom, 3);
    if (file.gcount() == 3 && static_cast<unsigned char>(bom[0]) == 0xEF &&
        static_cast<unsigned char>(bom[1]) == 0xBB && static_cast<unsigned char>(bom[2]) == 0xBF) {
        return true;  // BOM 已跳过
    }
    file.seekg(0);  // 无 BOM，回到开头
    return false;
}

std::string sanitize_utf8(std::string_view text) {
    static constexpr char kUfffd[] = "\xEF\xBF\xBD";  // U+FFFD (EF BF BD)

    const char* data = text.data();
    const size_t len = text.size();
    std::string result;
    result.reserve(len);

    size_t i = 0;
    auto append_valid = [&](size_t start, size_t count) { result.append(data + start, count); };

    while (i < len) {
        const unsigned char c = static_cast<unsigned char>(data[i]);

        if (c < 0x80) {
            // ASCII 或合法 1 字节：直接保留
            result.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        // 判定当前多字节序列长度
        int seq_len = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            seq_len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            seq_len = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            seq_len = 4;
        } else {
            // 孤立续字节 (0x80-0xBF)、C0/C1、0xF5-0xFF：非法首字节
            result += kUfffd;
            ++i;
            continue;
        }

        // 检查续字节是否完整且合法
        bool ok = (i + static_cast<size_t>(seq_len) <= len);
        if (ok) {
            for (int k = 1; k < seq_len; ++k) {
                const unsigned char cc = static_cast<unsigned char>(data[i + k]);
                if ((cc & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
            }
        }
        // 约束：不允许字节序列过长表示（overlong）与非法码点
        if (ok && seq_len == 3 &&
            (c == 0xE0 && (static_cast<unsigned char>(data[i + 1]) & 0xE0) == 0x80)) {
            ok = false;  // E0 80..9F 的 overlong 2-byte
        }
        if (ok && seq_len == 4 &&
            (c == 0xF0 && (static_cast<unsigned char>(data[i + 1]) & 0xF0) == 0x80)) {
            ok = false;  // F0 80..8F 的 overlong 3-byte
        }
        if (ok && seq_len == 4 && (c == 0xF4 && static_cast<unsigned char>(data[i + 1]) > 0x8F)) {
            ok = false;  // F4 90..BF 超出 U+10FFFF
        }

        if (ok) {
            append_valid(i, static_cast<size_t>(seq_len));
            i += static_cast<size_t>(seq_len);
        } else {
            // 非法序列 / 续字节缺失：按 maximal subpart 策略合并替换，
            // 跳过能构成最长合法前缀的续字节（如截断的 3 字节序列 E6 96 → 单个 U+FFFD，
            // 而非逐字节各替换一个）
            size_t consumed = 1;
            while (i + consumed < len &&
                   (static_cast<unsigned char>(data[i + consumed]) & 0xC0) == 0x80) {
                ++consumed;
            }
            result += kUfffd;
            i += consumed;
        }
    }

    return result;
}

nlohmann::json sanitize_json_strings(const nlohmann::json& j) {
    if (j.is_string()) {
        return nlohmann::json(sanitize_utf8(j.get_ref<const std::string&>()));
    }
    if (j.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& item : j) {
            out.push_back(sanitize_json_strings(item));
        }
        return out;
    }
    if (j.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = j.begin(); it != j.end(); ++it) {
            out[it.key()] = sanitize_json_strings(it.value());
        }
        return out;
    }
    return j;  // number/bool/null 原样返回
}

}  // namespace agent::tool
