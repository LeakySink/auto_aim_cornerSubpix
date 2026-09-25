#################### src：主程序 ###################
# 本目录每个 *.cpp → 同名可执行文件；增删文件无需改本文件。
# 约定见 cmake/auto_executables.cmake（sentry* 需 USE_ROS2 等）

include(${PROJECT_SOURCE_DIR}/cmake/auto_executables.cmake)
auto_add_executables(
  DEFAULT_LIBS ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim auto_buff tools io
)
