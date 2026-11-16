# 文档索引

本分支（`feat/heavy-aim`）只做 **2027 重装自瞄**。文档精简到 **5 份活文档**，其余全部
移到 [`archive/`](archive/)，不再维护但保留历史。

## 活文档

| 文档 | 什么时候看 |
|---|---|
| [`heavy_handover.md`](heavy_handover.md) | **计划**：未来 6 个月做什么、验收 gate、故障排查、第一周 checklist |
| [`heavy_aim_baseline.md`](heavy_aim_baseline.md) | **目标与预算**：75% 命中率 ⟺ σ≤24 mm 的分解、M0–M8 模块基线、四层检验体系、统计口径 |
| [`architecture.md`](architecture.md) | **工程参考**：四层架构、数据流与延迟、9 个入口、重装策略、模块速查、弹道与护栏、调试字段。**约 320 行**，逐模块数学推导已移到 [`archive/pipeline_details.md`](archive/pipeline_details.md) |
| [`context_handoff.md`](context_handoff.md) | **当前状态**：走到哪一步、验证强度分级、未闭环风险 R1–R9 |
| [`build_verification_60f03de.md`](build_verification_60f03de.md) | **构建与验证**：怎么在本机编、门控灵敏度实测、实车 `.rlog` 复核、一键采样脚本 |

另有资源目录里的说明（不在 `docs/`，但同样要读）：

- [`../assets/ballistic/README.md`](../assets/ballistic/README.md)：弹道表怎么生成、气动参数、
  `big.csv` 覆盖范围与重生成步骤

## 归档（`archive/`）

**已归档但仍描述当前实现**（代码没变，只是不再单独维护）：

| 文档 | 内容 |
|---|---|
| [`archive/ekf_model.md`](archive/ekf_model.md) | 11 维 EKF：状态 / 转移 / 观测 / 噪声 |
| [`archive/planner.md`](archive/planner.md) | TinyMPC 规划器：约束、权重、参考轨迹 |
| [`archive/detector.md`](archive/detector.md) | 检测与后处理（tup / yolov5 / yolo11 / 传统灯条） |
| [`archive/coordinate_systems.md`](archive/coordinate_systems.md) | 相机 / 云台 / 世界坐标系与变换链 |
| [`archive/CONFIG_PARAMETERS_REFERENCE.md`](archive/CONFIG_PARAMETERS_REFERENCE.md) | 全部 yaml 参数速查 |
| [`archive/remote_logger.md`](archive/remote_logger.md) | RemoteLogger 分层、`.rlog`、图片话题 |
| [`archive/heavy_robot_backend.md`](archive/heavy_robot_backend.md) | 重装后端改造 P0/P1 清单（已并入 `heavy_handover.md`） |
| [`archive/assumptions.md`](archive/assumptions.md) | 物理世界假设清单（被违反时代码会怎样） |
| [`archive/r_adaptive_and_gating.md`](archive/r_adaptive_and_gating.md) | R 自适应 + 新息门控的原始论证（实测结论见 `build_verification_60f03de.md`） |
| [`archive/filter_ekf_vs_ukf.md`](archive/filter_ekf_vs_ukf.md) | 为什么现在不换 UKF |
| [`archive/gimbal_mechanics_and_aiming.md`](archive/gimbal_mechanics_and_aiming.md) | 机械尺度 → 电控带宽 → 自瞄的推导 |
| [`archive/robot_types_and_aiming.md`](archive/robot_types_and_aiming.md) | 2027 各兵种特性与自瞄剖析 |
| [`archive/roadmap.md`](archive/roadmap.md) | 旧的后端加固周计划（里程碑已并入 `heavy_handover.md`） |

**历史记录**：[`history/`](history/)（2027 规则影响、段错误审计与修复交接）、
[`archive/`](archive/) 里 2026 参赛材料。

## 约定

1. 一处改动一个提交，中文提交信息 `类型: 主题`。
2. 改了行为就改这 5 份里的对应文档，并在上面的索引里补一行。
3. 新增文档前先问：**能不能并进现有 5 份**？不能才是新增。
4. 根目录只留 `readme.md`，其余文档进 `docs/`。

---

> **2026-11 起打符（能量机关）子系统已整体移除**：`tasks/auto_buff/`、两个 `auto_buff_debug*`
> 入口、专用模型与 `buff_layout.xml` 全部删除；`GimbalMode` 不再有 `SMALL_BUFF`/`BIG_BUFF`，
> 下位机若仍上报 mode=2/3 按 IDLE 处理。同时删除 `uav` / `uav_debug` / `mt_standard` /
> `mt_auto_aim_debug` 四个非重装入口。
