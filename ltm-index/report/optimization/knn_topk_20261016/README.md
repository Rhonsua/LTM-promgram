# KNN top-k 正确性与候选维护优化（2026-10-16）

## 问题与实现

原 best-first KNN 在已有 k 个候选时，遇到距离等于当前最差距离的记录便跳过。
虽然最后按 `(距离, 键)` 排序，但同距的较小键可能已经被丢掉，无法通过排序找回。
例如同一坐标依次插入键 9、8、1，查询一个最近邻，正确结果应为键 1，旧选择逻辑
可能保留先遇到的键 9。旧测试主要比较距离，无法发现这种完整答案不同的情况。

本轮修改 `src/index/rtree.cc` 的 best-first 查询与头文件契约注释，并新增两个
RTree 回归、一个 HybridIndex 回归，未改存储层、WAL、页格式、锁顺序或暴力扫描。

- 候选按 `(距离, 键)` 排序，用大顶堆保留前 k 条，堆顶为最差的完整排名。同距且键
  更小的记录会替换堆顶；跳过候选时也检查键，避免无谓的载荷物化。
- 子树只有距离下界，必须保留距离相等的分支，只在下界严格大于堆顶距离时剪枝。
- 维护一次被接纳的候选从 O(k) 扫描、移位降为 O(log k) 堆更新，最终排序仍为
  O(k log k)。候选内存为 O(min(k, N)) 条记录，未按任意输入 k 预分配；待访问节点
  队列的内存另计，不宣称整个查询只有 O(k) 内存。k=0 保持原来按 1 处理的行为。
- 暴力路径继续全量叶链扫描并全排序，保留与候选堆不同的选择方式。

复杂度改进不等于已测得吞吐量提升；本轮没有并发吞吐或大 k 性能百分比结论。
同距候选需要正确竞争键排名，会比旧实现错误地跳过它们做更多有效工作。

## 新回归与修复前对照

1. `RTreeDiff.KnnTopKBreaksLeafTiesByKey`：无分裂的单叶页，9 条记录逆键插入，
   包含键 0 与 uint64 最大值边界；同一非零距离，k 为 0/1/3/8/9/20，逐条核对
   排名、距离、MBR 和载荷。
2. `RTreeDiff.KnnTopKMatchesCompleteOracleAcrossSplitPolicies`：even / linear /
   quadratic 三种策略，三维、96 条记录、12 帧，结构页数实际超过帧数；删除前后、
   两个探测点、11 种 k，合计 132 组参数各跑 fast 与 brute。独立模型直接从输入
   MBR 计算距离并全排序，比较完整记录，覆盖零距离、同距边界及 k 大于记录数。
3. `HybridConcurrency.KnnTopKTiesRemainOrderedAfterReopen`：48 条记录逆序写入，
   同距 KNN 和 Query 提前停止分别核对；显式检查点、关闭、同对象重开后再验证，
   末尾双树校验与零锁序违反。

修复前在独立构建运行上述三个用例，三个均因错误答案失败，保留原始输出和测试
二进制在 [baseline_085030383268](baseline_085030383268/manifest.json)。其中 Hybrid
对照包含完整记录与首条回调断言失败，并非仅根据任意非零退出码判定复现。
原四份源码另保存在 `before_source`，基线构建的实际源码完整保存在其 `source_snapshot`。

## 复现入口与历史证据

从工程根目录执行：

```powershell
python -B scripts/verify_knn_topk.py --phase ordinary
python -B scripts/verify_knn_topk.py --phase asan
python -B scripts/verify_knn_topk.py --phase debug
wsl -d Ubuntu --exec python3 -B /mnt/d/db_project/LTM/ltm-index/scripts/verify_knn_topk.py --phase tsan
python -B scripts/run_pipeline.py --skip-build --build-dir build-opt-topk
```

分别使用独立 `build-opt-topk`、`build-opt-topk-asan`、`build-opt-topk-debug` 与
`build-opt-topk-tsan`。证据目录有同名时追加时间，保留失败记录。baseline 模式仅用于
修复前源码的预期失败对照，不能对当前修复后源码重复要求失败。TSAN 数据库存放于
WSL 的持久文件系统，保持真实文件同步，未用内存文件系统替代落盘。

