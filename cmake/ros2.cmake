# cmake/ros2.cmake
#
# 拆分后的调用顺序（顶层 CMakeLists.txt）：
#
#   include(${PROJECT_SOURCE_DIR}/cmake/ros2.cmake)
#   ros2_init()                          # ① 探测系统 ROS 是否可用 → USE_ROS2
#   ros2_find_messages()                 # ② 扫描本地消息包 + 系统 sp_msgs/std_msgs
#   ros2_setup_interfaces()              # ③ 注册编译期目标 ros2_msgs（有本地包时）
#   add_subdirectory(...)                # 子目录里用 ros2_use
#   ros2_finalize()                      # ④ 收尾检查 + 状态汇总
#
# 子 CMake（如 io/ros2）用法：
#   if(USE_ROS2)
#     ros2_use(io)                       # rclcpp + std_msgs/sp_msgs + 本地消息包
#   endif()
#
# 变量（配置后可在子目录读取）：
#   USE_ROS2, ROS2_ROOT, ROS2_WS, ROS2_MSG_ROOT
#   ROS2_MSG_PACKAGES, ROS2_MSG_DIRS, ROS2_HAS_SYSTEM_MSGS
#
# 时机：配置阶段只探测/注册；colcon 在编译阶段由目标 ros2_msgs 触发。
# 注意：以下均为 macro（find_package 结果需留在调用方）；macro 内不用 return()。

# ---------------------------------------------------------------------------
# 内部工具
# ---------------------------------------------------------------------------
function(_ros2_read_pkg_name package_xml out_var)
  file(READ "${package_xml}" _xml)
  if(_xml MATCHES "<name>[ \t\r\n]*([^<]+)[ \t\r\n]*</name>")
    string(STRIP "${CMAKE_MATCH_1}" _name)
    set(${out_var} "${_name}" PARENT_SCOPE)
  else()
    set(${out_var} "" PARENT_SCOPE)
  endif()
endfunction()

macro(_ros2_discover_msgs msg_root)
  set(ROS2_MSG_DIRS "")
  set(ROS2_MSG_PACKAGES "")
  if(IS_DIRECTORY "${msg_root}")
    file(GLOB _ros2_cand LIST_DIRECTORIES true "${msg_root}/*")
    foreach(_ros2_dir IN LISTS _ros2_cand)
      if(IS_DIRECTORY "${_ros2_dir}" AND EXISTS "${_ros2_dir}/package.xml")
        _ros2_read_pkg_name("${_ros2_dir}/package.xml" _ros2_name)
        if(_ros2_name STREQUAL "")
          get_filename_component(_ros2_name "${_ros2_dir}" NAME)
          message(WARNING "ros2: ${_ros2_dir}/package.xml 无 <name>，回退用目录名 '${_ros2_name}'")
        endif()
        list(APPEND ROS2_MSG_DIRS "${_ros2_dir}")
        list(APPEND ROS2_MSG_PACKAGES "${_ros2_name}")
        message(STATUS "ros2: 发现消息包 ${_ros2_name} ← ${_ros2_dir}")
      endif()
    endforeach()
  endif()
endmacro()

