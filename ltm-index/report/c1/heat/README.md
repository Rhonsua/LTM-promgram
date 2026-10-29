# P1：热度与混合索引并发边界（2026-10-06 至 2026-10-07）

实现与验收范围以 docs/status.md 为准；锁顺序、回调及生命周期契约见
docs/architecture.md 的「HybridIndex 并发边界」。P0 原始证据保留在
report/c1/acceptance/，本目录保存 P1 改动后的实测。

## 实现

- HotColdTracker 以自身互斥锁同步表访问、扩容、衰减、驻留标志、页释放、计数重置
  与统计读取；stats 返回值快照，锁内不反向访问存储对象。
- HybridIndex 在完整逻辑操作上协调读写，包括 Erase 读取旧 MBR、双树事务、
  Close、Checkpoint 与 Validate；普通读者可重叠，KNN 为可变单次计数额外串行。
- 范围与空间回调先复制一页命中记录、释放页闩后执行。支持嵌套读取，拒绝回调内
  写操作及排他运维调用；避免共享门升级死锁。
- BufferPool::SnapshotStats 保护池与 I/O 值快照；HybridIndex 查询数用原子计数。
  新增显式 DecayHeat，未启动后台线程。

## 已完成的门槛

| 门槛 | 结果 | 原始文件 |
| --- | --- | --- |
| 普通构建新增用例 | 4 passed、0 failed | normal_final.txt |
| ASAN 普通全量 ctest | 17/17，186.83 秒 | verified_acceptance/asan/ctest.txt |
| ASAN 两个结构守门 | 均退出 0；I6 违反 0 | verified_acceptance/asan/validate_bplus.txt、validate_rtree.txt |
| Debug 正确性专项 | 5/5，39.67 秒 | verified_acceptance/debug/ctest.txt |
| Debug 两个结构守门 | 均退出 0 | verified_acceptance/debug/validate_bplus.txt、validate_rtree.txt |
| Debug 默认逆序终止 | 指定标记与退出码 3 均符合预期 | verified_acceptance/debug/order_abort.txt |
| 最终源码短时重复压力 | 18 轮全过、63.43 秒，版本哈希未变化 | soak_final/manifest.json |

verified_acceptance/*/manifest.json 保存最终构建、源码哈希、工具链、参数及每项退出码。
final_verification.json 确认 ASAN/Debug 各 77 个源码文件及 CMake 哈希与交付文件一致。
acceptance/ 是生命周期修复之前的版本；final_acceptance/ 保存沙箱内 C1902 构建失败，
不视为源码测试失败。最终 verified_acceptance 在允许访问 MSVC 调试信息服务的环境运行。
ASAN 运行库导入已确认。Debug 仅上表专项，不宣称完整 Debug ctest 通过。
layering 扫描 46 个源文件、违规 0；ASCII 检查扫描 81 个文件、违规 0。

新增测试覆盖：6 个热度访问线程与衰减、精确 Touch 计数和 rehash；屏障验证两个
读回调实际重叠；范围/空间/KNN 回调嵌套读取与早停；启用热度、12 帧池下的 5 读者、
2 写者、24 次衰减、4 次检查点和 4 次校验。不可变记录逐次对照，写键分区且版本
编码进 MBR/payload，结束时全量记录对照独立 map，并检查淘汰与锁序计数。
最终新增第四个用例覆盖同对象三次重开和存储已建立后的打开失败重试；并加强混合
读取对请求键、允许版本及不可变查询键集的检查，没有改变负载操作流。

## 长时间验证（2026-10-06 完成，生命周期修复之前的版本）

1. 持续 crabbing：build-ninja/soak_mixed.exe 1800 6 2 20000 512 10 10。
   输出 soak_crabbing_30min.txt。使用原有进度心跳、排空检查点、最终校验及全量扫描。
   此配置 heat=nullptr，不覆盖热度；工作集大于池容量的 REGIME 必须为 evicting。
   本轮退出 0、SOAK_VERDICT clean、REGIME evicting；91710073 次操作、最终 18771 条
   全量读取正确、178 次检查点、I6 与锁序违反 0。版本和参数见 crabbing_manifest.json。
2. 启用热度：python scripts/soak_hybrid_heat.py --seconds 1800。
   每轮运行固定混合并发用例，重新创建数据库；原始轮次在 soak/round_*.txt，
   参数、源码/二进制哈希、轮次退出码与单轮耗时在 soak/manifest.json。
   每轮限时 120 秒，退出 0 之外还检查测试计数和 HYBRID_HEAT 标记，拒绝零用例
   的假通过。输出进度在 soak_console.txt。此项是 30 分钟重复轮次，不能解释成
   单个数据库连续 30 分钟的历史，也不是吞吐基准。
   本轮 255 轮全过，1800.18 秒，源码和二进制哈希未变化，进程退出 0。

2026-10-07 生命周期补验后的源码与这些长压测版本分别记录。资源清理修复仅在存在旧
存储对象的重开/重试分支执行，没有改变上述每轮新建对象的查询/写入/热度路径；最终
源码重新跑完整 ASAN、Debug 专项、两个结构守门及新生命周期用例，并补 60 秒重复压力
验证（soak_final/）。不把早先 30 分钟报告改标为最终源码。

## 复现

```text
python scripts/accept_c1.py --mode asan --output-dir report/c1/heat/verified_acceptance
python scripts/accept_c1.py --mode debug --output-dir report/c1/heat/verified_acceptance
python scripts/build_local.py build build-ninja RelWithDebInfo
python scripts/soak_hybrid_heat.py --seconds 1800
build-ninja/soak_mixed.exe 1800 6 2 20000 512 10 10
```

使用新的输出目录保留旧证据。accept_c1.py 会先构建、检查 Ninja 头文件依赖，再运行
测试与守门；build_local.py test 本身不会构建。

## 限制

### 生命周期补验发现与修复（2026-10-07）

独立 ASAN 复现 report/c1/heat/reopen_probe.cc 在同一 HybridIndex 上执行
Open → Put → Close → Open：旧实现先销毁 disk/recovery，再销毁仍引用它们的
BufferPool，触发 heap-use-after-free。旧 HybridDiff 重开用例创建了另一个对象，
没有覆盖此路径。失败原始输出保留在 reopen_before_fix.txt。修复按借用依赖的逆序
释放旧资源后再重建，并重置会话计数；回归覆盖多次重开和失败后重试。相同独立
ASAN 复现在 reopen_probe.txt 中打印 REOPEN_PROBE clean、退出 0；完整最终验收均通过。

ASAN 不检测数据竞争，MSVC 无 TSAN，锁序检查为单线程持锁顺序判据；压测不证明
所有调度安全。原始 pool()/disk()、底层树直接调用及动态注册表不经接口门，仍需
调用方同步；对象销毁前须停止所有调用者。长查询及其回调会阻挡写者，KNN 调用
串行；本轮没有性能加速结论。M6-2 的强口径整体完成状态仍保持未完成。
