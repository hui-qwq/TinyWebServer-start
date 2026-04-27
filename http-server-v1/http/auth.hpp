#pragma once

#include <mysql/mysql.h>

#include <string>

enum class AuthResult {
    Success,
    InvalidInput,
    UserExists,
    WrongUserOrPassword,
    DatabaseError
};

AuthResult register_user(std::string& username, std::string& password);

AuthResult login_user(std::string& username, std::string& password);

std::string get_form_value(const std::string& body,
                           const std::string& key);

std::string mysql_escape(MYSQL* conn, const std::string& input);

std::string hash_password(const std::string& password);
