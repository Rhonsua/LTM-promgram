# 检查点与恢复性能实测

2026-10-07 普通配置 15/15、ASAN 两项补验通过；17 项结果协议与两个实际 CLI 拒绝检查通过。原始记录及复核见 [audit_20261007.json](audit_20261007.json)。当前工程不再开展旧纯内存版本对照。

固定 Hybrid 的 512 次插入与 512 次更新，完整输入和最终记录跨组一致，并由独立 Python 生成核对。二维记录、4KB 页、24 帧、两树容量 32。每场景仅一次显式检查点，改变检查点位置，分别留下 0/16/64/256/512 次后续更新。每组三次独立数据库，运行次序打乱。

Prepare 完成全部 Put 与日志同步后以退出码 9 立即退出，不执行 Close 或析构；第二个进程按规定顺序执行原有 Recover；第三个进程逐条比较键、MBR、载荷，完整扫描并执行双树 I8。没有未提交事务，故本轮不测 Undo 耗时曲线。

| 检查点后更新 | WAL 字节 | Recover 中位毫秒 | 最短至最长毫秒 | 实际重放记录中位数 |
|---:|---:|---:|---:|---:|
| 0 | 592 | 1.564 | 1.121–1.594 | 0 |
| 16 | 827568 | 13.782 | 13.409–14.567 | 53 |
| 64 | 3349816 | 51.640 | 51.372–52.644 | 214 |
| 256 | 14248680 | 224.021 | 221.520–224.095 | 1262 |
| 512 | 29463656 | 462.565 | 456.151–467.914 | 2695 |

Recover 时间取现有 RecoveryManager 的报告，包含其分析、Redo、Undo 阶段及同步，排除对象构造、进程启动、查询、结果核对、关闭。`storage_ready_us` 另列设备、恢复管理器、缓冲池构造与 Recover 的总时间，仍不是完整应用启动时间。592 字节是日志头和检查点记录，此组扫描两条且无重放。

系统文件缓存未清除；未隔离系统后台活动；没有同时启动项目内其他测量或压力实验。仅报告固定、小规模、已提交事务场景，不能推广为设备冷启动、断电持久性或普遍线性关系。ASAN 在 0/512 更新尾补验且导入表含实际运行库，检查器耗时不参加性能比较。

## 复跑

在项目目录执行，输出目录必须不存在。

```powershell
python -B scripts/recovery_bench.py --output-dir report/recovery/new_run
python -B scripts/recovery_bench.py --skip-build --output-dir report/recovery/another_run
python -B scripts/recovery_bench.py --asan --build-dir build-recovery-asan --tails 0 512 --repeats 1 --output-dir report/recovery/new_asan
python -B scripts/test_recovery_bench.py
```

独立 CMake 包装位于 `tools/recovery_bench`，只构建该入口及原有库；不会覆盖历史 build-ninja/ASAN/Debug/FI 二进制。本轮存储和索引核心、主 CMake 未修改。新增 source/entry 的哈希、普通与 ASAN 原始命令和输出分别保存在各自 manifest，不能视为核心全量测试重跑。

普通数据位于 [ordinary_20261007](ordinary_20261007/)，ASAN 补验位于 [asan_20261007](asan_20261007/)，每场景的 `crash_snapshot` 为恢复前数据库原件快照，`database` 为恢复核对后的数据库。两份分别保留，原始快照不会因恢复而改写。构建预检、17 项协议、分层、ASCII、拒绝覆盖/非法参数和 ASAN 导入检查见 `checks_20261007`。第一次构建的历史文件遍历遇到 Linux log 链接不可访问，失败记录保留在 `build_20261007`，修正目录范围后的构建通过。

报告图表、表格与分析保存在 [结题材料](../../../结题材料/README.md)；结题目录仅复制恢复日志/JSON/CSV 与摘要，完整崩溃快照留在本工程证据目录，避免重复大文件。
