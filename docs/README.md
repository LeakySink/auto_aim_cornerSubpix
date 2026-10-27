# 文档索引

本目录只放**与当前代码一致**的活文档；一次性任务报告与已闭环的缺陷记录归档在 [`history/`](history/)。

## 活文档（描述当前实现 —— 改代码请同步更新这里）

| 文档 | 内容 |
| --- | --- |
| [`architecture.md`](architecture.md) | **主文档**：四层架构、数据流、多线程模型、14 个入口、配置文件；附录 A 运行流程（逐行 + 时序图 + 远程看图）、附录 B `plot(data)` 字段字典、附录 C 各模块数学流程（C.6 弹道 + C.6.1 多项式表、C.14 脏数据护栏） |
| [`ekf_model.md`](ekf_model.md) | 11 维 EKF：状态/转移/观测/噪声与可观测性 |
| [`planner.md`](planner.md) | TinyMPC 规划器（约束、权重、参考轨迹） |
| [`detector.md`](detector.md) | 检测与后处理（tup / yolov5 / yolo11 / 传统灯条） |
| [`coordinate_systems.md`](coordinate_systems.md) | 相机 / 云台 / 世界坐标系与变换链 |
| [`remote_logger.md`](remote_logger.md) | RemoteLogger（`tools/rdbg/`）分层、`.rlog`、图片话题与订阅 |
| [`CONFIG_PARAMETERS_REFERENCE.md`](CONFIG_PARAMETERS_REFERENCE.md) | 配置参数参考（以标定类为主） |
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

## 待定（未随代码更新，使用前请自行核对）

- `2026技术方案_*`（创新点详解 / 完整版 / 算法篇 / 生成报告 / 图表清单 / 技术路线图）：参赛与申报材料，写作于 2026-10，**之后弹道、检测、护栏均有较大改动**，与当前实现可能不一致；保留仅作存档。
