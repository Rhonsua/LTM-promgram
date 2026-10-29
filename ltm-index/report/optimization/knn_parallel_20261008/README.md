# KNN 读取并行优化（2026-10-08）

HybridIndex 原先用递归互斥锁包住整个 KNN 遍历和用户回调，只为保护 RTree 共用的诊断计数器。一个慢回调会阻塞其他 KNN 读者。本轮把计数器移到调用内，完成遍历与回调后短锁发布，移除整段查询锁。页格式、持久化协议、距离及同距键排序保持不变。

这是工程实现与验证记录，不是结题材料。修改文件为 `src/index/rtree.{cc,h}`、`src/index/hybrid_index.{cc,h}`、两个回归测试文件，以及新增验证入口。历史源码证据和构建二进制保持原样。

## 实际行为与边界

- 两个 KNN 查询的回调可同时进入；回调内嵌套读取和提前停止仍可正常结束，写操作升级仍返回 `kInvalidArgument`。
- 查询队列、候选结果和探测计数独立。`knn_stats()` 返回值而非共享状态引用；调用方保存的快照不会被其他查询改写。该返回类型变化要求使用方重新编译。
- 诊断快照的 `pages_visited`、`entries_scanned` 来自最近完成的 best-first 调用；`brute_pages` 由暴力调用完成时覆盖，best-first 完成时重置。并发快照不能归属指定查询，也不能据此计算指定查询的剪枝率。
- 完整读操作仍持 HybridIndex 共享接口门，写者、检查点与校验需等待读者和回调结束。BufferPool 和热度表仍有同步开销；原始 RTree 的写入、生命周期操作仍须外部同步。
- 已验证并行能力，尚未测量吞吐量增幅。此次 Debug / TSAN 只覆盖受影响的四项专项，未重跑完整 Linux 套件或长时间压测，也未重新执行历史强杀矩阵。

## 验证结果

| 配置 | 测试结果 | 额外检查 | 成功记录 |
| --- | --- | --- | --- |
| 旧代码对照 | 重叠回归预期退出 1，确认整段串行阻止重叠 | 原始三份核心源码与旧测试二进制保留 | [baseline](baseline_133307936531/manifest.json) |
| 普通 | 完整 ctest 20/20 | B+ / R 结构守门、分层、ASCII | [ordinary](ordinary_134028863137/manifest.json) |
| ASAN | 完整 ctest 20/20 | 双树守门、分层、ASCII；运行库导入确认 | [asan](asan_134402072301/manifest.json) |
| Debug | RTreeDiff / C1Replay / HybridConcurrency / WriteAbort 4/4 | 双树守门、分层、ASCII | [debug](debug/manifest.json) |
| Linux TSAN | 同上四项 4/4 | 双树守门、严格 Werror、全编译单元插桩与运行库符号、竞争正反对照、分层、ASCII | [tsan](tsan_134407156444/manifest.json) |
| 主链路 | 两个独立进程均通过 | 512 条记录、12 帧；写入、检查点、完整记录核对、关闭、新进程重开；记录指纹一致 | [pipeline](pipeline/manifest.json) |

三个新回归在上述四种优化后构建中均通过：

1. `KnnReadersOverlapAndNestedQueriesRemainIndependent`：两个不同探测点，回调重叠、嵌套 KNN、回调写升级拒绝、提前停止和零锁序违反。相同回归已在旧实现运行并失败。
2. `ParallelKnnMatchesIndependentTopKAfterReopen`：96 条记录、12 帧，树页总数超过帧数；关闭重开后四个读者共 192 次查询，k 为 1/5/17/200，逐条对照独立距离与键排序模型并检查完整载荷。最终结构一致、有实际缺页和零锁序违反。
3. `KnnStatsSnapshotsAreStableWithParallelBruteForceAndReaders`：优化小 k、大 k、暴力扫描并发，第四个读者读取 4000 次统计快照；计数对必须来自完整发布，保存的历史快照保持不变。

普通和 ASAN 的 20 项是 ctest 测试目标数，三个新回归为其中测试目标内部新增用例，不另加到 20 项。四项专项同理。

## 证据与复现

[audit.json](audit.json) 记录独立复核结果：396 对当前源码与运行时快照哈希匹配、原始 ctest 及用例通过标记、当次二进制、主链路原始输出重新解析与数据库哈希、56 个历史 Windows 二进制保持原哈希。每个成功目录保留完整 `LastTest.log`、构建参数、编译命令、阶段输出、源码快照和清单。TSAN 测试数据库放在 WSL 的持久文件系统，保留正常同步语义；没有用内存文件系统替代落盘。

从工程根目录运行以下入口会创建新证据目录；有同名目录时自动追加时间，不覆盖既有记录：

```powershell
python -B scripts/verify_knn_optimization.py --phase ordinary
python -B scripts/verify_knn_optimization.py --phase asan
python -B scripts/verify_knn_optimization.py --phase debug
wsl -d Ubuntu --exec python3 -B /mnt/d/db_project/LTM/ltm-index/scripts/verify_knn_optimization.py --phase tsan
python -B scripts/run_pipeline.py --skip-build --build-dir build-opt-knn
```

构建分别使用 `build-opt-knn`、`build-opt-knn-asan`、`build-opt-knn-debug` 和 `build-opt-knn-tsan`。`baseline` 模式只用于旧实现预期失败对照，不适用于当前优化后的源码；原三份代码保存在 `baseline_source`，原始回归可执行文件保存在成功 baseline 目录。

本次最终数据可用 `python -B report/optimization/knn_parallel_20261008/verify_evidence.py` 在 Windows 复核；该脚本依赖当前源码和对应构建文件仍与本次快照一致，后续修改源码应按新版本建立证据，不能更新旧清单冒充当前验证。

初次 `baseline` 遇到编译器 PDB 管理器访问失败；重试成功。初次 ordinary / ASAN / TSAN 的新重开用例在读取结构页数前未调用 Validate，读取到尚未更新的诊断快照，修正测试夹具后重跑通过。失败清单与原始输出仍保留，不计作成功。
