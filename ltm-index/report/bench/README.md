# P2 最小基准闭环（2026-10-07）

当前已扩展至 Hybrid `spatial/contain/knn`，结果和契约见 [Hybrid 基准说明](hybrid_README.md)。
下文保留 B+ 初次闭环的版本、数据和验收证据；历史哈希只对应当时实现。

已实现并验证 B+ 点查/范围查询、完整结果校验、分阶段计时、测量区间统计与结构化
结果的生产者/驱动闭环。M6-3 全部负载与 P3 大规模实验仍未完成；本轮没有修改
索引、存储、热度或页闩实现。

## 复现

```text
python scripts/build_local.py build build-ninja RelWithDebInfo
python scripts/test_bench.py
python scripts/bench.py --records 2048 --ops 2000 --frames 12 512 --repeats 3
```

默认每次创建带 UTC 时间戳的独立 report/bench 子目录；指定 --output-dir 时目标
必须尚不存在，不能覆盖已有证据。--dry-run 只打印命令，不生成成功报告。
每次运行保存数据/WAL、stdout/stderr、result.json；实验级保存 results.csv、
summary.json、manifest.json（参数、命令、工具/源码/二进制哈希、起止时间、退出码）。
manifest 的 source_unchanged 验证测量期间源码及二进制没有变化。

独立调用需要显式 --run-dir，已有数据库会退出 2 且不改原文件。该版本只支持 point/range；
当时未知负载、mix/spatial/knn、8K/16K、迁移/恢复/R 分裂实验全部拒绝。
当前 spatial/contain/knn 已实现，其他延期实验继续拒绝。
保留 mem-vs-ltm 这个旧实验名称，但实际含义是同一页式 B+ 树在全驻留与淘汰配置
下的对照，不是原内存树与磁盘树的比较。驱动要求至少三次重复及两个经实测证明的
regime，否则整个实验退出 1，不能把空输出、零操作或缺少字段记为成功。

## 测量契约

- 单线程，B+ 每页容量 32、4KB 页、纯 CLOCK、heat=nullptr、预取关闭。
- 固定 seed=42，2048 条完整载荷记录；显式 Fisher-Yates 装载顺序和独立查询随机流。
  驱动逐项匹配请求参数，对照每组数据/查询指纹及树页数；每组只改变池帧数。
- 装载使用逐条 Insert，单独记录 load_seconds，没有宣称 BulkLoad。
- 装载后结构校验、检查点、完整叶链记录校验/缓存填充；随后 200 次查询预热。
  装载、预热、校验和关闭均不计入查询延迟或测量区间 I/O。
- 2000 次正式查询，每次返回完整 key/payload、范围键序与条数均对照固定数据。
  查询结果收集/复制及清理计入查询计时；预生成输入和结果核对在每次计时区间外。
  throughput=ops/各次查询计时之和；measured_wall_seconds 另外保留整个测量循环
  （含核对）墙钟时间。p50/p95/p99/p999 使用所有单次样本的 nearest-rank。
- 池/磁盘统计是测量前后差值，来自 SnapshotStats 与 DiskManager::IoStats。
  全树 99 页加元数据 1 页，实际工作集 100 页；全叶链扫描填充缓存后才开始预热。
  驻留配置必须测到缺页/淘汰均为 0，淘汰配置必须实际测到二者非零。
  I/O 是应用逻辑页读写，系统文件缓存仍存在，不能称为设备物理 I/O 或 RSS 超限。
- 只读测量要求 page_writes=wal_bytes=0；操作失败、错误结果、I6/锁序违反、错误
  字段类型、NaN、参数不符及吞吐/命中率/分位数不一致均拒绝成功。

## 本轮结果

原始 12 次运行在 p2_comparison/；每组 3 次重复、输入指纹一致、全部完整记录正确。

| 负载 | 池帧 | 配置 | 吞吐中位数（次/秒） | p99 中位数（微秒） | 测量页读中位数 |
| --- | ---: | --- | ---: | ---: | ---: |
| point | 12 | evicting | 45442 | 45.0 | 2764 |
| point | 512 | resident | 726269 | 1.5 | 0 |
| range，宽度 16 | 12 | evicting | 25060 | 66.3 | 4450 |
| range，宽度 16 | 512 | resident | 163217 | 7.2 | 0 |

summary.json 同时保留吞吐 min/median/max，results.csv 保留每轮所有指标。测量样本
较小且驻留计时区间很短，这些数字用于证明基准闭环与路径差别，不能推广为论文级
性能结论、设备性能、内存算法比较或并发加速比；扩大时长、负载分布与重复次数属 P3。

## 验收

- 普通全量 ctest：17/17，125.19 秒；原始逐用例输出在 p2_acceptance/ctest_lasttest.txt。
- 两个结构守门退出 0，I6 违反 0；见 p2_acceptance/validate_bplus.txt、validate_rtree.txt。
- ASAN 基准对照：512 条记录、每轮 200 次查询、8/128 帧、point/range，各三次，
  共 12 次全过；原始证据与版本哈希在 p2_asan/。
- 驱动协议测试 5/5，逐个删除每一项必填字段，并覆盖骨架、重复结果、非零退出、
  零操作、NaN、布尔伪装整数、参数错配、错误计数、只读写入等；见 driver_contract.txt。
- 可执行程序拒绝未知负载、零/负操作、坏种子、零记录、8K 页；拒绝重用已有库且
  数据/WAL 哈希未变；驱动拒绝延期实验并将六次实际超时记为整体失败。原始日志
  和各项预期退出码在 p2_acceptance/manifest.json。
- 分层和 ASCII 字面量检查通过。MSVC 沙箱内 C1902 通过允许访问调试信息服务的
  环境完成构建；没有改编译器安装或索引逻辑。

最终版本的基准程序需匹配各报告中的二进制哈希。report/bench/accept_p2.py 是本轮
验收入口，输出目录固定以保护首次证据，重复执行需选择新的输出目录或保留旧报告。
p2_verification.json 复核各次报告的源码/二进制、CSV 行数、17 个测试日志、六次
失败超时，并记录 RelWithDebInfo 构建缓存及 ASAN 开关，确认索引内核未改动。

## 后续

Hybrid 空间/包含/KNN 与固定混合读写已接入，后者实测见 [mixed 说明](mixed/README.md)；
下一项扩独立测量轨迹与数据量，并补明确的恢复/检查点实验；
页大小、预取、自动检查点继续延期。不得将配置开关存在或骨架 CSV 当作实验完成。
