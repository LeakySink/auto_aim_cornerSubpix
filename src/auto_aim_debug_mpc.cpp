#include <fmt/core.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <thread>

#include "io/camera.hpp"
#include "io/gimbal/gimbal.hpp"
#include "io/remote_debug.hpp"
#include "tasks/auto_aim/kalman_markers.hpp"
#include "tasks/auto_aim/planner/planner.hpp"
#include "tasks/auto_aim/solver.hpp"
#include "tasks/auto_aim/tracker.hpp"
#include "tasks/auto_aim/yolo.hpp"
#include "tools/exiter.hpp"
#include "tools/img_tools.hpp"
#include "tools/remote_logger.hpp"
#include "tools/math_tools.hpp"
#include "tools/thread_safe_queue.hpp"
using namespace std::chrono_literals;

const std::string keys =
  "{help h usage ? |                        | 输出命令行参数说明}"
  "{@config-path   | configs/sentry.yaml | 位置参数，yaml配置文件路径 }";

int main(int argc, char * argv[])
{
  tools::Exiter exiter;

  cv::CommandLineParser cli(argc, argv, keys);
  auto config_path = cli.get<std::string>(0);
  if (cli.has("help") || config_path.empty()) {
    cli.printMessage();
    return 0;
  }

  tools::RemoteLogger::instance().init(config_path);
  io::RemoteDebug::install();

  io::Gimbal gimbal(config_path);
  io::Camera camera(config_path);

  auto_aim::YOLO yolo(config_path, false);
  auto_aim::Solver solver(config_path);
  auto_aim::Tracker tracker(config_path, solver);
  auto_aim::Planner planner(config_path);

  tools::ThreadSafeQueue<std::optional<auto_aim::Target>, true> target_queue(1);
  target_queue.push(std::nullopt);
  // armors 被两个线程共享：主线程 `armors = yolo.detect(img)` 整体赋值
  // （会销毁旧节点），plan 线程同时 `for (const auto & a : armors)` 遍历。
  // 两者重叠时 plan 线程的迭代器指向已释放节点 -> UAF / 段错误。
  // 用互斥量保护：主线程先在局部变量里算完 new_armors（含 tracker.track 的
  // 修改），再在锁内整体替换；plan 线程的遍历也在锁内。
  std::list<auto_aim::Armor> armors;
  std::mutex armors_mutex;
  // 最新世界姿态（主线程写、plan 线程读，用于把 q 记进 plot 数据）
  Eigen::Quaterniond gimbal_q = Eigen::Quaterniond::Identity();
  std::mutex gimbal_q_mutex;
  std::atomic<bool> quit = false;
  auto plan_thread = std::thread([&]() {
    auto t0 = std::chrono::steady_clock::now();
    uint16_t last_bullet_count = 0;

    while (!quit) {
      auto target = target_queue.front();
      auto gs = gimbal.state();
      auto plan = planner.plan(target, gs.bullet_speed);
      const auto yaw_err = tools::limit_rad(plan.yaw0 - gs.yaw);
      const auto pitch_err = plan.pitch0 + gs.pitch;
      const auto yp_error=std::hypot(yaw_err, pitch_err);
      const bool fire = std::hypot(yaw_err, pitch_err) < planner.fire_thresh();
      if (plan.control) {
        gimbal.send(
       plan.control, fire, plan.yaw, plan.yaw_vel, plan.yaw_acc, plan.pitch, plan.pitch_vel,
       plan.pitch_acc);
      }
      auto fired = gs.bullet_count > last_bullet_count;
      last_bullet_count = gs.bullet_count;

      nlohmann::json data;
      data["t"] = tools::delta_time(std::chrono::steady_clock::now(), t0);

      data["gimbal_yaw"] = gs.yaw;                // rad（离线回放按 rad 解释）
      data["gimbal_yaw_vel"] = gs.yaw_vel;
      data["gimbal_pitch"] = gs.pitch;
      data["gimbal_pitch_vel"] = gs.pitch_vel;
      {
        std::lock_guard<std::mutex> lock(gimbal_q_mutex);
        data["gimbal_qw"] = gimbal_q.w();
        data["gimbal_qx"] = gimbal_q.x();
        data["gimbal_qy"] = gimbal_q.y();
        data["gimbal_qz"] = gimbal_q.z();
      }

      data["plan_yaw"] = plan.yaw;
      data["plan_yaw_vel"] = plan.yaw_vel;
      data["plan_yaw_acc"] = plan.yaw_acc;

      data["plan_pitch"] = plan.pitch;
      data["plan_pitch_vel"] = plan.pitch_vel;
      data["plan_pitch_acc"] = plan.pitch_acc;

      data["yaw_error"] = yaw_err;
      data["pitch_error"] = pitch_err;
      data["yp_error"]=yp_error;

      data["fire"] = fire ? 1 : 0;

      if (target.has_value()) {
        const auto & e = target->ekf();
        data["target_x"] = target->ekf_x()[0];   //z
        data["target_vx"] = target->ekf_x()[1];
        data["target_y"] = target->ekf_x()[2];   //z
        data["target_vy"] = target->ekf_x()[3];
        data["target_z"] = target->ekf_x()[4];   //z
        data["target_vz"] = target->ekf_x()[5];  //vz
        data["target_yaw"] = target->ekf_x()[6];
        data["armor_id"] = target->last_id;      // last_id 是 public
        // 卡方检验数据（含 nis/nees 与两个阈值），Watch 页可与参考线对照
        for (const auto & kv : e.data) data[kv.first] = kv.second;
      }
      {
        // 遍历必须持锁：主线程随时可能把 armors 整体替换掉
        std::lock_guard<std::mutex> lock(armors_mutex);
        if (target.has_value() && !armors.empty()) {
          for (const auto & a : armors) {
            if (a.name == target->name && a.type == target->armor_type) {
              data["obs_yaw"] = a.ypd_in_world[0];       // z[0]
              data["obs_pitch"] = a.ypd_in_world[1];     // z[1]
              data["obs_dist"] = a.ypd_in_world[2];      // z[2]
              data["obs_armor_yaw"] = a.ypr_in_world[0]; // z[3]
              break;
            }
          }
        }
      }
      if (target.has_value()) {
        data["w"] = target->ekf_x()[7];
        data["markers"] = tools::viz::kalman_markers(*target);
      } else {
        data["w"] = 0.0;
      }
      tools::RemoteLogger::instance().plot(data);

      std::this_thread::sleep_for(10ms);
    }
  });

  cv::Mat img;
  std::chrono::steady_clock::time_point t;

  while (!exiter.exit()) {
    camera.read(img, t);
    // 单独发布**原始帧**（此时还没画任何标注）：离线回放/重跑检测只能吃干净图，
    // 而 reprojection 那条流是画过框和关键点的。plot_image 仅在 host 订阅该流时
    // 才真正编码发送，不订阅时零开销。
    tools::RemoteLogger::instance().plot_image(img, {{"name", "raw"}});
    auto q = gimbal.q(t);
    solver.set_R_gimbal2world(q);
    {
      // 记下最新世界姿态：离线回放要复现 tracker 必须有它（只有 yaw/pitch 不够，
      // 世界系姿态来自 IMU，无法从云台角度反推）
      std::lock_guard<std::mutex> lock(gimbal_q_mutex);
      gimbal_q = q;
    }
    // 先在局部变量里完成检测与跟踪（tracker.track 会就地修改 armors），
    // 再在锁内整体替换共享列表 —— 锁持有时间只是一次 list move，很短，
    // 不会阻塞 plan 线程的遍历。
    auto new_armors = yolo.detect(img);
    auto targets = tracker.track(new_armors, t);
    {
      std::lock_guard<std::mutex> lock(armors_mutex);
      armors = std::move(new_armors);
    }
    if (!targets.empty())
      target_queue.push(targets.front());
    else
      target_queue.push(std::nullopt);

    if (!targets.empty()) {
      auto target = targets.front();

      // 当前帧target更新后
      std::vector<Eigen::Vector4d> armor_xyza_list = target.armor_xyza_list();
      for (const Eigen::Vector4d & xyza : armor_xyza_list) {
        auto image_points =
          solver.reproject_armor(xyza.head(3), xyza[3], target.armor_type, target.name);
        tools::draw_points(img, image_points, {0, 255, 0});
      }

      // 红框 = 当前帧的理想瞄准点（plan() 算完才写，无目标/无解时 validity 为 false）
      if (planner.debug_xyza_valid()) {
        Eigen::Vector4d aim_xyza = planner.debug_xyza();
        auto image_points =
          solver.reproject_armor(aim_xyza.head(3), aim_xyza[3], target.armor_type, target.name);
        tools::draw_points(img, image_points, {0, 0, 255});
      }
    }

    tools::RemoteLogger::instance().plot_image(img, {{"name", "reprojection"}});
    io::RemoteDebug::on_frame(camera.fps());
  }

  quit = true;
  if (plan_thread.joinable()) plan_thread.join();
  gimbal.send(false, false, 0, 0, 0, 0, 0, 0);
  tools::RemoteLogger::instance().shutdown();

  return 0;
}
