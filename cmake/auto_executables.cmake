# cmake/auto_executables.cmake
#
# 自动把目录下每个 *.cpp 编成同名可执行文件。
# 使用 file(GLOB CONFIGURE_DEPENDS)：增删 .cpp 会触发重新配置，一般无需改 CMakeLists。
#
# 用法（在 src/ / tests/ / calibration/ 的 CMakeLists.txt）：
#
#   include(${PROJECT_SOURCE_DIR}/cmake/auto_executables.cmake)
#   auto_add_executables(
#     [DIR path]                 # 默认 CMAKE_CURRENT_SOURCE_DIR
#     [EXCLUDE file.cpp ...]     # 不做成可执行文件（如库源码 calibrator.cpp）
#     DEFAULT_LIBS lib1 lib2 ...
#   )
#
# 命名约定（按文件名 stem，无需改 CMake）：
#   calibrate / calibrate_*     → 额外编译 calibration/calibrator.cpp
#   sentry* / publish_test /
#   subscribe_test / topic_loop_test → 仅 USE_ROS2=ON 时生成，并 ros2_use()
#   ros2_*                      → USE_ROS2 或 (rclcpp+sensor_msgs) 时生成；链 sensor_msgs
#   *buff* 且 *debug*           → 追加 CERES_LIBRARIES（若已定义）
#   sentry*                     → 追加 omniperception
#   multi_sender_test + fake_robot 同时存在 → add_dependencies(multi_sender_test fake_robot)

function(auto_add_executables)
  cmake_parse_arguments(AA "" "DIR" "EXCLUDE;DEFAULT_LIBS" ${ARGN})

  if(NOT AA_DIR)
    set(AA_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()

  # ros2_* 软依赖：即使 USE_ROS2=OFF（无 sp_msgs）也可单独找 rclcpp + sensor_msgs
  if(NOT rclcpp_FOUND OR NOT sensor_msgs_FOUND)
    if(DEFINED ENV{ROS_DISTRO} AND EXISTS "/opt/ros/$ENV{ROS_DISTRO}")
      list(PREPEND CMAKE_PREFIX_PATH "/opt/ros/$ENV{ROS_DISTRO}")
    elseif(EXISTS "/opt/ros/humble")
      list(PREPEND CMAKE_PREFIX_PATH "/opt/ros/humble")
    endif()
    find_package(rclcpp QUIET)
    find_package(sensor_msgs QUIET)
  endif()

  set(_calibrator_src "${PROJECT_SOURCE_DIR}/calibration/calibrator.cpp")

  file(GLOB _srcs CONFIGURE_DEPENDS "${AA_DIR}/*.cpp")
  list(SORT _srcs)

  set(_created "")
  set(_skipped "")

  foreach(_src IN LISTS _srcs)
    get_filename_component(_basename "${_src}" NAME)
    get_filename_component(_name "${_src}" NAME_WE)

    if(_basename IN_LIST AA_EXCLUDE)
      list(APPEND _skipped "${_name}(exclude)")
      continue()
    endif()

    set(_sources "${_src}")
    set(_libs ${AA_DEFAULT_LIBS})
    set(_ros2_full FALSE)   # 需要 USE_ROS2 + ros2_use
    set(_ros2_image FALSE)  # ros2_*：可用软路径

    if(_name STREQUAL "calibrate" OR _name MATCHES "^calibrate_")
      if(EXISTS "${_calibrator_src}")
        list(APPEND _sources "${_calibrator_src}")
      endif()
    endif()

    if(_name MATCHES "^ros2_")
      set(_ros2_image TRUE)
    elseif(
      _name MATCHES "^sentry" OR
      _name STREQUAL "publish_test" OR
      _name STREQUAL "subscribe_test" OR
      _name STREQUAL "topic_loop_test")
      set(_ros2_full TRUE)
    endif()

    if(_ros2_image)
      if(USE_ROS2)
        # ok
      elseif(rclcpp_FOUND AND sensor_msgs_FOUND)
        # soft path
      else()
        list(APPEND _skipped "${_name}(no ros2/sensor_msgs)")
        continue()
      endif()
    elseif(_ros2_full)
      if(NOT USE_ROS2)
        list(APPEND _skipped "${_name}(USE_ROS2=OFF)")
        continue()
      endif()
    endif()

    add_executable(${_name} ${_sources})

    if(_name MATCHES "^sentry")
      list(APPEND _libs omniperception)
    endif()

    if(_name MATCHES "buff" AND _name MATCHES "debug")
      if(DEFINED CERES_LIBRARIES)
        list(APPEND _libs ${CERES_LIBRARIES})
      endif()
    endif()

    target_link_libraries(${_name} ${_libs})

    if(_ros2_image)
      if(USE_ROS2)
        ros2_use(${_name})
        if(NOT sensor_msgs_FOUND)
          find_package(sensor_msgs REQUIRED)
        endif()
        target_link_libraries(${_name} sensor_msgs::sensor_msgs)
      else()
        target_link_libraries(${_name} rclcpp::rclcpp sensor_msgs::sensor_msgs)
      endif()
    elseif(_ros2_full)
      ros2_use(${_name})
    endif()

    list(APPEND _created ${_name})
  endforeach()

  if("multi_sender_test" IN_LIST _created AND "fake_robot" IN_LIST _created)
    add_dependencies(multi_sender_test fake_robot)
  endif()

  get_filename_component(_dir_label "${AA_DIR}" NAME)
  message(STATUS "auto_exec[${_dir_label}]: ${_created}")
  if(_skipped)
    message(STATUS "auto_exec[${_dir_label}] skip: ${_skipped}")
  endif()
endfunction()
