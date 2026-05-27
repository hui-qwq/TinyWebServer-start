#include "auth.hpp"
#include "../db/sql_connection_pool.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>

AuthResult register_user(std::string& username, std::string& password) {
    if(username.empty() || password.empty()) {
        return  AuthResult::InvalidInput;
    }

    Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
    MYSQL* conn = guard.get();
    if(conn == nullptr) return AuthResult::DatabaseError;

    // Prepared statement: ? 占位符的值由 mysql_stmt_bind_param 单独传入，
    // 值和 SQL 结构完全分离，无需手动转义
    const char* sql = "INSERT INTO users(username, password, salt) VALUES (?, ?, ?)";
    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return AuthResult::DatabaseError;

    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        mysql_stmt_close(stmt);
        return AuthResult::DatabaseError;
    }

    std::string salt = generate_salt();
    std::string password_hash = hash_password(password, salt);

    // 绑定三个 VARCHAR 参数: username, password_hash, salt
    MYSQL_BIND bind[3];
    std::memset(bind, 0, sizeof(bind));

    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)username.c_str();
    bind[0].buffer_length = username.size();

    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void*)password_hash.c_str();
    bind[1].buffer_length = password_hash.size();

    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void*)salt.c_str();
    bind[2].buffer_length = salt.size();

    mysql_stmt_bind_param(stmt, bind);

    AuthResult res = AuthResult::Success;
    if (mysql_stmt_execute(stmt) != 0) {
        res = (mysql_stmt_errno(stmt) == 1062) ? AuthResult::UserExists
                                               : AuthResult::DatabaseError;
    }

    mysql_stmt_close(stmt);
    return res;
}

std::string generate_token() {
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        std::memcpy(buf, &now, std::min(sizeof(now), sizeof(buf)));
    }
    std::stringstream ss;
    for (int i = 0; i < 32; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(buf[i]);
    }
    return ss.str();
}

AuthResult login_user(std::string& username, std::string& password, int* out_user_id) {
    if(username.empty() || password.empty()) {
        return  AuthResult::InvalidInput;
    }

    Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
    MYSQL* conn = guard.get();
    if(conn == nullptr) return AuthResult::DatabaseError;

    const char* sql = "SELECT id, password, salt FROM users WHERE username = ?";
    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return AuthResult::DatabaseError;

    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        mysql_stmt_close(stmt);
        return AuthResult::DatabaseError;
    }

    // 输入参数：username
    MYSQL_BIND param_bind[1];
    std::memset(param_bind, 0, sizeof(param_bind));
    param_bind[0].buffer_type = MYSQL_TYPE_STRING;
    param_bind[0].buffer = (void*)username.c_str();
    param_bind[0].buffer_length = username.size();

    mysql_stmt_bind_param(stmt, param_bind);

    if (mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return AuthResult::DatabaseError;
    }

    int db_id = 0;
    char db_password[65] = {0};
    char db_salt[65]   = {0};
    bool id_null = 0, pw_null = 0, salt_null = 0;
    unsigned long pw_len = 0, salt_len = 0;

    MYSQL_BIND result_bind[3];
    std::memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_LONG;
    result_bind[0].buffer = &db_id;
    result_bind[0].is_null = &id_null;

    result_bind[1].buffer_type = MYSQL_TYPE_STRING;
    result_bind[1].buffer = db_password;
    result_bind[1].buffer_length = sizeof(db_password) - 1;
    result_bind[1].length = &pw_len;
    result_bind[1].is_null = &pw_null;

    result_bind[2].buffer_type = MYSQL_TYPE_STRING;
    result_bind[2].buffer = db_salt;
    result_bind[2].buffer_length = sizeof(db_salt) - 1;
    result_bind[2].length = &salt_len;
    result_bind[2].is_null = &salt_null;

    mysql_stmt_bind_result(stmt, result_bind);

    AuthResult res = AuthResult::WrongUserOrPassword;
    if (mysql_stmt_fetch(stmt) == 0 && !id_null && !pw_null && !salt_null) {
        std::string stored_hash(db_password, pw_len);
        std::string stored_salt(db_salt, salt_len);
        std::string computed = hash_password(password, stored_salt);

        if (stored_hash == computed) {
            if (out_user_id) *out_user_id = db_id;
            res = AuthResult::Success;
        }
    }

    mysql_stmt_close(stmt);
    return res;
}

std::string get_form_value(const std::string& body,
                        const std::string& key) {
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t amp = body.find('&', pos);
        std::string part = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);

        size_t eq = part.find('=');
        if (eq != std::string::npos) {
            std::string k = part.substr(0, eq);
            std::string v = part.substr(eq + 1);
            if (k == key) return v;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }

    return "";
}

std::string hash_password(const std::string& password, const std::string& salt) {
    // PBKDF2-HMAC-SHA256, 10 万次迭代
    // 暴力破解成本远高于单次 SHA-256
    constexpr int iterations = 100000;
    constexpr int hash_len = 32;
    unsigned char hash[hash_len];

    PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                      reinterpret_cast<const unsigned char*>(salt.c_str()),
                      static_cast<int>(salt.size()),
                      iterations,
                      EVP_sha256(),
                      hash_len, hash);

    std::stringstream ss;
    for (int i = 0; i < hash_len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(hash[i]);
    }
    return ss.str();
}

std::string generate_salt() {
    // 生成 32 字节的密码学安全随机数，转为 64 字符的十六进制字符串
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        std::memcpy(buf, &now, std::min(sizeof(now), sizeof(buf)));
    }

    std::stringstream ss;
    for (int i = 0; i < 32; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(buf[i]);
    }
    return ss.str();
}
