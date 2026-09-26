cmake_minimum_required(VERSION 3.16)

find_package(yaml-cpp REQUIRED)

add_subdirectory(serial)

# 创建目标 io
add_library(io STATIC 
    hikrobot/hikrobot.cpp    
    mindvision/mindvision.cpp  
    usbcamera/usbcamera.cpp  
    camera.cpp
    cboard.cpp
    dm_imu/dm_imu.cpp
    gimbal/gimbal.cpp
)

# hikrobot：优先系统 MVS（与官方客户端同库），避免工程内旧 so 导致 EnumDevices=MV_E_RESOURCE
target_include_directories(io PUBLIC hikrobot/include)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  set(_hik_bundled "${CMAKE_CURRENT_SOURCE_DIR}/hikrobot/lib/amd64")
  set(_hik_mvs "/opt/MVS/lib/64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
  set(_hik_bundled "${CMAKE_CURRENT_SOURCE_DIR}/hikrobot/lib/arm64")
  set(_hik_mvs "/opt/MVS/lib/aarch64")
else()
  message(FATAL_ERROR "Unsupported architecture: ${CMAKE_HOST_SYSTEM_PROCESSOR}!")
endif()
if(EXISTS "${_hik_mvs}/libMvCameraControl.so")
  set(HIKROBOT_LIB_DIR "${_hik_mvs}")
  message(STATUS "HikRobot SDK: ${HIKROBOT_LIB_DIR} (system MVS)")
else()
  set(HIKROBOT_LIB_DIR "${_hik_bundled}")
  message(STATUS "HikRobot SDK: ${HIKROBOT_LIB_DIR} (bundled)")
endif()
set(HIKROBOT_LIB_DIR "${HIKROBOT_LIB_DIR}" PARENT_SCOPE)
target_link_directories(io PUBLIC "${HIKROBOT_LIB_DIR}")
# 运行时也要能找到该 so（勿依赖手工 LD_LIBRARY_PATH）
set_property(TARGET io APPEND PROPERTY INTERFACE_LINK_OPTIONS
  "LINKER:--disable-new-dtags"
  "LINKER:-rpath,${HIKROBOT_LIB_DIR}")

# mindvision
target_include_directories(io PUBLIC mindvision/include)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  target_link_directories(io PUBLIC mindvision/lib/amd64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
  target_link_directories(io PUBLIC mindvision/lib/arm64)
else()
  message(FATAL_ERROR "Unsupported architecture: ${CMAKE_HOST_SYSTEM_PROCESSOR}!")
endif()

target_link_libraries(io MvCameraControl MVSDK usb-1.0 yaml-cpp serial)

add_subdirectory(ros2)