# ---------------------------------------------------------------------------
# ① 初始化：探测系统 ROS 是否存在
# ---------------------------------------------------------------------------
macro(ros2_init)
  set(ROS2_ROOT "")
  set(ROS2_WS "${CMAKE_BINARY_DIR}/ros2_ws")
  set(ROS2_MSG_ROOT "")
  set(ROS2_MSG_PACKAGES "")
  set(ROS2_MSG_DIRS "")
  set(ROS2_HAS_SYSTEM_MSGS FALSE)

  if(DEFINED USE_ROS2 AND NOT USE_ROS2)
    set(USE_ROS2 FALSE)
    message(STATUS "ros2_init: USE_ROS2=OFF（命令行指定）")
  else()
    if(DEFINED ENV{ROS_DISTRO} AND EXISTS "/opt/ros/$ENV{ROS_DISTRO}/setup.bash")
      set(ROS2_ROOT "/opt/ros/$ENV{ROS_DISTRO}")
    elseif(EXISTS "/opt/ros/humble/setup.bash")
      set(ROS2_ROOT "/opt/ros/humble")
    elseif(EXISTS "/opt/ros/jazzy/setup.bash")
      set(ROS2_ROOT "/opt/ros/jazzy")
    endif()

    set(_ros2_env_ok FALSE)
    if(ROS2_ROOT)
      list(PREPEND CMAKE_PREFIX_PATH "${ROS2_ROOT}")
      if(NOT DEFINED ENV{AMENT_PREFIX_PATH} OR NOT "$ENV{AMENT_PREFIX_PATH}" MATCHES "${ROS2_ROOT}")
        set(ENV{AMENT_PREFIX_PATH} "${ROS2_ROOT}:$ENV{AMENT_PREFIX_PATH}")
      endif()
      execute_process(
        COMMAND python3 -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')"
        OUTPUT_VARIABLE _ros2_pyver OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      set(_ros2_py "${ROS2_ROOT}/lib/python${_ros2_pyver}/site-packages")
      if(EXISTS "${_ros2_py}")
        set(ENV{PYTHONPATH} "${_ros2_py}:$ENV{PYTHONPATH}")
      endif()

      set(_ros2_ws_cmake "${ROS2_WS}/cmake")
      file(MAKE_DIRECTORY "${_ros2_ws_cmake}")
      foreach(_ros2_ament_dir
          ament_cmake_package_templates
          ament_cmake_core
          ament_cmake_uninstall_target)
        set(_ros2_link "${CMAKE_CURRENT_BINARY_DIR}/${_ros2_ament_dir}")
        set(_ros2_real "${_ros2_ws_cmake}/${_ros2_ament_dir}")
        if(EXISTS "${_ros2_link}" AND NOT IS_SYMLINK "${_ros2_link}")
          file(REMOVE_RECURSE "${_ros2_link}")
        endif()
        if(NOT EXISTS "${_ros2_link}")
          file(MAKE_DIRECTORY "${_ros2_real}")
          file(CREATE_LINK "${_ros2_real}" "${_ros2_link}" SYMBOLIC)
        endif()
      endforeach()

      execute_process(
        COMMAND python3 -c "import ament_package"
        RESULT_VARIABLE _ros2_ament_ok OUTPUT_QUIET ERROR_QUIET)
      find_program(_ros2_colcon colcon)
      if(_ros2_ament_ok EQUAL 0 AND _ros2_colcon)
        find_package(ament_cmake QUIET)
        find_package(rclcpp QUIET)
        find_package(rosidl_typesupport_cpp QUIET)
        if(ament_cmake_FOUND AND rclcpp_FOUND AND rosidl_typesupport_cpp_FOUND)
          set(_ros2_env_ok TRUE)
        endif()
      endif()
    endif()

    # 记录是否命令行强制开启（供后续无消息包时 FATAL）
    set(ROS2_FORCE_ON FALSE)
    if(DEFINED USE_ROS2 AND USE_ROS2)
      set(ROS2_FORCE_ON TRUE)
    endif()

    if(ROS2_FORCE_ON)
      if(NOT ROS2_ROOT)
        message(FATAL_ERROR "ros2_init: -DUSE_ROS2=ON 但未找到 /opt/ros/<distro>")
      endif()
      find_program(_ros2_colcon colcon)
      if(NOT _ros2_colcon)
        message(FATAL_ERROR "ros2_init: -DUSE_ROS2=ON 但未找到 colcon")
      endif()
      find_package(ament_cmake REQUIRED)
      find_package(rclcpp REQUIRED)
      find_package(rosidl_typesupport_cpp REQUIRED)
      set(USE_ROS2 TRUE)
      message(STATUS "ros2_init: USE_ROS2=ON（命令行强制），ROS2_ROOT=${ROS2_ROOT}")
    elseif(_ros2_env_ok)
      find_package(ament_cmake REQUIRED)
      find_package(rclcpp REQUIRED)
      find_package(rosidl_typesupport_cpp REQUIRED)
      set(USE_ROS2 TRUE)
      message(STATUS "ros2_init: 检测到 ROS2（${ROS2_ROOT}），USE_ROS2=ON")
    else()
      set(USE_ROS2 FALSE)
      message(STATUS "ros2_init: 未检测到完整 ROS2 环境，USE_ROS2=OFF")
    endif()
  endif()
endmacro()

# ---------------------------------------------------------------------------
# ② 设置消息组件搜索目录并扫描（子目录可读 ROS2_MSG_*）
#    本分支 io 桥接依赖系统包 std_msgs + sp_msgs；本地 io/ros2/msg 可选。
# ---------------------------------------------------------------------------
macro(ros2_find_messages)
  if(USE_ROS2)
    set(ROS2_MSG_ROOT "${PROJECT_SOURCE_DIR}/io/ros2/msg")
    if(${ARGC} GREATER 0)
      set(ROS2_MSG_ROOT "${ARGV0}")
    endif()

    _ros2_discover_msgs("${ROS2_MSG_ROOT}")

    find_package(std_msgs QUIET)
    find_package(sp_msgs QUIET)
    set(ROS2_HAS_SYSTEM_MSGS FALSE)
    if(std_msgs_FOUND AND sp_msgs_FOUND)
      set(ROS2_HAS_SYSTEM_MSGS TRUE)
      message(STATUS "ros2_find_messages: 系统包 std_msgs + sp_msgs 可用")
    else()
      message(STATUS "ros2_find_messages: std_msgs=${std_msgs_FOUND}, sp_msgs=${sp_msgs_FOUND}")
    endif()

    if(NOT ROS2_MSG_PACKAGES STREQUAL "")
      file(MAKE_DIRECTORY "${ROS2_WS}/src")
      list(LENGTH ROS2_MSG_PACKAGES _ros2_n)
      math(EXPR _ros2_last "${_ros2_n} - 1")
      foreach(_ros2_i RANGE ${_ros2_last})
        list(GET ROS2_MSG_DIRS ${_ros2_i} _ros2_dir)
        list(GET ROS2_MSG_PACKAGES ${_ros2_i} _ros2_name)
        file(RELATIVE_PATH _ros2_rel "${ROS2_WS}/src" "${_ros2_dir}")
        set(_ros2_link "${ROS2_WS}/src/${_ros2_name}")
        if(EXISTS "${_ros2_link}" OR IS_SYMLINK "${_ros2_link}")
          file(REMOVE "${_ros2_link}")
        endif()
        file(CREATE_LINK "${_ros2_rel}" "${_ros2_link}" SYMBOLIC)
        list(PREPEND CMAKE_PREFIX_PATH "${ROS2_WS}/install/${_ros2_name}")
      endforeach()
      message(STATUS "ros2_find_messages: 搜索目录=${ROS2_MSG_ROOT}，包=[${ROS2_MSG_PACKAGES}]")
    else()
      message(STATUS "ros2_find_messages: ${ROS2_MSG_ROOT} 下无本地消息包")
    endif()

    # 本仓库 ROS2 桥接需要系统 sp_msgs；仅本地测试包不足以开启
    if(NOT ROS2_HAS_SYSTEM_MSGS)
      if(ROS2_FORCE_ON)
        message(FATAL_ERROR "ros2_find_messages: -DUSE_ROS2=ON 但未找到 std_msgs/sp_msgs")
      else()
        set(USE_ROS2 FALSE)
        message(STATUS "ros2_find_messages: 缺 std_msgs/sp_msgs，USE_ROS2=OFF")
      endif()
    endif()
  else()
    set(ROS2_MSG_ROOT "")
    set(ROS2_MSG_PACKAGES "")
    set(ROS2_MSG_DIRS "")
    set(ROS2_HAS_SYSTEM_MSGS FALSE)
  endif()
endmacro()

# ---------------------------------------------------------------------------
# ③ 编译/部署注册：创建编译期目标 ros2_msgs（有本地包时；配置阶段不跑 colcon）
# ---------------------------------------------------------------------------
macro(ros2_setup_interfaces)
  if(USE_ROS2)
    if(ROS2_MSG_PACKAGES STREQUAL "")
      message(STATUS "ros2_setup_interfaces: 无本地消息包，跳过 colcon（使用系统 sp_msgs）")
    else()
      find_program(_ros2_colcon colcon)
      if(NOT _ros2_colcon)
        message(FATAL_ERROR "ros2_setup_interfaces: 未找到 colcon")
      endif()

      set(_ros2_byproducts "")
      foreach(_ros2_name IN LISTS ROS2_MSG_PACKAGES)
        list(APPEND _ros2_byproducts
          "${ROS2_WS}/install/${_ros2_name}/share/${_ros2_name}/cmake/${_ros2_name}Config.cmake"
          "${ROS2_WS}/install/${_ros2_name}/lib/lib${_ros2_name}__rosidl_typesupport_cpp${CMAKE_SHARED_LIBRARY_SUFFIX}"
          "${ROS2_WS}/stamp/${_ros2_name}")
      endforeach()

      if(NOT TARGET ros2_msgs)
        add_custom_target(ros2_msgs
          COMMAND ${CMAKE_COMMAND}
            -DROS2_ROOT=${ROS2_ROOT}
            -DROS2_WS=${ROS2_WS}
            "-DROS2_PKG_NAMES=${ROS2_MSG_PACKAGES}"
            "-DROS2_PKG_DIRS=${ROS2_MSG_DIRS}"
            -P ${PROJECT_SOURCE_DIR}/cmake/ros2_build_msgs.cmake
          BYPRODUCTS ${_ros2_byproducts}
          COMMENT "ros2: building interface packages (colcon) → ${ROS2_WS}"
          VERBATIM)
      endif()

      message(STATUS "ros2_setup_interfaces: 已注册目标 ros2_msgs（编译阶段 colcon）")
    endif()
  endif()
endmacro()

# ---------------------------------------------------------------------------
# 子 CMake 用：链接 rclcpp / 挂消息包
# ---------------------------------------------------------------------------
function(ros2_link_rclcpp target)
  if(NOT USE_ROS2)
    return()
  endif()
  if(NOT TARGET ${target})
    message(FATAL_ERROR "ros2_link_rclcpp: 目标不存在: ${target}")
  endif()
  ament_target_dependencies(${target} rclcpp)
endfunction()

function(ros2_attach_messages target)
  if(NOT USE_ROS2)
    return()
  endif()
  if(NOT TARGET ros2_msgs)
    return()
  endif()
  if(NOT TARGET ${target})
    message(FATAL_ERROR "ros2_attach_messages: 目标不存在: ${target}")
  endif()

  add_dependencies(${target} ros2_msgs)

  foreach(_pkg IN LISTS ROS2_MSG_PACKAGES)
    set(_prefix "${ROS2_WS}/install/${_pkg}")
    set(_iface "ros2_msg_${_pkg}")
    if(NOT TARGET ${_iface})
      add_library(${_iface} INTERFACE)
      target_include_directories(${_iface} INTERFACE "${_prefix}/include/${_pkg}")
      target_link_directories(${_iface} INTERFACE "${_prefix}/lib")
      target_link_libraries(${_iface} INTERFACE
        ${_pkg}__rosidl_typesupport_cpp
        ${_pkg}__rosidl_typesupport_introspection_cpp
        ${_pkg}__rosidl_typesupport_fastrtps_cpp
        ${_pkg}__rosidl_generator_c)
    endif()
    target_link_libraries(${target} ${_iface})
  endforeach()
endfunction()

function(ros2_use target)
  if(NOT USE_ROS2)
    return()
  endif()
  if(NOT TARGET ${target})
    message(FATAL_ERROR "ros2_use: 目标不存在: ${target}")
  endif()

  # 本分支桥接依赖：rclcpp + 系统 std_msgs/sp_msgs
  set(_deps rclcpp)
  if(std_msgs_FOUND)
    list(APPEND _deps std_msgs)
  endif()
  if(sp_msgs_FOUND)
    list(APPEND _deps sp_msgs)
  endif()
  ament_target_dependencies(${target} ${_deps})

  # 若有本地消息包，一并挂上（编译期依赖 ros2_msgs）
  ros2_attach_messages(${target})
endfunction()

# ---------------------------------------------------------------------------
# ④ 主 CMake 末尾收尾（不编新东西）
# ---------------------------------------------------------------------------
macro(ros2_finalize)
  if(USE_ROS2)
    if(NOT ROS2_HAS_SYSTEM_MSGS)
      message(FATAL_ERROR "ros2_finalize: USE_ROS2=ON 但缺少 std_msgs/sp_msgs")
    endif()
    if(ROS2_MSG_PACKAGES STREQUAL "")
      message(STATUS "ros2_finalize: OK — ROOT=${ROS2_ROOT}, 系统包 std_msgs+sp_msgs（无本地包）")
    else()
      if(NOT TARGET ros2_msgs)
        message(FATAL_ERROR "ros2_finalize: 有本地消息包但未注册 ros2_msgs")
      endif()
      message(STATUS "ros2_finalize: OK — ROOT=${ROS2_ROOT}, WS=${ROS2_WS}, pkgs=[${ROS2_MSG_PACKAGES}]")
    endif()
  else()
    message(STATUS "ros2_finalize: ROS2 未启用")
  endif()
endmacro()
