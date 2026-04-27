#include "auth.hpp"
#include "../db/sql_connection_pool.hpp"
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>

AuthResult register_user(std::string& username, std::string& password) {
    if(username.empty() || password.empty()) {
        return  AuthResult::InvalidInput;
    }

    Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
    MYSQL* conn = guard.get();

    if(conn == nullptr) {
        return AuthResult::DatabaseError;
    }

    std::string safe_username = mysql_escape(conn, username);
    std::string password_hash = hash_password(password);
    std::string safe_password = mysql_escape(conn, password_hash);
    std::string sqlqy = "INSERT INTO users(username, password) VALUES ('" +
                        safe_username + "','" + safe_password + "')";

    if (mysql_query(conn, sqlqy.c_str()) != 0) {
        if (mysql_errno(conn) == 1062) {
            return AuthResult::UserExists;
        } else {
            return AuthResult::DatabaseError;
        }
    }
    return AuthResult::Success;
}

AuthResult login_user(std::string& username, std::string& password) {
    if(username.empty() || password.empty()) {
        return  AuthResult::InvalidInput;
    }

    Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
    MYSQL* conn = guard.get();

    if(conn == nullptr) return AuthResult::DatabaseError;
    
    std::string safe_username = mysql_escape(conn, username);
    std::string sqlqy = "SELECT password FROM users WHERE username='" + safe_username + "'";

    if (mysql_query(conn, sqlqy.c_str()) != 0) {
        return AuthResult::DatabaseError;
    }

    MYSQL_RES* result = mysql_store_result(conn);
    if(result == nullptr) return AuthResult::DatabaseError;

    MYSQL_ROW row = mysql_fetch_row(result);
    bool login_ok = false;

    if (row != nullptr && row[0] != nullptr) {
        std::string db_password = row[0];
        std::string password_hash = hash_password(password);

        login_ok = (db_password == password_hash);
    }

    mysql_free_result(result);

    if(login_ok) {
        return AuthResult::Success;
    }
    return AuthResult::WrongUserOrPassword;
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

std::string mysql_escape(MYSQL* conn, const std::string& input) {
    std::string out;
    out.resize(input.size() * 2 + 1);

    unsigned long len = mysql_real_escape_string(
        conn,
        out.data(),
        input.c_str(),
        static_cast<unsigned long>(input.size())
    );

    out.resize(len);
    return out;
}

std::string hash_password(const std::string& password) {
    unsigned char hash[SHA256_DIGEST_LENGTH];

    SHA256(reinterpret_cast<const unsigned char*>(password.c_str()),
           password.size(),
           hash);

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(hash[i]);
    }

    return ss.str();
}
