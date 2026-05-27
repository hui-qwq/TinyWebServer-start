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

AuthResult login_user(std::string& username, std::string& password, int* out_user_id = nullptr);

std::string get_form_value(const std::string& body,
                           const std::string& key);

std::string generate_token();
std::string hash_password(const std::string& password, const std::string& salt);
std::string generate_salt();
