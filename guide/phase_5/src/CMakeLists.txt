cmake_minimum_required(VERSION 3.10)
project(Phase5_Timer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR})

add_library(timer_lib STATIC
    lst_timer.cpp
)

add_executable(test_timer test_timer.cpp)
target_link_libraries(test_timer timer_lib pthread)
