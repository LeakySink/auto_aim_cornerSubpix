#################### tests ###################
# 按子目录分类；每个子目录 *.cpp → 同名可执行文件（见 cmake/auto_executables.cmake）。
#   rdbg/  RemoteLogger / Host 协议与证明
#   algo/  感知 / 自瞄算法
#   io/    硬件冒烟
#   ros2/  ROS2 桥（需 USE_ROS2 或 ros2_* 依赖）

include(${PROJECT_SOURCE_DIR}/cmake/auto_executables.cmake)

set(_TEST_LIBS ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io auto_aim auto_buff)

foreach(_cat IN ITEMS rdbg algo io ros2)
  auto_add_executables(
    DIR ${CMAKE_CURRENT_SOURCE_DIR}/${_cat}
    DEFAULT_LIBS ${_TEST_LIBS}
  )
endforeach()

# multi_sender_test 与 fake_robot 分属同一次 DIR 扫描时已 add_dependencies；
# 若拆目录，在此补依赖。
if(TARGET multi_sender_test AND TARGET fake_robot)
  add_dependencies(multi_sender_test fake_robot)
endif()
