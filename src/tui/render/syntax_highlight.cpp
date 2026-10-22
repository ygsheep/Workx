#include "render/syntax_highlight.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef WORKX_HAS_TREE_SITTER
#include <tree_sitter/api.h>

// 各 grammar 的入口函数声明由 scripts/gen_ts_grammars.py 生成，无需手改
extern "C" {
#include "render/ts_langs_decl.inc"
}
#endif  // WORKX_HAS_TREE_SITTER

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {

// ---- 配色（One Dark 风格）----
const Color kKeyword = Color::BlueLight;      // 关键字
const Color kType = Color::CyanLight;         // 内置类型/库标识
const Color kString = Color::GreenLight;      // 字符串
const Color kComment = Color::GrayDark;       // 注释
const Color kNumber = Color::YellowLight;     // 数字
const Color kPreproc = Color::MagentaLight;   // 预处理指令
const Color kFunction = Color::MagentaLight;  // 函数名
const Color kConstant = Color::YellowLight;   // 常量
const Color kProperty = Color::CyanLight;     // 属性/字段

// ---- 关键字表（常见语言子集）----

const std::string_view kCppKeywords[] = {
    "alignas",      "alignof",
    "and",          "asm",
    "auto",         "bool",
    "break",        "case",
    "catch",        "char",
    "class",        "const",
    "constexpr",    "const_cast",
    "continue",     "decltype",
    "default",      "delete",
    "do",           "double",
    "dynamic_cast", "else",
    "enum",         "explicit",
    "export",       "extern",
    "false",        "float",
    "for",          "friend",
    "goto",         "if",
    "inline",       "int",
    "long",         "mutable",
    "namespace",    "new",
    "noexcept",     "nullptr",
    "operator",     "private",
    "protected",    "public",
    "register",     "reinterpret_cast",
    "return",       "short",
    "signed",       "sizeof",
    "static",       "static_cast",
    "struct",       "switch",
    "template",     "this",
    "throw",        "true",
    "try",          "typedef",
    "typeid",       "typename",
    "union",        "unsigned",
    "using",        "virtual",
    "void",         "volatile",
    "while",
};
const std::string_view kCppTypes[] = {
    "std",      "string",      "vector",   "map",     "set",     "shared_ptr", "unique_ptr",
    "optional", "size_t",      "int8_t",   "int16_t", "int32_t", "int64_t",    "uint8_t",
    "uint16_t", "uint32_t",    "uint64_t", "FILE",    "ostream", "istream",    "ifstream",
    "ofstream", "string_view", "function", "variant",
};

const std::string_view kPythonKeywords[] = {
    "and",  "as",     "assert", "async",  "await",  "break",   "class",    "continue", "def",
    "del",  "elif",   "else",   "except", "False",  "finally", "for",      "from",     "global",
    "if",   "import", "in",     "is",     "lambda", "None",    "nonlocal", "not",      "or",
    "pass", "raise",  "return", "True",   "try",    "while",   "with",     "yield",
};
const std::string_view kPythonTypes[] = {
    "print",     "len",  "range", "int",    "float",  "str",   "bool",
    "list",      "dict", "set",   "tuple",  "self",   "super", "open",
    "enumerate", "zip",  "map",   "filter", "sorted", "type",  "object",
};

const std::string_view kJsKeywords[] = {
    "async",      "await",   "break",  "case",     "catch", "class",  "const",   "continue",
    "debugger",   "default", "delete", "do",       "else",  "export", "extends", "false",
    "finally",    "for",     "from",   "function", "get",   "if",     "import",  "in",
    "instanceof", "let",     "new",    "null",     "of",    "return", "set",     "static",
    "super",      "switch",  "this",   "throw",    "true",  "try",    "typeof",  "undefined",
    "var",        "void",    "while",  "yield",
};
const std::string_view kJsTypes[] = {
    "console", "Object",  "Array",   "String",  "Number", "Boolean", "Promise",
    "Date",    "JSON",    "Math",    "Symbol",  "Map",    "Set",     "document",
    "window",  "process", "require", "exports", "module", "Buffer",
};

const std::string_view kRustKeywords[] = {
    "as",     "async", "await", "break", "const",  "continue", "crate", "dyn",   "else",   "enum",
    "extern", "false", "fn",    "for",   "if",     "impl",     "in",    "let",   "loop",   "match",
    "mod",    "move",  "mut",   "pub",   "ref",    "return",   "self",  "Self",  "static", "struct",
    "super",  "trait", "true",  "type",  "unsafe", "use",      "where", "while",
};
const std::string_view kRustTypes[] = {
    "Option", "Result",  "String", "Vec", "Box",     "Some",     "None",   "Ok",
    "Err",    "HashMap", "i32",    "i64", "u32",     "u64",      "usize",  "f32",
    "f64",    "bool",    "char",   "str", "println", "println!", "format", "panic",
};

const std::string_view kGoKeywords[] = {
    "break",  "case",        "chan", "const",   "continue", "default", "defer",
    "else",   "fallthrough", "for",  "func",    "go",       "goto",    "if",
    "import", "interface",   "map",  "package", "range",    "return",  "select",
    "struct", "switch",      "type", "var",
};
const std::string_view kGoTypes[] = {
    "bool", "byte",   "error", "float32", "float64", "int",     "int8",   "int16", "int32", "int64",
    "rune", "string", "uint",  "uint8",   "uint16",  "uint32",  "uint64", "nil",   "true",  "false",
    "make", "len",    "cap",   "append",  "fmt",     "Println", "Printf", "New",
};

const std::string_view kBashKeywords[] = {
    "if",   "then", "else",     "elif", "fi",    "for",    "while",  "until", "do",       "done",
    "case", "esac", "function", "in",   "local", "export", "return", "break", "continue", "echo",
    "cd",   "exit", "source",   "read", "set",   "shift",  "trap",   "unset", "alias",
};
const std::string_view kBashTypes[] = {
    "$@", "$?", "$$",    "$#",    "$0",   "$1",  "true", "false", "[[",     "]]",
    "&&", "||", "2>&1",  "sudo",  "grep", "sed", "awk",  "cat",   "ls",     "rm",
    "cp", "mv", "mkdir", "touch", "find", "git", "pip",  "npm",   "python",
};

const std::string_view kSqlKeywords[] = {
    "SELECT",      "FROM",    "WHERE",  "INSERT", "INTO",       "VALUES",  "UPDATE", "SET",
    "DELETE",      "CREATE",  "TABLE",  "DROP",   "ALTER",      "ADD",     "COLUMN", "INDEX",
    "VIEW",        "JOIN",    "INNER",  "LEFT",   "RIGHT",      "FULL",    "OUTER",  "ON",
    "AS",          "AND",     "OR",     "NOT",    "NULL",       "IS",      "IN",     "LIKE",
    "BETWEEN",     "ORDER",   "GROUP",  "BY",     "HAVING",     "LIMIT",   "OFFSET", "DISTINCT",
    "COUNT",       "SUM",     "AVG",    "MIN",    "MAX",        "PRIMARY", "KEY",    "FOREIGN",
    "REFERENCES",  "CASE",    "WHEN",   "THEN",   "ELSE",       "END",     "UNION",  "ALL",
    "EXISTS",      "DEFAULT", "UNIQUE", "CHECK",  "CONSTRAINT", "BEGIN",   "COMMIT", "ROLLBACK",
    "TRANSACTION",
};
const std::string_view kSqlTypes[] = {
    "INT",       "INTEGER", "BIGINT", "TEXT",    "VARCHAR", "CHAR", "BOOLEAN",  "DATE",
    "TIMESTAMP", "REAL",    "DOUBLE", "NUMERIC", "DECIMAL", "BLOB", "DATETIME",
};

const std::string_view kJsonKeywords[] = {"true", "false", "null"};
const std::string_view kYamlKeywords[] = {"true", "false", "null", "yes", "no", "on", "off"};

struct LangSpec {
    std::string_view line_comment;  ///< 行注释前缀（空 = 无）
    std::string_view block_begin;   ///< 块注释开始（空 = 无；仅行内识别）
    std::string_view block_end;     ///< 块注释结束
    bool hash_preproc = false;      ///< 行首 # 整行着色（cpp 预处理）
    const std::string_view* keywords = nullptr;
    std::size_t keyword_count = 0;
    const std::string_view* types = nullptr;
    std::size_t type_count = 0;
    bool sql_upper = false;  ///< SQL 关键字大小写不敏感
};

const std::string_view kEmptyView;

const LangSpec& spec_for(std::string_view lang) {
    static const LangSpec none{};
    if (lang.empty()) return none;
    if (lang == "cpp" || lang == "c" || lang == "c++" || lang == "cxx" || lang == "cc" ||
        lang == "h" || lang == "hpp") {
        static const LangSpec s{
            .line_comment = "//",
            .block_begin = "/*",
            .block_end = "*/",
            .hash_preproc = true,
            .keywords = kCppKeywords,
            .keyword_count = sizeof(kCppKeywords) / sizeof(kCppKeywords[0]),
            .types = kCppTypes,
            .type_count = sizeof(kCppTypes) / sizeof(kCppTypes[0]),
        };
        return s;
    }
    if (lang == "python" || lang == "py") {
        static const LangSpec s{
            .line_comment = "#",
            .keywords = kPythonKeywords,
            .keyword_count = sizeof(kPythonKeywords) / sizeof(kPythonKeywords[0]),
            .types = kPythonTypes,
            .type_count = sizeof(kPythonTypes) / sizeof(kPythonTypes[0]),
        };
        return s;
    }
    if (lang == "js" || lang == "javascript" || lang == "jsx" || lang == "ts" ||
        lang == "typescript" || lang == "tsx") {
        static const LangSpec s{
            .line_comment = "//",
            .block_begin = "/*",
            .block_end = "*/",
            .keywords = kJsKeywords,
            .keyword_count = sizeof(kJsKeywords) / sizeof(kJsKeywords[0]),
            .types = kJsTypes,
            .type_count = sizeof(kJsTypes) / sizeof(kJsTypes[0]),
        };
        return s;
    }
    if (lang == "rust" || lang == "rs") {
        static const LangSpec s{
            .line_comment = "//",
            .block_begin = "/*",
            .block_end = "*/",
            .keywords = kRustKeywords,
            .keyword_count = sizeof(kRustKeywords) / sizeof(kRustKeywords[0]),
            .types = kRustTypes,
            .type_count = sizeof(kRustTypes) / sizeof(kRustTypes[0]),
        };
        return s;
    }
    if (lang == "go" || lang == "golang") {
        static const LangSpec s{
            .line_comment = "//",
            .block_begin = "/*",
            .block_end = "*/",
            .keywords = kGoKeywords,
            .keyword_count = sizeof(kGoKeywords) / sizeof(kGoKeywords[0]),
            .types = kGoTypes,
            .type_count = sizeof(kGoTypes) / sizeof(kGoTypes[0]),
        };
        return s;
    }
    if (lang == "bash" || lang == "sh" || lang == "shell" || lang == "zsh" || lang == "console" ||
        lang == "shell-session") {
        static const LangSpec s{
            .line_comment = "#",
            .keywords = kBashKeywords,
            .keyword_count = sizeof(kBashKeywords) / sizeof(kBashKeywords[0]),
            .types = kBashTypes,
            .type_count = sizeof(kBashTypes) / sizeof(kBashTypes[0]),
        };
        return s;
    }
    if (lang == "sql") {
        static const LangSpec s{
            .line_comment = "--",
            .keywords = kSqlKeywords,
            .keyword_count = sizeof(kSqlKeywords) / sizeof(kSqlKeywords[0]),
            .types = kSqlTypes,
            .type_count = sizeof(kSqlTypes) / sizeof(kSqlTypes[0]),
            .sql_upper = true,
        };
        return s;
    }
    if (lang == "json") {
        static const LangSpec s{
            .keywords = kJsonKeywords,
            .keyword_count = sizeof(kJsonKeywords) / sizeof(kJsonKeywords[0]),
        };
        return s;
    }
    if (lang == "yaml" || lang == "yml") {
        static const LangSpec s{
            .line_comment = "#",
            .keywords = kYamlKeywords,
            .keyword_count = sizeof(kYamlKeywords) / sizeof(kYamlKeywords[0]),
        };
        return s;
    }
    return none;
}

bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool word_in(const std::string_view& w, const std::string_view* table, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (w == table[i]) return true;
    return false;
}

std::string upper(std::string_view w) {
    std::string s(w);
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/// @brief 分词并着色一行；返回 false 表示整行原样输出（无任何命中）
Element tokenize_line(std::string_view line, const LangSpec& spec) {
    Elements toks;
    size_t i = 0;
    const size_t n = line.size();
    std::string plain;

    auto flush_plain = [&]() {
        if (!plain.empty()) {
            toks.push_back(ftxui::text(plain));
            plain.clear();
        }
    };

    while (i < n) {
        char c = line[i];

        // 行注释：整行剩余着色后结束
        if (!spec.line_comment.empty() && line.substr(i).starts_with(spec.line_comment)) {
            flush_plain();
            toks.push_back(ftxui::color(kComment)(ftxui::text(std::string(line.substr(i)))));
            return ftxui::hbox(std::move(toks));
        }
        // 行内块注释：/* ... */ 或到行尾
        if (!spec.block_begin.empty() && line.substr(i).starts_with(spec.block_begin)) {
            flush_plain();
            size_t end = line.find(spec.block_end, i + spec.block_begin.size());
            size_t len = (end == std::string_view::npos) ? n - i : end + spec.block_end.size() - i;
            toks.push_back(ftxui::color(kComment)(ftxui::text(std::string(line.substr(i, len)))));
            i += len;
            continue;
        }
        // 字符串
        if (c == '"' || c == '\'') {
            flush_plain();
            size_t j = i + 1;
            while (j < n) {
                if (line[j] == '\\') {
                    j += 2;
                    continue;
                }
                if (line[j] == c) {
                    ++j;
                    break;
                }
                ++j;
            }
            toks.push_back(ftxui::color(kString)(ftxui::text(std::string(line.substr(i, j - i)))));
            i = j;
            continue;
        }
        // 数字
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            flush_plain();
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '.' ||
                             line[j] == '_' || line[j] == '-' || line[j] == '+')) {
                // 数字后的 '-'/'+' 多属运算符；仅在紧邻指数 e/E 时算数
                if ((line[j] == '-' || line[j] == '+') &&
                    (j == i || (line[j - 1] != 'e' && line[j - 1] != 'E')))
                    break;
                ++j;
            }
            toks.push_back(ftxui::color(kNumber)(ftxui::text(std::string(line.substr(i, j - i)))));
            i = j;
            continue;
        }
        // 标识符
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < n && is_ident_char(line[j])) ++j;
            std::string_view w = line.substr(i, j - i);
            Color color = Color::Default;
            if (spec.sql_upper) {
                std::string uw = upper(w);
                if (word_in(uw, spec.keywords, spec.keyword_count))
                    color = kKeyword;
                else if (spec.types && word_in(uw, spec.types, spec.type_count))
                    color = kType;
            } else {
                if (word_in(w, spec.keywords, spec.keyword_count))
                    color = kKeyword;
                else if (spec.types && word_in(w, spec.types, spec.type_count))
                    color = kType;
            }
            flush_plain();
            auto e = ftxui::text(std::string(w));
            if (color != Color::Default) e = ftxui::color(color)(e);
            toks.push_back(e);
            i = j;
            continue;
        }
        plain.push_back(c);
        ++i;
    }
    flush_plain();
    return ftxui::hbox(std::move(toks));
}

