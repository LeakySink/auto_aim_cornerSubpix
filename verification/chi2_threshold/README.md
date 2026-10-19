# 卡方阈值的可复现验证

这两个程序用来回答："卡方阈值是不是必须改？改成 9.488 / 19.675 后是不是正确的？"
它们**不参与 CMake 构建**（`verification/` 未被任何 CMakeLists 扫描），需要时手工编译。

## 1. `chi2_quantile_and_mc.cpp` —— 只依赖标准库

```bash
g++ -std=c++17 -O2 chi2_quantile_and_mc.cpp -o a.exe && ./a.exe
```

- 自实现正则化不完全 Gamma 函数，精确计算 χ² 分位数（无需 scipy / Boost）
- 用 `target.cpp` / `math_tools.cpp` 里**逐行抄录**的真实模型（11 维状态、4 维观测、真实 `h`/`h_jacobian`/`F`/`Q`/`R`）跑蒙特卡洛

## 2. `old_vs_new_ekf.cpp` —— 依赖 Eigen

```bash
EIGEN=/path/to/eigen-3.4.0
ROOT=/path/to/auto_aim_cornerSubpix-main
g++ -std=c++17 -O2 -I$EIGEN -I$ROOT \
    old_vs_new_ekf.cpp old_ekf.cpp $ROOT/tools/extended_kalman_filter.cpp -o b.exe && ./b.exe
```

- `old_ekf.{cpp,hpp}` 是改动前（备份目录）的 EKF，仅把 namespace 改成 `tools_old`
- 同一份观测数据同时喂给新旧两个 EKF，逐帧比对状态 `x` 与协方差 `P`

## 实测结果（2026-10-18，120 条轨迹 × 5200 帧）

```
逐帧比对 新旧 EKF： max|x_new-x_old| = 2.6e-11 , max|P_new-P_old| = 7.8e-11
新 NIS(先验): E=4.0021  P(>9.488)=5.05%     [χ²(4) 理论: 4 与 5%]
旧 NIS(后验): E=3.5834  P(>9.488)=3.24%
旧阈值 0.711 触发 Bad-Converge 的帧数: 最小 4208 / 中位 4265 / 最大 4324
（窗口 10000、阈值 40%；理论预测 4000/0.936 ≈ 4274 帧，60 fps ≈ 71 s）
```

分位数核验（`chi2_quantile_and_mc.cpp`）：

```
χ²(4):  CDF(0.711)  = 0.050034   → 0.711 是 5% 分位数（不是 50%）
χ²(4):  50% 分位数  = 3.3567
χ²(4):  95% 分位数  = 9.4877     → 9.488 正确，P(X>9.488) = 5.00%
χ²(11): 95% 分位数  = 19.6751    → 19.675 正确，P(X>19.675) = 5.00%
```

仓库内还有一份常驻回归测试：`tests/algo/ekf_update_check.cpp`
（断言 NIS 必须是先验定义、9.488/0.711 的分位数性质、以及蒙特卡洛分布）。
