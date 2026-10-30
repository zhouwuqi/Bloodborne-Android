# AYANEO Pocket S：30 帧稳定输出与温控（pocket-s 分支）

目标：在 Pocket S（骁龙 G3x Gen 2，与 8 Gen 2 同代的 CPU，Adreno A32）上稳定 30 帧，帧在屏幕上停留得均匀，发热可控。

## 改了什么

1. **帧率限制器的时间槽规则**（`gpu/shadps4/core/libraries/videoout/driver.cpp`）
   2.2.0 的限制器用"本帧实际翻页的时刻 + 帧间隔"算下一个时间槽。线程从 `sleep_until` 醒来总会晚几十到一百多微秒，
   这个延迟每帧都累加进去，结果是 29.9 帧而不是 30 帧。对着 60Hz 屏幕，每隔几秒就会滑过一次刷新，出现一阵抖动。
   现在：只晚了不到 1ms（且不超过帧间隔的 1/10）的翻页，按原来的时间槽节奏往下排；真正晚到的帧照旧顺延。
   `BB_PACE_FROM_WAKE=1` 恢复旧行为。

2. **显示端反馈与相位锁**（新文件 `gpu/shim/bbport_pacing.{h,cpp}`）
   App 回收每个缓冲区时会带回 release fence，它在该帧离开屏幕的那一刻触发。读出 fence 的触发时间
   （`SYNC_IOC_FILE_INFO`），游戏进程就能知道每帧在屏幕上实际停了多久，**不需要改 APK**。
   - 每 10 秒在游戏日志里输出一行 `Pacing:`：屏幕上的帧率、停留时长正确的比例、只在显示端不均匀的比例、
     游戏自己迟到的比例、交帧到上屏的延迟、温度余量和调速器档位。`BB_PACING_STATS=0` 关闭。
   - 相位锁（锁相环）：积分项学出屏幕和 CPU 时钟之间的频率差，比例项把"交帧到上屏"的延迟稳在锁定时的值，
     所以时间槽不会相对屏幕慢慢漂移。如果交帧均匀、上屏却反复不均匀（说明锁在了刷新边界上），
     就把时间槽挪半个刷新周期。需要 `vsync=1`（FIFO），开启补帧时不工作。`BB_PACE_LOCK=0` 关闭。

3. **温控调速器**（`gpu/shim/bbport_adpf.cpp`）
   - 0 档：ADPF 的目标时长取帧预算的 85%（`BB_ADPF_MARGIN`），给偏重的帧留出余量，减少掉帧。
   - 1 档（10 秒预测温度余量 ≥ 0.85）：目标改为完整帧预算，频率只给到刚好够用。
   - 2 档（≥ 0.95，到 1.0 就开始降频）：除已经单独绑核的 GPU 命令线程外，所有线程避开超大核（X3）。
     Android 重设 cpuset 时会把线程放回原来的核，所以这一档每 100ms 检查并重新绑定一次。
   - 某一档的温度余量回落到阈值以下 0.10、并持续 30 秒，才降回下一档。`BB_THERMAL_GOV=0` 关闭调速器。

## 怎么用（不重装 App，游戏和已编好的原生代码都不受影响）

前提：Pocket S 上已经装了官方 2.2.0 App，并且至少启动过一次；电脑能用 adb 连上设备。官方 APK 是 debuggable 的，
所以可以用 `run-as` 直接替换 App 目录里的库。

```bash
tools/android/dev/build_libbbgpu.sh      # 编 libbbgpu.so（需要 NDK 29 和依赖库，见脚本开头）
tools/android/dev/pocket_s.sh push       # 推送到设备（第一次会把官方的库备份成 libbbgpu.so.official）
tools/android/dev/pocket_s.sh settings   # 设置 fps_limit=30、vsync=1、关闭补帧（会先备份 bbport.ini）
# 在 Pocket S 上玩 15～30 分钟
tools/android/dev/pocket_s.sh log        # 拉回日志并输出帧节奏和温度汇总表
tools/android/dev/pocket_s.sh restore    # 恢复官方库和原来的设置
```

App 更新时会重新解包自己的运行时，替换进去的库会被覆盖，到时重新执行一次 `push` 即可。

对比测试：把 `tools/android/dev/pocket-s/bionic-local.sh` 里的 `BB_PACE_LOCK` 改成 0，执行 `pocket_s.sh profile`，
同一段路线各跑一次，对比两次 `log` 的结果。

## 本机验证（没有 Pocket S 时能做的）

- `tests/run_pacing_sim.sh`：实时模拟一块屏幕（59.8 / 60.3Hz，GPU 完成时间有抖动，游戏偶尔迟到），
  完整跑一遍节奏模块，统计不是两次刷新的帧所占的比例：

  | 场景 | 不开锁 | 开锁 |
  |---|---|---|
  | 59.8Hz，2.2.0 原版限制器 | ~7–8% | – |
  | 59.8Hz | 4.6% | 0% |
  | 60.3Hz | 6.0% | 0% |
  | 59.8Hz，GPU 0.5–7ms | 13.8% | 0% |
  | 59.8Hz，2% 的帧晚 10ms | 7.8% | 2.6% |

- `tests/test_thermal_governor.cpp`：调速器的升降级、迟滞、目标时长和线程迁移。
- 和官方 2.2.0 运行时的 ABI 对比：编译器相同（NDK 29，clang 21），`bb-probe` 和 `libbbnet20.so`
  从 libbbgpu 引用的符号全部存在，依赖库列表一致（VMA 头文件用 3.4.0）。

**真机效果还没验证**，以上都是模拟和静态检查的结果。到 Pocket S 上主要看三点：`Pacing:` 行里停留时长正确的比例、
`Thermal governor:` 的档位变化，以及半小时后的帧率。
