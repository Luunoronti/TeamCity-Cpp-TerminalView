cmake_minimum_required(VERSION 3.15)
project(teamcity_ticker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(teamcity_ticker
  src/main.cpp
  third_party/httplib.h
  third_party/json.hpp
)

# Threads for Linux/Unix
find_package(Threads REQUIRED)
target_link_libraries(teamcity_ticker PRIVATE Threads::Threads)

# Windows: link winsock2 for httplib
if (WIN32)
  target_link_libraries(teamcity_ticker PRIVATE ws2_32)
endif()

# Make headers visible
target_include_directories(teamcity_ticker PRIVATE third_party)