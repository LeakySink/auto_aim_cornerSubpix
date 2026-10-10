# cmake/ros2_build_msgs.cmake
# 由 add_custom_target(ros2_msgs) 在【编译阶段】通过 cmake -P 调用。
# 需要 -D：ROS2_ROOT、ROS2_WS、ROS2_PKG_NAMES、ROS2_PKG_DIRS（后两者为 ; 分隔列表）

if(NOT ROS2_ROOT OR NOT ROS2_WS)
  message(FATAL_ERROR "ros2_build_msgs: 缺少 ROS2_ROOT / ROS2_WS")
endif()
if(NOT ROS2_PKG_NAMES)
  message(STATUS "ros2_build_msgs: 无消息包，跳过")
  return()
endif()

list(LENGTH ROS2_PKG_NAMES _n)
list(LENGTH ROS2_PKG_DIRS _nd)
if(NOT _n EQUAL _nd)
  message(FATAL_ERROR "ros2_build_msgs: PKG_NAMES/PKG_DIRS 数量不一致")
endif()

set(_stamp_dir "${ROS2_WS}/stamp")
set(_build_list "")

math(EXPR _last "${_n} - 1")
foreach(_i RANGE ${_last})
  list(GET ROS2_PKG_NAMES ${_i} _name)
  list(GET ROS2_PKG_DIRS ${_i} _dir)
  set(_config "${ROS2_WS}/install/${_name}/share/${_name}/cmake/${_name}Config.cmake")
  set(_stamp "${_stamp_dir}/${_name}")
  set(_need FALSE)
  if(NOT EXISTS "${_config}")
    set(_need TRUE)
  elseif(NOT EXISTS "${_stamp}")
    set(_need TRUE)
  else()
    file(GLOB_RECURSE _srcs
      "${_dir}/*.msg" "${_dir}/*.srv" "${_dir}/*.action"
      "${_dir}/CMakeLists.txt" "${_dir}/package.xml")
    foreach(_f IN LISTS _srcs)
      if("${_f}" IS_NEWER_THAN "${_stamp}")
        set(_need TRUE)
      endif()
    endforeach()
  endif()
  if(_need)
    list(APPEND _build_list "${_name}")
  endif()
endforeach()

if(_build_list STREQUAL "")
  message(STATUS "ros2_build_msgs: 消息包已是最新，跳过 colcon")
  return()
endif()

find_program(_colcon colcon)
if(NOT _colcon)
  message(FATAL_ERROR "ros2_build_msgs: 未找到 colcon")
endif()

string(REPLACE ";" " " _pkgs "${_build_list}")
message(STATUS "ros2_build_msgs: colcon build [${_pkgs}]")

file(MAKE_DIRECTORY
  "${ROS2_WS}/src" "${ROS2_WS}/build" "${ROS2_WS}/install"
  "${ROS2_WS}/log" "${_stamp_dir}")

# 刷新软链接（配置阶段也会建；这里再保证一次）
foreach(_i RANGE ${_last})
  list(GET ROS2_PKG_NAMES ${_i} _name)
  list(GET ROS2_PKG_DIRS ${_i} _dir)
  file(RELATIVE_PATH _rel "${ROS2_WS}/src" "${_dir}")
  set(_link "${ROS2_WS}/src/${_name}")
  if(EXISTS "${_link}" OR IS_SYMLINK "${_link}")
    file(REMOVE "${_link}")
  endif()
  file(CREATE_LINK "${_rel}" "${_link}" SYMBOLIC)
endforeach()

execute_process(
  COMMAND bash -c "set -e
    source '${ROS2_ROOT}/setup.bash'
    colcon --log-base '${ROS2_WS}/log' build \
      --packages-select ${_pkgs} \
      --build-base '${ROS2_WS}/build' \
      --install-base '${ROS2_WS}/install' \
      --cmake-args -DCMAKE_BUILD_TYPE=Release"
  WORKING_DIRECTORY "${ROS2_WS}"
  RESULT_VARIABLE _rc
  ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "ros2_build_msgs: colcon 失败 (code=${_rc})\n${_err}")
endif()

foreach(_name IN LISTS _build_list)
  if(EXISTS "${ROS2_WS}/install/${_name}/share/${_name}/cmake/${_name}Config.cmake")
    file(WRITE "${_stamp_dir}/${_name}" "")
  else()
    message(FATAL_ERROR "ros2_build_msgs: 包 ${_name} 安装后仍找不到 Config.cmake")
  endif()
endforeach()

message(STATUS "ros2_build_msgs: 完成")
