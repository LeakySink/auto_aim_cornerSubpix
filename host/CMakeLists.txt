cmake_minimum_required(VERSION 3.16)
project(udp_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Release)

find_package(nlohmann_json REQUIRED)

add_executable(udp_backend
  src/main.cpp
  src/backend/udp_receiver.cpp
)

target_include_directories(udp_backend PRIVATE src)
target_link_libraries(udp_backend PRIVATE nlohmann_json::nlohmann_json)