#ifdef WORKX_HAS_TREE_SITTER

// ============================================================================
// Tree-sitter：grammar 注册表 + parser 池 + AST 着色
// ============================================================================

/// 关键字节点 type 集合（tree-sitter 把关键字 token 的 type 命名为关键字文本本身）
const std::unordered_set<std::string_view>& keyword_types() {
    static const std::unordered_set<std::string_view> s = {
        // C/C++
        "if",
        "else",
        "for",
        "while",
        "do",
        "switch",
        "case",
        "default",
        "break",
        "continue",
        "return",
        "goto",
        "class",
        "struct",
        "union",
        "enum",
        "namespace",
        "typedef",
        "using",
        "template",
        "typename",
        "public",
        "private",
        "protected",
        "virtual",
        "override",
        "const",
        "constexpr",
        "static",
        "inline",
        "extern",
        "register",
        "volatile",
        "mutable",
        "auto",
        "decltype",
        "new",
        "delete",
        "this",
        "sizeof",
        "alignof",
        "alignas",
        "operator",
        "explicit",
        "friend",
        "noexcept",
        "throw",
        "try",
        "catch",
        "co_await",
        "co_yield",
        "co_return",
        "requires",
        "concept",
        // Python
        "def",
        "lambda",
        "import",
        "from",
        "as",
        "pass",
        "yield",
        "global",
        "nonlocal",
        "with",
        "async",
        "await",
        "in",
        "is",
        "not",
        "and",
        "or",
        "elif",
        "raise",
        "assert",
        "del",
        // Rust
        "fn",
        "let",
        "mut",
        "match",
        "impl",
        "trait",
        "pub",
        "mod",
        "crate",
        "self",
        "super",
        "where",
        "unsafe",
        "move",
        "ref",
        "use",
        "dyn",
        "loop",
        "while",
        "for",
        "if",
        "else",
        "return",
        "struct",
        "enum",
        "type",
        "const",
        "static",
        "extern",
        "async",
        "await",
        // Go
        "func",
        "var",
        "type",
        "package",
        "range",
        "chan",
        "select",
        "defer",
        "interface",
        "map",
        "go",
        "if",
        "else",
        "for",
        "switch",
        "case",
        "default",
        "break",
        "continue",
        "return",
        "goto",
        "fallthrough",
        "import",
        "const",
        "struct",
        // JS/TS
        "function",
        "var",
        "let",
        "const",
        "typeof",
        "instanceof",
        "void",
        "extends",
        "export",
        "import",
        "of",
        "get",
        "set",
        "static",
        "class",
        "new",
        "return",
        "if",
        "else",
        "for",
        "while",
        "switch",
        "case",
        "break",
        "continue",
        "throw",
        "try",
        "catch",
        "finally",
        "delete",
        "in",
        "this",
        "super",
        "default",
        "do",
        "async",
        "await",
        "yield",
        // Bash
        "then",
        "fi",
        "esac",
        "done",
        "local",
        "declare",
        "function",
        "elif",
        "until",
        "case",
        "in",
        "do",
        "select",
        "time",
    };
    return s;
}

