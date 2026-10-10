################## calibration ################
# 本目录每个 *.cpp → 同名可执行文件；calibrator.cpp 是库源码，排除。
# calibrate.cpp 会自动附带 calibrator.cpp（见 cmake/auto_executables.cmake）

include(${PROJECT_SOURCE_DIR}/cmake/auto_executables.cmake)
auto_add_executables(
  EXCLUDE calibrator.cpp
  DEFAULT_LIBS ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io
)
