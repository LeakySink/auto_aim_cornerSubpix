#################### src：主程序 ###################

add_executable(standard standard.cpp)
add_executable(mt_standard mt_standard.cpp)
add_executable(standard_mpc standard_mpc.cpp)
add_executable(auto_aim_debug_mpc auto_aim_debug_mpc.cpp)
add_executable(mt_auto_aim_debug mt_auto_aim_debug.cpp)
add_executable(auto_buff_debug auto_buff_debug.cpp)
add_executable(auto_buff_debug_mpc auto_buff_debug_mpc.cpp)
add_executable(uav uav.cpp)
add_executable(uav_debug uav_debug.cpp)

target_link_libraries(standard ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim auto_buff tools io)
target_link_libraries(mt_standard ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim auto_buff tools io)
target_link_libraries(standard_mpc ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim auto_buff tools io)
target_link_libraries(auto_aim_debug_mpc ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim auto_buff tools io)
target_link_libraries(mt_auto_aim_debug ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim auto_buff tools io)
target_link_libraries(auto_buff_debug ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_buff tools io ${CERES_LIBRARIES})
target_link_libraries(auto_buff_debug_mpc ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_buff tools io ${CERES_LIBRARIES})
target_link_libraries(uav ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim auto_buff tools io)
target_link_libraries(uav_debug ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim tools io)

# ROS2：sentry 系列（依赖 io 中的 ROS2 桥）
if(USE_ROS2)
  add_executable(sentry sentry.cpp)
  add_executable(sentry_bp sentry_bp.cpp)
  add_executable(sentry_debug sentry_debug.cpp)
  add_executable(sentry_multithread sentry_multithread.cpp)

  target_link_libraries(sentry ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim omniperception tools io)
  target_link_libraries(sentry_bp ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim omniperception tools io)
  target_link_libraries(sentry_debug ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim omniperception tools io)
  target_link_libraries(sentry_multithread ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim omniperception tools io)

  ros2_use(sentry)
  ros2_use(sentry_bp)
  ros2_use(sentry_debug)
  ros2_use(sentry_multithread)
endif()