const std::unordered_set<std::string_view>& comment_types() {
    static const std::unordered_set<std::string_view> s = {
        "comment",
        "line_comment",
        "block_comment",
        "documentation_comment",
        "documentation_comment_prefix",
        "documentation_comment_suffix",
    };
    return s;
}

const std::unordered_set<std::string_view>& string_types() {
    static const std::unordered_set<std::string_view> s = {
        "string",           "string_literal",     "char_literal",
        "raw_string",       "raw_string_literal", "string_content",
        "string_array",     "string_expression",  "indented_string_expression",
        "string_fragment",  "path_expression",    "path_fragment",
        "hpath_expression", "spath_expression",   "uri_expression",
    };
    return s;
}

const std::unordered_set<std::string_view>& number_types() {
    static const std::unordered_set<std::string_view> s = {
        "number",
        "number_literal",
        "integer",
        "float",
        "integer_literal",
        "float_literal",
        "decimal_floating_literal",
        "hex_literal",
        "octal_literal",
        "binary_literal",
        "integer_expression",
        "float_expression",
    };
    return s;
}

const std::unordered_set<std::string_view>& type_types() {
    static const std::unordered_set<std::string_view> s = {
        "primitive_type", "type_identifier", "type_specifier",
        "type_argument",  "abstract_type",   "concrete_type",
    };
    return s;
}

