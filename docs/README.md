# 文档索引

本目录只放**与当前代码一致**的活文档；一次性任务报告与已闭环的缺陷记录归档在 [`history/`](history/)。

## 活文档（描述当前实现 —— 改代码请同步更新这里）

| 文档 | 内容 |
| --- | --- |
| [`context_handoff.md`](context_handoff.md) | **会话级交接（最新）**：仓库经历了什么、每个关键决策的理由与证据、验证强度的诚实分级、未闭环风险 R1-R9、下一步五步走（编译 → .rlog 对照 → 重装 M1 → 执行器辨识 → BA）、工作约定、代码定位表 |
| [`architecture.md`](architecture.md) | **主文档**：四层架构、数据流、多线程模型、14 个入口、配置文件；附录 A 运行流程（逐行 + 时序图 + 远程看图）、附录 B `plot(data)` 字段字典、附录 C 各模块数学流程（C.6 弹道 + C.6.1 多项式表、C.14 脏数据护栏） |
| [`filter_ekf_vs_ukf.md`](filter_ekf_vs_ukf.md) | **EKF → UKF 评估**：非线性判断、单帧/真实跟踪实测对照（精度 3%、耗时 5x）、对 75% 预算的影响、代码修改量、触发条件与低成本替代方案。结论：不建议现在换 |
| [`r_adaptive_and_gating.md`](r_adaptive_and_gating.md) | **R 自适应 + 新息门控**：观测噪声随视角降权（floor=0.5 的过拟合对照）、卡方门控（18.47 与 9.488 的误杀对照）、组合效果（粗差场景位置误差 17x 改善）、上线注意事项与回滚方式 |
| [`build_verification_60f03de.md`](build_verification_60f03de.md) | **`60f03de` 编译与门控复核**：本机真实编译（0 error / 0 新增告警，含 9 个 TU 与工具链差异）、C++ 回环 A/B（门控灵敏度由 `R` 决定，`d=0.3 m` 幽灵板只拦 0.9%）、实车 `.rlog` 复核（**门控 0 触发 / 看门狗确认失效** / replay 距离量纲 2.28× 不可用）、复现方式与一键采样脚本 |
| [`ekf_model.md`](ekf_model.md) | 11 维 EKF：状态/转移/观测/噪声与可观测性 |
| [`planner.md`](planner.md) | TinyMPC 规划器（约束、权重、参考轨迹） |
| [`detector.md`](detector.md) | 检测与后处理（tup / yolov5 / yolo11 / 传统灯条） |
| [`coordinate_systems.md`](coordinate_systems.md) | 相机 / 云台 / 世界坐标系与变换链 |
| [`remote_logger.md`](remote_logger.md) | RemoteLogger（`tools/rdbg/`）分层、`.rlog`、图片话题与订阅 |
| [`CONFIG_PARAMETERS_REFERENCE.md`](CONFIG_PARAMETERS_REFERENCE.md) | 配置参数参考（以标定类为主） |
| [`heavy_handover.md`](heavy_handover.md) | **重装自瞄交接书（未来 6 个月唯一任务）**：一页速览、代码/文档地图、现状已实现-vs-未实现、已修 bug 复盘（含教训）、已知问题优先级、六个月研发计划与 gate、工作规范与 DoD、环境/故障排查、对外接口、风险、第一天 checklist |
| [`heavy_aim_baseline.md`](heavy_aim_baseline.md) | **重装自瞄系统基线（命中率 >=75%）**：把 75% 翻译成 sigma<=24mm 的顶层指标与模块级误差预算；M0-M8 各模块的基线/检验方法/验收阈值/典型失效；四层检验体系（单元/离线回放/静态靶/实车）；统计口径（样本量、分层下界、置信区间）；基线参数总表；不达标排查决策树；里程碑 gate |
| [`heavy_robot_backend.md`](heavy_robot_backend.md) | **2027 重装自瞄后端改造清单**：为步兵调的后端改到重装需要动的 P0/P1 项（弹速兜底 22 改 16.5、big.csv 补 16.5 档、回退 k 分弹种、大装甲收敛、radius 重标、MPC 加速度/延迟/开火门控、两阶段形态切换、板面选点）、涉及文件与施工顺序、前后端分工边界 |
| [`gimbal_mechanics_and_aiming.md`](gimbal_mechanics_and_aiming.md) | **机械尺度→电控带宽→自瞄工作基础**：各兵种转动惯量/力矩/闭环带宽估算、相位滞后与跟踪误差、执行链断链（加速度被丢弃）、各兵种自瞄假设、阶跃响应标定方案与 P0/P1/P2 清单 |
| [`robot_types_and_aiming.md`](robot_types_and_aiming.md) | **2027 兵种特性与自瞄剖析**：各兵种几何/弹速/装甲变化、物理限制、自适应装甲的战术含义、不同兵种自瞄能力矩阵、对本仓库的 P0/P1/P2 影响清单（依据 2027 三份规则手册 + 618 条变化对照） |
| [`assumptions.md`](assumptions.md) | **物理世界假设清单**：每条假设对应的代码位置、被违反后的表现、验证方法；排查"莫名不准"时逐条对号 |
| [`roadmap.md`](roadmap.md) | **后端加固路线图**（2026-10 ~ 2027-01）：变更影响矩阵、十周计划、1 月切换流程、验收标准 |

## 历史归档（已闭环，仅用于追溯"为什么这么改"）

| 文档 | 内容 |
| --- | --- |
| [`history/segfault_audit.md`](history/segfault_audit.md) | 段错误 / UB 审计清单（含各模型真实输出维度核对） |
| [`history/segfault_fix_handover.md`](history/segfault_fix_handover.md) | 上述审计的修复交接：NaN→UB→`limit_rad` 死循环的机制、RK4 数值基线 |
| [`history/rule_2027_impact.md`](history/rule_2027_impact.md) | 2026→2027 规则变化对代码的影响分析（前瞻，**尚未实施**） |

## 已删除（内容已被吸收，`git log` 可查）

- `OPTIMIZATION_HANDOVER.md`、`REVIEW_OPTIMIZATION_20261018.md`（2026-10-11 / 10-18）：7 项优化改造及其逐条复核。结论已并入 [`architecture.md`](architecture.md) 的 C.5（NIS 卡方阈值 9.488/19.675、LDLT 求逆等价性）与 [`verification/chi2_threshold/`](../verification/chi2_threshold)；其中「往 yaml 加 `drag_coefficient_up/down/boundary`」一项**已作废**——三键与"按高度二选一"的链路于 2026-10 移除，弹道改由 `ballistics-main` 拟合表承担。
- 根目录原有的 `SEGFAULT_*.md` / `RULE_2027_IMPACT.md` / `REMOTE_LOGGER.md`：按上表归档到 `history/` 或本目录。

## 归档

- [`archive/`](archive/) —— 2026 赛季参赛材料（只做历史留存，不再维护；说明见 `archive/README.md`）
