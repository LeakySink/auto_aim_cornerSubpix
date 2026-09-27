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

# hikrobot：工程内 so 只用于链接期找符号，不写入 RPATH/RUNPATH。
# 运行时由环境（bashrc 里通常是 /opt/MVS/lib/...）提供 libMvCameraControl.so，换机只改环境即可。
target_include_directories(io PUBLIC hikrobot/include)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  set(_hik_libdir "${CMAKE_CURRENT_SOURCE_DIR}/hikrobot/lib/amd64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
  set(_hik_libdir "${CMAKE_CURRENT_SOURCE_DIR}/hikrobot/lib/arm64")
else()
  message(FATAL_ERROR "Unsupported architecture: ${CMAKE_HOST_SYSTEM_PROCESSOR}!")
endif()
target_link_options(io INTERFACE
  "LINKER:-L${_hik_libdir}"
  "LINKER:-rpath-link,${_hik_libdir}")

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