const std::unordered_set<std::string_view>& constant_types() {
    static const std::unordered_set<std::string_view> s = {
        "built_in_constant", "escape_sequence", "escape", "char_escape", "escape_sequence_unicode",
    };
    return s;
}

/// 按节点 type 映射颜色（不含 parent 上下文）
Color classify_by_type(std::string_view type) {
    if (keyword_types().count(type)) return kKeyword;
    if (comment_types().count(type)) return kComment;
    if (string_types().count(type)) return kString;
    if (constant_types().count(type)) return kConstant;
    if (number_types().count(type)) return kNumber;
    if (type_types().count(type)) return kType;
    return Color::Default;
}

/// identifier 节点：查 parent 上下文判断函数名 / 属性
Color classify_identifier(TSNode node) {
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent)) {
        const char* pt = ts_node_type(parent);
        std::string_view ptype(pt);
        auto ends_with = [&](const char* s) {
            const size_t n = std::strlen(s);
            return ptype.size() >= n && ptype.substr(ptype.size() - n) == s;
        };
        if (ends_with("call_expression") || ends_with("function_declarator") ||
            ends_with("method_definition") || ends_with("function_definition") ||
            ends_with("function_item") || ends_with("method_declaration") ||
            ends_with("function_declaration")) {
            return kFunction;
        }
        if (ends_with("field_access") || ends_with("member_expression") ||
            ends_with("attribute_item") || ends_with("field_declaration")) {
            return kProperty;
        }
    }
    return Color::Default;
}

