################## calibration ################

add_executable(calibrate calibrate.cpp calibrator.cpp)
add_executable(split_video split_video.cpp)

target_link_libraries(calibrate ${OpenCV_LIBS} fmt::fmt yaml-cpp tools io)
target_link_libraries(split_video ${OpenCV_LIBS} fmt::fmt tools)
