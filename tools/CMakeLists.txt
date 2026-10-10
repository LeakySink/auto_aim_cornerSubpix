cmake_minimum_required(VERSION 3.16)

find_package(yaml-cpp REQUIRED)

add_library(tools OBJECT 
    exiter.cpp
    extended_kalman_filter.cpp
    ransac_sine_fitter.cpp
    img_tools.cpp
    math_tools.cpp
    remote_logger.cpp
    rdbg/transport.cpp
    rdbg/control.cpp
    rdbg/data.cpp
    rdbg/session.cpp
    rdbg/image.cpp
    rdbg/engine.cpp
    rdbg/tf_auto_pub.cpp
    trajectory.cpp
    pid.cpp
    crc.cpp
)

target_link_libraries(tools PUBLIC yaml-cpp)