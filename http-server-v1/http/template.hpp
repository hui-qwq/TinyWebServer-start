#pragma once

#include <cctype>
#include <string>

// HTML 实体转义：< → &lt;, " → &quot; 等
inline std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

// URL 解码：%20 → 空格, + → 空格, %E4%B8%AD → 中
inline std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            char hex[3] = {s[i + 1], s[i + 2], '\0'};
            out += static_cast<char>(std::strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// 将模板中所有 {{KEY}} 替换为 value
inline void replace_placeholder(std::string& tmpl,
                                const std::string& key,
                                const std::string& value) {
    std::string marker = "{{" + key + "}}";
    size_t pos = 0;
    while ((pos = tmpl.find(marker, pos)) != std::string::npos) {
        tmpl.replace(pos, marker.size(), value);
        pos += value.size();
    }
}

// 将 Markdown 字符串转为安全的 JSON 字符串（仅处理双引号、反斜杠和换行）
inline std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 10);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}