struct GrammarRegistry {
    std::unordered_map<std::string, const TSLanguage*> langs;
    std::mutex mtx;

    GrammarRegistry() {
// 各 grammar 注册由 scripts/gen_ts_grammars.py 生成，无需手改
#include "render/ts_langs_reg.inc"
    }
};

GrammarRegistry& registry() {
    static GrammarRegistry inst;
    return inst;
}

/// parser 池：每种语言复用一个 TSParser，避免 ts_parser_new 开销
/// （渲染在主线程，池内 parser 不会被并发使用）
TSParser* get_parser_for_lang(const TSLanguage* lang) {
    static std::mutex pool_mtx;
    static std::unordered_map<const TSLanguage*, TSParser*> pool;
    std::lock_guard<std::mutex> lk(pool_mtx);
    auto it = pool.find(lang);
    if (it != pool.end()) return it->second;
    TSParser* p = ts_parser_new();
    if (!ts_parser_set_language(p, lang)) {
        ts_parser_delete(p);
        return nullptr;
    }
    pool[lang] = p;
    return p;
}

/// 把 markdown 代码块 lang 标签规范化为 grammar key
std::string normalize_lang(std::string_view lang) {
    size_t b = 0, e = lang.size();
    while (b < e && (lang[b] == ' ' || lang[b] == '\t')) ++b;
    while (e > b && (lang[e - 1] == ' ' || lang[e - 1] == '\t')) --e;
    std::string s(lang.substr(b, e - b));
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "python3" || s == "gyp") s = "python";
    if (s == "shell-session" || s == "console") s = "bash";
    return s;
}

