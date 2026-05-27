cmake_minimum_required(VERSION 3.10)
project(tinywebserver VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")

# MySQL
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED mysqlclient)

# OpenSSL (PBKDF2, random)
find_package(OpenSSL REQUIRED)

# pthread
find_package(Threads REQUIRED)

# ---- Core library (shared between server and tests) ----
add_library(tinycore STATIC
    http/auth.cpp
    db/sql_connection_pool.cpp
    logger/logger.cpp
)

target_include_directories(tinycore PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${MYSQL_INCLUDE_DIRS}
)
target_link_libraries(tinycore PUBLIC
    ${MYSQL_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

# ---- Server ----
add_executable(server
    app/main.cpp
    webserver/webserver.cpp
    http/http_conn.cpp
    thread_pool/thread_pool.cpp
)

target_link_libraries(server tinycore)

# ---- Tests ----
add_executable(test_utils
    tests/test_utils.cpp
)

target_link_libraries(test_utils tinycore)

add_executable(mysql_pool_test
    tests/mysql_pool_test.cpp
)

target_link_libraries(mysql_pool_test tinycore)

# ---- CTest integration ----
enable_testing()
add_test(NAME test_utils COMMAND test_utils)
