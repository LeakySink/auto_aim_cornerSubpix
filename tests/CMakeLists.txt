#################### tests ###################
# 本目录每个 *.cpp → 同名可执行文件；增删测试无需改本文件。
# 约定见 cmake/auto_executables.cmake
#   calibrate_*.cpp 自动带上 calibration/calibrator.cpp
#   ros2_*.cpp      有 rclcpp+sensor_msgs 即可（不强制 sp_msgs）
#   publish/subscribe/topic_loop_test、sentry* 需 USE_ROS2

include(${PROJECT_SOURCE_DIR}/cmake/auto_executables.cmake)
auto_add_executables(
  DEFAULT_LIBS ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io auto_aim auto_buff
)
