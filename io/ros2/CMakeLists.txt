################## ROS2 桥 ##################
# 变量与目标由顶层 ros2_init / find_messages / setup_interfaces 准备好。

if(USE_ROS2)
  message(STATUS "io: ROS2 enabled, system_msgs=${ROS2_HAS_SYSTEM_MSGS}, local_pkgs=[${ROS2_MSG_PACKAGES}], compiling ROS2 bridge")
  target_sources(io PRIVATE
    publish2nav.cpp
    subscribe2nav.cpp
    ros2.cpp
  )
  ros2_use(io)
else()
  message(STATUS "io: ROS2 disabled, ROS2 bridge not compiled")
endif()
