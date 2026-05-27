#include "../http/template.hpp"
#include "../http/auth.hpp"

#include <cassert>
#include <iostream>
#include <string>

static int passed = 0;
static int failed = 0;

void check(const char* name, bool cond) {
    if (cond) {
        std::cout << "  PASS " << name << '\n';
        ++passed;
    } else {
        std::cout << "  FAIL " << name << '\n';
        ++failed;
    }
}

// ==================== url_decode ====================
void test_url_decode() {
    std::cout << "[url_decode]\n";
    check("empty",           url_decode("") == "");
    check("no encoding",     url_decode("hello") == "hello");
    check("plus to space",   url_decode("hello+world") == "hello world");
    check("%20 to space",    url_decode("hello%20world") == "hello world");
    check("multiple %XX",    url_decode("%48%65%6C%6C%6F") == "Hello");
    check("chinese UTF-8",   url_decode("%E4%B8%AD%E6%96%87") == "中文");
    check("mixed",           url_decode("a%3Db+%26+c") == "a=b & c");
    check("incomplete %XX",  url_decode("%2") == "%2");       // 不完整，保持原样
    check("invalid hex",     url_decode("%GG") == "%GG");     // 非法 hex
}

// ==================== html_escape ====================
void test_html_escape() {
    std::cout << "[html_escape]\n";
    check("no special",      html_escape("hello") == "hello");
    check("ampersand",       html_escape("a & b") == "a &amp; b");
    check("less than",       html_escape("a < b") == "a &lt; b");
    check("greater than",    html_escape("a > b") == "a &gt; b");
    check("double quote",    html_escape("say \"hi\"") == "say &quot;hi&quot;");
    check("single quote",    html_escape("it's") == "it&#39;s");
    check("all together",    html_escape("<script>&\"'") == "&lt;script&gt;&amp;&quot;&#39;");
    check("empty",           html_escape("") == "");
}

// ==================== escape_json ====================
void test_escape_json() {
    std::cout << "[escape_json]\n";
    check("no special",      escape_json("hello") == "hello");
    check("double quote",    escape_json("say \"hi\"") == "say \\\"hi\\\"");
    check("backslash",       escape_json("a\\b") == "a\\\\b");
    check("newline",         escape_json("a\nb") == "a\\nb");
    check("tab",             escape_json("a\tb") == "a\\tb");
    check("carriage return", escape_json("a\rb") == "a\\rb");
    check("markdown content", escape_json("## Title\n\n```cpp\ncode\n```") ==
                                    "## Title\\n\\n```cpp\\ncode\\n```");
}

// ==================== replace_placeholder ====================
void test_replace_placeholder() {
    std::cout << "[replace_placeholder]\n";
    {
        std::string s = "Hello {{NAME}}!";
        replace_placeholder(s, "NAME", "World");
        check("single", s == "Hello World!");
    }
    {
        std::string s = "{{A}} and {{A}}";
        replace_placeholder(s, "A", "X");
        check("multiple same key", s == "X and X");
    }
    {
        std::string s = "{{TITLE}} by {{AUTHOR}}";
        replace_placeholder(s, "TITLE", "My Post");
        replace_placeholder(s, "AUTHOR", "Tom");
        check("multiple different keys", s == "My Post by Tom");
    }
    {
        std::string s = "no placeholder";
        replace_placeholder(s, "X", "Y");
        check("no match", s == "no placeholder");
    }
    {
        std::string s = "";
        replace_placeholder(s, "X", "Y");
        check("empty string", s == "");
    }
}

// ==================== get_form_value ====================
void test_get_form_value() {
    std::cout << "[get_form_value]\n";
    check("first field",     get_form_value("a=1&b=2", "a") == "1");
    check("second field",    get_form_value("a=1&b=2", "b") == "2");
    check("missing",         get_form_value("a=1&b=2", "c") == "");
    check("empty value",     get_form_value("a=&b=2", "a") == "");
    check("single field",    get_form_value("key=value", "key") == "value");
    check("empty body",      get_form_value("", "a") == "");
    check("value with eq",   get_form_value("a=b=c&d=1", "a") == "b=c");
}

// ==================== hash_password (PBKDF2) ====================
void test_hash_password() {
    std::cout << "[hash_password]\n";
    std::string salt = generate_salt();
    check("salt not empty",  !salt.empty());
    check("salt 64 chars",   salt.size() == 64);

    std::string h1 = hash_password("mypassword", salt);
    std::string h2 = hash_password("mypassword", salt);
    check("deterministic",   h1 == h2);
    check("64 char output",  h1.size() == 64);

    std::string h3 = hash_password("different", salt);
    check("different password", h1 != h3);

    std::string salt2 = generate_salt();
    std::string h4 = hash_password("mypassword", salt2);
    check("different salt",  h1 != h4);
}

// ==================== generate_token ====================
void test_generate_token() {
    std::cout << "[generate_token]\n";
    std::string t1 = generate_token();
    std::string t2 = generate_token();
    check("64 char token",   t1.size() == 64);
    check("random enough",   t1 != t2);
    check("hex only",        t1.find_first_not_of("0123456789abcdef") == std::string::npos);
}

// ==================== generate_salt ====================
void test_generate_salt() {
    std::cout << "[generate_salt]\n";
    std::string s = generate_salt();
    check("64 char salt",    s.size() == 64);
    check("hex only",        s.find_first_not_of("0123456789abcdef") == std::string::npos);
}

int main() {
    std::cout << "=== Running Unit Tests ===\n\n";

    test_url_decode();
    test_html_escape();
    test_escape_json();
    test_replace_placeholder();
    test_get_form_value();
    test_hash_password();
    test_generate_token();
    test_generate_salt();

    std::cout << "\n=== Results: " << passed << " passed, "
              << failed << " failed ===\n";
    return failed > 0 ? 1 : 0;
}
