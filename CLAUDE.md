# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System

```bash
# Configure and build
cmake -B build
make -C build/ -j`nproc`

# Run main executables
./build/standard_mpc          # Standard infantry with MPC trajectory planner
./build/auto_aim_debug_mpc    # Debug version with visualization
./build/mt_auto_aim_debug     # Multi-threaded version
./build/sentry                # Sentry robot (requires ROS2)

# Run calibration programs
./build/capture               # Capture calibration images
./build/calibrate_camera      # Camera intrinsic calibration
./build/calibrate_handeye     # Hand-eye calibration

# Run module tests
./build/auto_aim_test         # Test auto-aim with video recording
./build/planner_test          # Test trajectory planner on real robot
./build/planner_test_offline  # Test planner with recorded data
./build/gimbal_response_test  # Test gimbal response characteristics
```

## Project Architecture

This is a RoboMaster auto-aim system built on a **modular vision framework** where different tasks (auto-aim, auto-buff, omniperception) share hardware resources through a unified application layer.

### Core Concept

Unlike traditional ROS-based systems, this framework:
- Has a **single main executable** per robot type that routes to different task modules based on gimbal mode signals
- Separates concerns into **four layers**: io (hardware abstraction), tools (utilities), tasks (feature implementation), src (application/orchestration)
- Supports **both industrial cameras** (MindVision/HikRobot) and **USB cameras** through unified interfaces

### Data Flow

```
Camera Thread → Image + Timestamp
                              ↓
                      Gimbal Thread (Quaternion)
                              ↓
                        Detector (Armor 4-corners)
                              ↓
                    Estimator (EKF - Target Motion State)
                              ↓
                    Planner (Trajectory Optimization)
                              ↓
                    Controller (Gimbal Command)
```

### Key Innovation: Trajectory Planning

The system uses a **trajectory perspective** instead of traditional decision trees:
- **Planner** (tasks/auto_aim/planner/): Uses TinyMPC solver to optimize gimbal trajectory subject to acceleration constraints
- Implements **early deceleration strategy** before armor switches to handle trajectory discontinuities
- Generates trajectory sequences for fire decision accounting for delay (t_fire)
- Unifies different motion states (translation, low/high speed spin) under single optimization framework

### Module Organization

**io/** - Hardware abstraction layer
- `camera.hpp/cpp`: Unified camera interface (industrial via SDK, USB via OpenCV)
- `gimbal/`: Gimbal communication (CAN or serial)
- `serial/`: Cross-platform serial communication
- `dm_imu/`, `cboard`: Microcontroller communication

**tools/** - Utility library
- `extended_kalman_filter.hpp`: EKF for target state estimation (position, velocity, yaw, angular velocity)
- `trajectory.hpp`: Ballistic trajectory calculation
- `yaml.hpp`: Configuration file loader
- `thread_safe_queue.hpp`: Multi-threading primitive
- `recorder.hpp`: Record timestamped video + quaternions (like rosbag)

**tasks/auto_aim/** - Auto-aim module
- `detector.cpp`: YOLO-based armor detection (v5/v8/yolo11 variants)
- `target.cpp`: Target state estimation using EKF
- `planner/planner.cpp`: **Trajectory planner using MPC**
- `solver.cpp`: Wrapper for planner integration
- `tracker.cpp`: Multi-target tracking and voting
- `shooter.cpp`: Fire decision based on trajectory alignment
- `aimer.cpp`: Coordinate transformation and yaw optimization

**src/** - Application layer (one executable per robot type)
- `standard_mpc.cpp`: Infantry with trajectory planner
- `sentry.cpp`: Sentry with navigation (ROS2)
- `uav.cpp`: Drone auto-aim

**configs/** - YAML configuration files
- Robot-specific configs (standard3.yaml, sentry.yaml, etc.)
- Contain camera params, gimbal limits, planner weights, etc.

## Important Details

### Camera Support
The project supports two camera types transparently:
- **Industrial cameras** (MindVision/HikRobot): Use vendor SDKs via `io/mindvision/` and `io/hikrobot/`
- **USB cameras**: Use OpenCV via `io/usbcamera/`
Configuration in YAML determines which is loaded.

### Gimbal Communication
Two protocols supported:
- **USB2CAN** (older): Uses SocketCAN
- **Virtual serial** (newer): Uses `io/serial/`
Configured via YAML, both abstracted behind `io/gimbal/` interface.

### Multi-threading
Two threading models:
- **Standard**: Camera thread → main loop processes
- **Multi-threaded** (mt_*): Separate detection threads using `thread_pool.hpp`
Detector threads communicate via `thread_safe_queue`

### ROS2 Integration
ROS2 is **optional** and only used for:
- Sentry navigation communication (publish/subscribe)
- Not required for core auto-aim functionality
CMake detects ROS2 automatically; missing ROS2 skips sentry targets.

### Coordinate Systems
- **Camera frame**: Standard optical frame (Z forward, Y down, X right)
- **Gimbal frame**: Quaternion from IMU, aligned via hand-eye calibration
- **World frame**: Target state estimated in world coordinates
- Yaw optimization accounts for camera-gimbal misalignment

### Trajectory Planner Parameters
Key planner params in YAML:
- `yaw_accel_limit`, `pitch_accel_limit`: Gimbal max acceleration (rad/s²)
- `decision_speed`: Threshold between low/high speed behavior
- `fire_thresh`: Max error for fire decision (radians)
- `prediction_delay_time`: Total system latency (~15ms)
- Planner weights: Cost function weights for trajectory optimization

### Calibration
Two types supported:
- **Camera intrinsic**: Chessboard pattern → `calibrate_camera.cpp`
- **Hand-eye**: Estimate camera-gimbal transform → `calibrate_handeye.cpp`
- Calibration data stored in `configs/calibration.yaml`

## Testing Strategy

Each module has independent test programs:
- `camera_test`: Verify camera connectivity
- `detector_video_test`: Test detector on video files
- `gimbal_response_test`: Measure gimbal step response for system ID
- `planner_test_offline`: Test planner with recorded data without hardware
- `minimum_vision_system`: End-to-end test with minimal components

Use `tests/auto_aim_test.cpp` for **integration testing with video recording** - this is the primary way to verify system behavior and debug issues.

## Performance Characteristics

Target performance: **~100 FPS** on NUC12WSKi7 (i7-1260P)
- Recognition: YOLO model via OpenVINO (GPU optional)
- Planning: <1ms per solve (TinyMPC optimized)
- Bottleneck is typically detection, not planning

## Debugging Tools

- **PlotJuggler**: Visualize gimbal trajectories and decision variables in real-time
- **NoMachine**: Remote desktop for on-robot debugging
- **Video recorder**: `tools/recorder.hpp` saves timestamped video + quaternion streams
- **Plotter**: `tools/plotter.hpp` publishes data for PlotJuggler via UDP

## Common Modifications

- Add new robot type: Create new `src/new_robot.cpp` following `standard_mpc.cpp` pattern
- Tune planner: Adjust weights in YAML config, use `planner_test_offline` to verify
- Switch camera: Change YAML config, rebuild (camera type abstracted)
- Add new detection model: Extend `yolos/` directory, update `yolo.cpp`
