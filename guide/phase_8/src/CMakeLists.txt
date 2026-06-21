cmake_minimum_required(VERSION 3.10)
project(Phase8_WebServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 MySQL 客户端库
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h)
find_library(MYSQL_LIBRARY mysqlclient)

include_directories(${CMAKE_SOURCE_DIR} ${MYSQL_INCLUDE_DIR})

add_executable(server
    main.cpp
    webserver.cpp
    config.cpp
    http_conn.cpp
    lst_timer.cpp
    log.cpp
    sql_connection_pool.cpp
)
target_link_libraries(server pthread ${MYSQL_LIBRARY})
