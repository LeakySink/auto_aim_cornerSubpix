#################### tests ###################

add_executable(auto_aim_test auto_aim_test.cpp)
add_executable(auto_buff_test auto_buff_test.cpp)
add_executable(camera_detect_test camera_detect_test.cpp)
add_executable(camera_test camera_test.cpp)
add_executable(camera_thread_test camera_thread_test.cpp)
add_executable(cboard_test cboard_test.cpp)
add_executable(fire_test fire_test.cpp)
add_executable(detector_video_test detector_video_test.cpp)
add_executable(gimbal_response_test gimbal_response_test.cpp)
add_executable(multi_usbcamera_test multi_usbcamera_test.cpp)
add_executable(usbcamera_detect_test usbcamera_detect_test.cpp)
add_executable(usbcamera_test usbcamera_test.cpp)
add_executable(handeye_test handeye_test.cpp)
add_executable(dm_test dm_test.cpp)
add_executable(minimum_vision_system minimum_vision_system.cpp)
add_executable(calibrate_test calibrate_test.cpp ${PROJECT_SOURCE_DIR}/calibration/calibrator.cpp)

target_link_libraries(auto_aim_test ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io auto_aim)
target_link_libraries(auto_buff_test ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_buff tools io)
target_link_libraries(camera_detect_test ${OpenCV_LIBS} fmt::fmt yaml-cpp tools auto_aim io)
target_link_libraries(camera_test ${OpenCV_LIBS} fmt::fmt tools io)
target_link_libraries(camera_thread_test ${OpenCV_LIBS} fmt::fmt auto_aim tools io)
target_link_libraries(cboard_test ${OpenCV_LIBS} fmt::fmt tools io)
target_link_libraries(fire_test ${OpenCV_LIBS} fmt::fmt tools io)
target_link_libraries(detector_video_test ${OpenCV_LIBS} fmt::fmt yaml-cpp tools auto_aim)
target_link_libraries(gimbal_response_test ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io)
target_link_libraries(multi_usbcamera_test ${OpenCV_LIBS} fmt::fmt tools io)
target_link_libraries(usbcamera_detect_test ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io auto_aim)
target_link_libraries(usbcamera_test ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io)
target_link_libraries(handeye_test ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io auto_aim)
target_link_libraries(dm_test ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io)
target_link_libraries(minimum_vision_system ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io auto_aim)
target_link_libraries(calibrate_test ${OpenCV_LIBS} fmt::fmt yaml-cpp tools)

# 多车协议自检的假车，不是独立测试。multi_sender_test 默认拉起同目录下的 fake_robot。
add_executable(fake_robot fake_robot.cpp)
target_link_libraries(fake_robot ${OpenCV_LIBS} fmt::fmt yaml-cpp tools)

add_executable(multi_sender_test multi_sender_test.cpp)
target_link_libraries(multi_sender_test ${OpenCV_LIBS} fmt::fmt tools)
add_dependencies(multi_sender_test fake_robot)

add_executable(gimbal_test gimbal_test.cpp)
target_link_libraries(gimbal_test ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim tools io)

add_executable(planner_test planner_test.cpp)
target_link_libraries(planner_test ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim tools io)

add_executable(planner_test_offline planner_test_offline.cpp)
target_link_libraries(planner_test_offline ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim tools io)

add_executable(auto_aim_debug_mpc_offline auto_aim_debug_mpc_offline.cpp)
target_link_libraries(auto_aim_debug_mpc_offline ${OpenCV_LIBS} fmt::fmt yaml-cpp auto_aim tools io)

# ROS2 测试
if(USE_ROS2)
  add_executable(publish_test publish_test.cpp)
  add_executable(subscribe_test subscribe_test.cpp)
  add_executable(topic_loop_test topic_loop_test.cpp)

  target_link_libraries(publish_test ${OpenCV_LIBS} fmt::fmt tools io auto_aim)
  target_link_libraries(subscribe_test ${OpenCV_LIBS} fmt::fmt tools io)
  target_link_libraries(topic_loop_test ${OpenCV_LIBS} fmt::fmt tools io)

  ros2_use(publish_test)
  ros2_use(subscribe_test)
  ros2_use(topic_loop_test)
endif()

# 海康 → /image_raw（对比 ROS 标定；只需 rclcpp + sensor_msgs，不依赖 sp_msgs）
if(DEFINED ENV{ROS_DISTRO} AND EXISTS "/opt/ros/$ENV{ROS_DISTRO}")
  list(PREPEND CMAKE_PREFIX_PATH "/opt/ros/$ENV{ROS_DISTRO}")
elseif(EXISTS "/opt/ros/humble")
  list(PREPEND CMAKE_PREFIX_PATH "/opt/ros/humble")
endif()
find_package(rclcpp QUIET)
find_package(sensor_msgs QUIET)
if(rclcpp_FOUND AND sensor_msgs_FOUND)
  add_executable(ros2_image_pub_test ros2_image_pub_test.cpp)
  target_link_libraries(ros2_image_pub_test
    ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io
    rclcpp::rclcpp
    sensor_msgs::sensor_msgs)
  message(STATUS "tests: ros2_image_pub_test enabled (HikRobot → /image_raw)")
else()
  message(STATUS "tests: ros2_image_pub_test skipped (need rclcpp + sensor_msgs)")
endif()
