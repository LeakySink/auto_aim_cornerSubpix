cmake_minimum_required(VERSION 3.16.3)

project(sp_vision)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Release)
message(STATUS "--------------------CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}--------------------")

find_package(OpenCV REQUIRED)
find_package(fmt REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
set(OpenVINO_DIR "/opt/intel/openvino_2024.6.0/runtime/cmake/")
find_package(OpenVINO REQUIRED)

include_directories(${EIGEN3_INCLUDE_DIR})
include_directories(${OpenCV_INCLUDE_DIRS})
include_directories(${PROJECT_SOURCE_DIR})

################## ROS2 ##################
# ① init → ② find messages → ③ setup interfaces → (子目录 attach) → ④ finalize
include(${PROJECT_SOURCE_DIR}/cmake/ros2.cmake)
ros2_init()
ros2_find_messages()
ros2_setup_interfaces()

add_subdirectory(tools)
add_subdirectory(io)
add_subdirectory(tasks/auto_aim)
add_subdirectory(tasks/auto_buff)
add_subdirectory(tasks/omniperception)

add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(calibration)

add_executable(diagnose_hik_camera io/hikrobot/diagnose_camera.cpp)
target_include_directories(diagnose_hik_camera PRIVATE io/hikrobot/include)
target_link_directories(diagnose_hik_camera PRIVATE io/hikrobot/lib/amd64)
target_link_libraries(diagnose_hik_camera ${OpenCV_LIBS} MvCameraControl usb-1.0)

ros2_finalize()