struct Span {
    uint32_t start = 0;
    uint32_t end = 0;
    Color color;
};

/// DFS 收集所有叶子节点的着色区间（文档序，天然按 start 有序）
void collect_spans(TSNode node, std::vector<Span>& spans) {
    const uint32_t child_count = ts_node_child_count(node);
    if (child_count == 0) {
        const uint32_t start = ts_node_start_byte(node);
        const uint32_t end = ts_node_end_byte(node);
        if (end <= start) return;
        std::string_view type(ts_node_type(node));
        Color c = classify_by_type(type);
        if (c == Color::Default && type == "identifier") c = classify_identifier(node);
        if (c != Color::Default) spans.push_back({start, end, c});
        return;
    }
    for (uint32_t i = 0; i < child_count; ++i) collect_spans(ts_node_child(node, i), spans);
}

/// 构建单行 Element：按 spans 切分该行字节区间 [ls, le)
Element build_line_element(const std::string& code, uint32_t ls, uint32_t le,
                           const std::vector<Span>& spans, size_t& span_idx) {
    Elements toks;
    uint32_t pos = ls;
    while (pos < le) {
        // 跳过已结束的 span
        while (span_idx < spans.size() && spans[span_idx].end <= pos) ++span_idx;
        if (span_idx >= spans.size() || spans[span_idx].start >= le) {
            toks.push_back(ftxui::text(code.substr(pos, le - pos)));
            break;
        }
        const Span& s = spans[span_idx];
        if (s.start > pos) {
            toks.push_back(ftxui::text(code.substr(pos, s.start - pos)));
            pos = s.start;
        }
        const uint32_t seg_end = std::min(s.end, le);
        toks.push_back(ftxui::color(s.color)(ftxui::text(code.substr(pos, seg_end - pos))));
        pos = seg_end;
        if (s.end > le) break;  // span 跨行，下一行继续使用同一 span
    }
    if (toks.empty()) return ftxui::text("");
    return ftxui::hbox(std::move(toks));
}

