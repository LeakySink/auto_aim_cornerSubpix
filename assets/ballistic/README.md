# assets/ballistic — 弹道拟合表

两张表都由外部仓库 **`ballistic-main`**（`g.py` + `v.py`，taichi 数值积分 + 三次多项式拟合）
离线生成，本仓库只消费产物。**`ballistic-main` 不在本仓库版本控制内**，所以下面把生成参数
和「怎么重生成」记在这里，否则下次没人知道表是怎么来的。

## 表格式

`V,pitch,time` 三列；`pitch` / `time` 各是 10 个多项式系数（`[...]` 形式）。

```
f(x, y) = c0 + c1·x + c2·y + c3·x² + c4·xy + c5·y²
             + c6·x³ + c7·x²y + c8·xy² + c9·y³
```

- `pitch` 列单位是**度**，且存的是**相对视线的抬高补偿量**（`g.py` 里
  `Pitch = degrees(solved_pitch − atan2(y, x))`），**不是绝对仰角**。
  `BallisticsPoly::solve()` 内部会补上 `atan2(h, d)`，调用方无需处理。
- `time` 列单位是**秒**；`x` 是水平距离，`y` 是目标相对枪口的高度差。

## 气动参数（`g.py:Cannon.__init__`）

| kind | 质量 M | 直径 | Cd | 对应 k = ρCdS/2M |
|---|---|---|---|---|
| `small` | 3.2 g | 16.8 mm | 0.47 | **0.01875** 1/m（= `tools::kDragCoefficientSmall`）|
| `big` | 43 g | 42.5 mm | 0.27 | **0.00513** 1/m（= `tools::kDragCoefficientBig`）|

`ρ` 取 30 ℃、100150 Pa 下的 1.15153 kg/m³。

## 当前覆盖范围

| 文件 | 行数 | V 范围 | x (d) 范围 |
|---|---|---|---|
| `small.csv` | 194 | 12.54 – 29.91 m/s | 0.5 – 15 m |
| `big.csv` | 80 | 13.0 – **16.6** m/s | 0.5 – 11 m（拟合域）|

## ~~big.csv 缺 16.5 档~~（已修复，2026-11-15）

历史上表上限只有 15.9625，而 2027 重装目标弹速是 16.5，运行时会**静默夹取**到
15.9625：不报错、原测试全绿，但 12 m 处抬枪角偏 1.0°≈209 mm（板宽才 135 mm）。

根因在 `ballistic-main/v.py`：

```python
V = [13., 16., 80]
dv = (V[1] - V[0]) / V[2]      # ← 错：应该是 (V[2] - 1)
for i in range(V[2]):          # i 取 0..79，最后一个点是 13 + 3/80*79 = 15.9625
```

差一个点，16.0 从未被生成。**已修正为**：

```python
V = [13., 16.6, 80]
dv = (V[1] - V[0]) / (V[2] - 1)   # 末点正好落在 16.6，盖住 16.5
```

新表已重生成（2026-11-15，taichi 1.7.4 / CUDA，RTX 5070）：

- 80 行，V∈[13.0, 16.6]，步长 0.045570，无 nan/inf
- `solve(16.5, 12m)` = **13.377°**（旧表夹取后为 14.374°，差 0.997°）
- 与独立 RK4 积分（k=0.00513）对拍：13.377° vs 13.362°，**差 0.015°**
- `tests/algo/ballistics_poly_check.cpp` §7 全绿

## 重生成步骤

需要 taichi（建议 CUDA 后端）与 GPU，**不能在普通办公机上跑**：

```bash
cd ballistic-main/ballistic-main
python v.py                                  # 产出 data/big.csv
cp data/big.csv <本仓库>/assets/ballistic/big.csv
```

替换后必须验证：

```bash
./build/tests/ballistics_poly_check assets/ballistic/big.csv
# 期望：== 7) 一节全绿，V 上限 ≥16.5 且 note 里不含"夹取"
```

`tests/algo/ballistics_poly_check.cpp` 的 §7 就是盯这件事的：它会断言
`v_max ≥ 16.5`、`solve(16.5, 12m)` 不被夹取、且 16.5 与表上限的解一致。
**CSV 没重生成之前这条断言是红的，这是有意为之。**

## 另注：`v.py` 的 `config` 与运行时 `ballistics_x_range` 不一致

`v.py` 里 `config = [0.5, 11., -2., 2., 0.2, 0.2]`（x 拟合到 11 m），
而 `configs/*.yaml` 的 `ballistics_x_range: [0.5, 15.0]`（运行时允许到 15 m）。

实测影响很小：x=11…15 m 区间多项式的偏差相对独立积分只有
−0.013° / −0.018° / −0.062°（11 / 13 / 15 m），即 15 m 处落点约 16 mm，可接受。
但**这个不一致本身仍应记着**——如果哪天把 x 上限改大、或换更大的目标距离，
外推误差需要重新量。