首轮 baseline 在沙箱中遇到编译器 PDB 管理器访问失败，沙箱外重新编译后完成对照。
首轮 Linux 发现新增 Hybrid 测试把 `std::min<size_t>` 返回的临时参数引用直接交给
分语句求值的断言宏；已将预期数量物化为值，再重新构建并复跑四配置。该失败
不是 TSAN 数据竞争报告；最初通过的 Windows 运行属于初版测试，最终结果另存。

随后两次 Linux 重试在原运行库正对照上没有报告预设竞争，驱动按失败退出，未把
这些运行计作有效检测。当前入口将正反对照的就绪计数改为 relaxed 原子操作，循环
从 1000 扩为 100000，负对照仍用互斥锁并检查最终值 300000。加强后的正对照必须
退出 66 且包含真实 data race 报告，负对照必须退出 0，才能运行项目检查。

独立核对入口为 `python -B report/optimization/knn_topk_20261016/verify_evidence.py`。
该入口要求最终 C++ 源码、测试及其运行快照哈希匹配。Windows 三配置使用的驱动
先于 Linux 对照加固，原入口保存于 `driver_before_control_fix.py`；核对脚本确认
后续入口改动仅在 Linux TSAN 对照分支内，并分别记录执行版本，而不是把新入口
冒称为已运行的 Windows 版本。旧测试版本的通过结果保留为历史记录；后续核心
源码再修改应建立新证据，不能改写本轮或 10 月 8 日的清单。

## 后续工程工作

1. 给 mixed 增加无逐写额外 Get 探测的测量轨迹，测量后完整核对并由新进程重开。
   正确性轨迹与测量轨迹使用同一固定输入，计时、采样和结果协议明确分开。
2. 当前 KNN 固定工作量的单/多读者及不同 k 性能测量，分开驻留和淘汰配置，至少
   三次重复；不能根据 ASAN / TSAN 用时或并行回归推算提速比例。
3. 扩大磁盘工作集并补冷热策略消融，分别界定超出 BufferPool 与超出系统内存的证据。

本轮仅涉及项目实现、回归和工程记录，未修改结题材料。

## 最终验证结果

| 配置 | 最终结果 | 清单 |
| --- | --- | --- |
| 普通 | 完整 ctest 20/20，双树结构守门、分层、ASCII 通过 | [ordinary](ordinary_085720328216/manifest.json) |
| ASAN | 完整 ctest 20/20，双树守门、分层、ASCII 通过；运行库导入确认 | [asan](asan_085805866834/manifest.json) |
| Debug | RTreeDiff / C1Replay / HybridConcurrency / WriteAbort 4/4，双树守门、分层、ASCII 通过 | [debug](debug_085744287298/manifest.json) |
| Linux TSAN | 同上四项 4/4，双树守门、严格 Werror、插桩、运行库符号、正反对照、分层、ASCII 通过 | [tsan](tsan_090231749877/manifest.json) |
| 主链路 | 两个独立进程均通过，512 条/12 帧，完整记录指纹一致 | [pipeline](pipeline/manifest.json) |

三个新回归包含在上述四种构建的测试目标中，ctest 的 20 / 4 是目标数，不能再把
新用例数加上去。各目录保留源码快照、编译命令、原始输出、`LastTest.log` 和二进制
哈希。原始测试时间不用于性能比较。

[独立审计](audit.json) 核对 400 对源码/执行快照：397 对与当前文件直接匹配，
三个 Windows 入口按已保存的原执行版本匹配，且只相差后来 Linux 对照分支的加固。
全部核心 C++ 与测试均匹配当前版本。10 月 8 日优化的 182 项文件证据和更早混合
验收的 56 个 Windows 二进制保持原哈希。原始主链路输出重新解析通过，数据与
二进制哈希已保存。

本轮确认同距 top-k 正确性和候选维护实现，尚未量化实际吞吐量提升；未重跑完整
Linux 套件、长时间压力或历史强杀矩阵。历史结果仍只适用于各自记录的版本与参数。