/// 整块 AST 高亮：返回逐逻辑行字节级 span（供折行渲染复用）
/// 把 collect_spans 产出的全文件绝对字节区间换算为每行行首相对偏移
std::vector<std::vector<HighlightSpan>> spans_with_ts(const std::vector<std::string>& lines,
                                                      const TSLanguage* lang) {
    std::string code;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) code.push_back('\n');
        code += lines[i];
    }
    if (code.empty()) return {};

    TSParser* parser = get_parser_for_lang(lang);
    if (!parser) return {};

    TSTree* tree =
        ts_parser_parse_string(parser, nullptr, code.data(), static_cast<uint32_t>(code.size()));
    if (!tree) return {};

    std::vector<Span> spans;
    spans.reserve(64);
    collect_spans(ts_tree_root_node(tree), spans);
    ts_tree_delete(tree);

    if (spans.empty()) return {};

    std::vector<std::vector<HighlightSpan>> out;
    out.resize(lines.size());
    size_t span_idx = 0;
    uint32_t off = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const uint32_t ls = off;
        const uint32_t le = ls + static_cast<uint32_t>(lines[i].size());
        while (span_idx < spans.size() && spans[span_idx].end <= ls) ++span_idx;
        for (size_t j = span_idx; j < spans.size() && spans[j].start < le; ++j) {
            const Span& s = spans[j];
            const uint32_t a = (s.start > ls) ? s.start - ls : 0;
            const uint32_t b = (s.end <= le) ? s.end - ls : le - ls;
            if (b > a) out[i].push_back({a, b, s.color});
            if (s.end > le) {  // span 跨行，下一行继续使用同一 span
                span_idx = j;
                break;
            }
        }
        off = le + 1;  // +1 跳过 '\n'
    }
    return out;
}

/// 整块 AST 高亮：返回每行 Element
std::vector<Element> highlight_with_ts(const std::vector<std::string>& lines,
                                       const TSLanguage* lang) {
    std::string code;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) code.push_back('\n');
        code += lines[i];
    }
    if (code.empty()) return {};

    TSParser* parser = get_parser_for_lang(lang);
    if (!parser) return {};

    TSTree* tree =
        ts_parser_parse_string(parser, nullptr, code.data(), static_cast<uint32_t>(code.size()));
    if (!tree) return {};

    std::vector<Span> spans;
    spans.reserve(64);
    collect_spans(ts_tree_root_node(tree), spans);
    ts_tree_delete(tree);

    if (spans.empty()) return {};

    std::vector<Element> out;
    out.reserve(lines.size());
    size_t span_idx = 0;
    uint32_t off = 0;
    for (const auto& l : lines) {
        const uint32_t ls = off;
        const uint32_t le = ls + static_cast<uint32_t>(l.size());
        out.push_back(build_line_element(code, ls, le, spans, span_idx));
        off = le + 1;  // +1 跳过 '\n'
    }
    return out;
}

#endif  // WORKX_HAS_TREE_SITTER

}  // namespace

Element highlight_code_line(std::string_view line, std::string_view lang) {
    if (line.empty()) return ftxui::text("");
    const LangSpec& spec = spec_for(lang);
    if (spec.keywords == nullptr && spec.line_comment.empty() && spec.block_begin.empty()) {
        // 未支持的语言：原样输出（与旧行为一致）
        return ftxui::text(std::string(line));
    }
    // cpp 行首 #：整行预处理着色
    if (spec.hash_preproc && !line.empty() && line.front() == '#') {
        return ftxui::color(kPreproc)(ftxui::text(std::string(line)));
    }
    return tokenize_line(line, spec);
}

std::vector<Element> highlight_code_block(const std::vector<std::string>& lines,
                                          std::string_view lang) {
    if (lines.empty()) return {};

#ifdef WORKX_HAS_TREE_SITTER
    const std::string nlang = normalize_lang(lang);
    if (!nlang.empty()) {
        GrammarRegistry& reg = registry();
        const TSLanguage* ts_lang = nullptr;
        {
            std::lock_guard<std::mutex> lk(reg.mtx);
            auto it = reg.langs.find(nlang);
            if (it != reg.langs.end()) ts_lang = it->second;
        }
        if (ts_lang) {
            std::vector<Element> hl = highlight_with_ts(lines, ts_lang);
            if (!hl.empty()) return hl;
        }
    }
#endif  // WORKX_HAS_TREE_SITTER

    // 回退：逐行关键字高亮
    std::vector<Element> out;
    out.reserve(lines.size());
    for (const auto& l : lines) out.push_back(highlight_code_line(l, lang));
    return out;
}

std::vector<std::vector<HighlightSpan>> highlight_block_spans(const std::vector<std::string>& lines,
                                                              std::string_view lang) {
    if (lines.empty()) return {};

#ifdef WORKX_HAS_TREE_SITTER
    const std::string nlang = normalize_lang(lang);
    if (!nlang.empty()) {
        GrammarRegistry& reg = registry();
        const TSLanguage* ts_lang = nullptr;
        {
            std::lock_guard<std::mutex> lk(reg.mtx);
            auto it = reg.langs.find(nlang);
            if (it != reg.langs.end()) ts_lang = it->second;
        }
        if (ts_lang) {
            std::vector<std::vector<HighlightSpan>> hl = spans_with_ts(lines, ts_lang);
            if (!hl.empty()) return hl;
        }
    }
#else
    (void)lines;
    (void)lang;
#endif  // WORKX_HAS_TREE_SITTER

    // 空返回：无 tree-sitter 或语言无 grammar，调用方按行回退关键字高亮
    return {};
}

Element render_spans_range(std::string_view text, uint32_t byte_from, uint32_t byte_to,
                           const std::vector<HighlightSpan>& spans) {
    Elements toks;
    uint32_t pos = byte_from;
    size_t idx = 0;
    while (idx < spans.size() && spans[idx].end <= byte_from) ++idx;
    while (idx < spans.size() && spans[idx].start < byte_to) {
        const HighlightSpan& s = spans[idx];
        const uint32_t a = std::max(s.start, byte_from);
        const uint32_t b = std::min(s.end, byte_to);
        if (a > pos) toks.push_back(ftxui::text(text.substr(pos, a - pos)));
        if (b > a) toks.push_back(ftxui::color(s.color)(ftxui::text(text.substr(a, b - a))));
        pos = b;
        if (s.end >= byte_to) break;
        ++idx;
    }
    if (pos < byte_to) toks.push_back(ftxui::text(text.substr(pos, byte_to - pos)));
    if (toks.empty()) return ftxui::text("");
    return ftxui::hbox(std::move(toks));
}

}  // namespace ftxtui
