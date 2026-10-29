# ltm-index 架构与开发约定

本文件是 `../终版方案.md` 的**工程版**：只保留写代码需要的部分。
方案原文（含立项论证、评审要点、实验设计）在 `../终版方案.md`。

---

### MSVC + Ninja 的头文件依赖（2026-10-06）

Ninja 依赖 `/showIncludes` 前缀识别头文件。中文 Windows 上原 CMake 缓存的前缀
出现乱码，导致依赖数为 0：修改头文件后，构建会错误地显示无需重建。
目前 CMake 为 MSVC/Ninja 设置 `scripts/msvc_compile.py` 编译启动器，统一 include
前缀和 UTF-8 输出；只有中文语言资源的编译器仅设置 VSLANG=1033 不足以解决问题。
该路径需要本机 Python，CMake 会显式查找；不影响 Visual Studio 生成器。
`scripts/accept_c1.py` 构建后检查测试框架头文件是否确实进入 Ninja 的依赖表。
触碰头文件的 dry-run 已确认触发重编译，证据见
`report/c1/acceptance/incremental_dependency_proof.txt`。

## 1. 分层与依赖方向

### 工程验收入口（2026-10-16）

`scripts/accept_mixed_measure.py --phase all` 顺序构建并验证普通、ASAN、Linux TSAN；
每轮新建构建及证据目录。`scripts/run_pipeline.py` 支持 Windows 的 MSVC 与 Linux
的 CMake/Ninja，程序后缀随平台选择。完整边界、重复参数和独立核对方法见
[工程验收](acceptance.md)。验收材料、源码快照和历史程序不得当作临时文件清除；
可生成缓存由 `scripts/clean_intermediates.ps1` 在检查路径、排除哈希证据后逐文件删除。

mixed 的 checked 与 measure 使用独立 schema；后者省去写后探测，仍保留完整
读结果参考和逐调用统计。`IndexStats.queries` 是成功 Get 加 Query 调用的统计，
不包含未命中 Get，不应泛称全部读请求数。此负载另记经过验证的未命中 Get，并
结合独立操作历史复算完整读请求及探测次数；未改核心统计语义。

KNN 固定工作量使用独立 `tools/knn_bench/` 构建，入口 `scripts/knn_bench.py`；
同一批查询按编号分摊给 1/2/4 个读者，完整答案在计时后校验，统计仅取线程全部
阻塞与全部完成两个边界。参考模型不调用生产距离/选择函数；原有 `ltm_bench`
协议保持不变。重开后先 Validate 填充实际结构页数，再判定驻留/淘汰；不能将
尚未填充的零页统计当作全驻留证明。测量边界及检测配置见
[工具说明](../tools/knn_bench/README.md)，当前限定矩阵见
[实测结果](../report/optimization/knn_matrix_20261016/README.md)。

### HybridIndex 并发边界（2026-10-06，P1；2026-10-08 更新 KNN）

同一个 HybridIndex 的完整公开操作受共享/独占接口门保护：Get、Query、Scan、Knn、
stats、is_open 与显式 DecayHeat 持共享门；Put、Erase、Open、Close、Checkpoint、
Validate 持独占门。Erase 读取旧 MBR 也包含在独占区间内。读者看到两棵树的一次
已提交逻辑状态；检查点与双树校验排空正在执行的操作。写者仍串行，长查询及其
回调会延迟写者。KNN 的遍历、候选结果与计数器按调用独立，多个 KNN 读者可以重叠执行。
BufferPool、热度表等共享组件仍有同步开销；本轮未测量吞吐量提升幅度。

调用关系是接口门 → 写事务锁/页闩 → 帧表锁 → 热度表锁。
其中 SnapshotStats 单独取写事务锁后取帧表共享锁；不取页闩。
取页路径先释放帧表锁再等待页闩；HotColdTracker 的锁是末端锁，持有时不调用
BufferPool 或获取页闩。Touch、Heat、DecayAll、扩容、释放、驻留标志、计数重置与
统计快照均由热度表自身的互斥锁保护。统计是分组件的值快照，读者计数持续增长时
不保证所有组件来自同一时刻；旧 pool.stats()/disk.stats() 引用接口仍需外部同步。

B+ 范围扫描、R 空间查询与叶链扫描先复制一页的命中记录，释放页闩和 pin 后调用
emit；临时载荷规模以一页为界。回调可以重入同一索引的读取与 stats；重入写入、
Open/Close、Checkpoint 或 Validate 返回 kInvalidArgument，避免共享门升级死锁。
这些扫描的 RecordRef 持有自持载荷；KNN 回调的载荷仍由查询结果缓冲持有。
保留通用回调结果时应物化记录，只有 owns_payload() 为真时复制 RecordRef 才能
同时延长载荷寿命。HybridIndex 的回调已经接收物化的 IndexEntry。
底层树直接调用仍按其原有并发契约执行，不获得 HybridIndex 的完整操作一致性。

2026-10-08 KNN 优化：移除 HybridIndex 覆盖整个 KNN 查询及回调的递归互斥锁。
RTree::Knn / KnnBruteForce 在调用内累积诊断计数，所有页 guard 和回调结束后才
短暂持有 knn_stats_mutex 发布；knn_stats() 在同一锁下返回值快照。该锁不跨越
取页或回调，不形成新的嵌套锁依赖。原始树的写入、Open/Destroy 与析构仍需调用方同步。
pages_visited / entries_scanned 表示最近结束的 best-first 调用，brute_pages 由最近
结束的暴力调用覆盖，并在 best-first 调用结束时重置；并发时不能用于归属某一次
查询或计算同一查询的剪枝率。遍历中错误、空树及提前停止也发布当次计数，参数
检查失败发生在计数生命周期之前。返回类型由引用改为值，使用方须重新编译。
三个新回归覆盖重叠回调与嵌套查询、有限帧重开后的并行 top-k、统计快照并发发布。
普通与 ASAN 全量测试及 Debug / TSAN 相关专项通过，证据与边界见
[KNN 优化说明](../report/optimization/knn_parallel_20261008/README.md)。

2026-10-16 KNN top-k 规则：候选选择与输出都使用 `(距离, 键)` 升序，不能仅按
距离拒绝边界候选。候选集用最多 k 项的大顶堆维护，堆顶是当前最差的完整排名；
同距但键更小的记录仍可替换堆顶。子树只有距离下界，因此只能在下界严格大于
堆顶距离时剪枝，不能排除同距子树。候选维护单次接纳为 O(log k)，最终排序
O(k log k)，候选载荷内存为 O(min(k, N)) 条记录；待访问节点队列另计，不宣称整个
查询仅 O(k) 内存。未按输入 k 预分配；k=0 保留原有按 1 处理的兼容行为。
暴力叶链扫描继续保留全量收集、全排序的独立选择路径，作为回归对照。
修复及本轮验证范围见 [top-k 优化记录](../report/optimization/knn_topk_20261016/README.md)。

pool()/disk() 暴露的原始对象不经过接口门；绕过公开 API 的访问、注册表动态修改、
跨索引回调形成的应用锁依赖及对象析构须由调用方同步。析构前必须停止所有调用者。
显式 DecayHeat 可以与读者及淘汰并行，未启动后台衰减或主动刷脏线程。

2026-10-07 生命周期补验修复：同一对象 Close 后重开，或失败 Open 后重试时，
先释放树及算子、检查点、缓冲池，再释放热度/策略、恢复对象和磁盘；旧缓冲池
析构仍需调用旧 WAL/disk，不能先替换这些依赖。旧资源的排空与重建均持独占
接口门，每次打开重置会话查询/写入计数。覆盖同对象多次重开与失败后重试的回归。

本轮验证入口：scripts/accept_c1.py --mode asan|debug --output-dir report/c1/heat/acceptance，
scripts/soak_hybrid_heat.py；结果见 report/c1/heat/README.md。该历史轮次未运行 TSAN，
ASAN、锁序检查与压测不替代数据竞争检测；当前 Linux 检测见下段。

2026-10-07 补充：已安装 WSL2 Ubuntu / Clang TSAN，运行库竞争正反对照通过，
当前检测结果见 report/c1/tsan/README.md，历史「无 TSAN」只指当时 MSVC 环境。
TSAN 发现派生状态回滚遗漏结构锁：RegisterRollback 的 undo 回调现在与根升降一样
持 structure_mutex 独占锁发布 root/height/leftmost_leaf/num_entries；即使值未改变也须同步。
回调在页回滚完成、帧表锁释放后执行；写事务锁仍在。读者仍只在取根快照时持结构共享锁，
释放后才等待页闩，保持原有锁顺序。新增不存在键删除的并发回归并保留修复前真实检测报告。
Hybrid 恢复夹具改用正常 Put 的页镜像与缺少最后 Commit 的 WAL 前缀；持原始 pool 写锁
反向进入 Hybrid API 不属于受支持的并发调用顺序，不能用该方式模拟崩溃或析构持锁对象。

```
L5 接口与组合层   src/index/hybrid_index.*  index_registry.*
L4 算子层         src/index/tree_ops.*  bplus_tree.*  rtree.*  mock_tree.*
L3 迁移层         src/migration/hot_cold_tracker.*  migration_policy.*
L2 持久化层       src/storage/wal.*  recovery.*
L1 缓存层         src/storage/buffer_pool.*  page.*
L0 设备层         src/storage/disk_manager.*
共享底座          src/util/{status,log,crc32c,types,config}.h
```

### 硬约束（由 `scripts/check_layering.py` 机器化强制）

| 规则 | 内容 |
| --- | --- |
| R1 | `src/index/**` **禁止文件 I/O**：不得出现 `<fstream>` / `fopen` / `pread` / `mmap` / `CreateFile` / `ReadFile` |
| R2 | `src/storage/**` 不得 `#include "index/**"`（分层单向） |
| R3 | `src/migration/**`、`src/util/**` 不得依赖 `index/**`；`util` 不得依赖 `storage/**` |
| R4 | 头文件禁止 `using namespace std;` |
| R5 | `src/bench/**` 与 `demo/**` 例外，可自由依赖 |

> 索引层允许 `#include "storage/disk_manager.h"` 与 `"storage/buffer_pool.h"` 的**声明**，
> 因为 `HybridIndex` 需要持有这两者的所有权并读取其统计；被禁止的是索引层自己发起 I/O。

### 提交前必做

```powershell
scripts\build.bat test                        # 编译 + ctest
python scripts\check_layering.py              # 分层检查
python scripts\check_ascii_literals.py        # 进控制台的字面量必须纯 ASCII（附录 C7）
python scripts\validate_tree.py               # 不变量自检（M2/M3 后为硬门槛）
```

---

## 2. 关键类型

| 类型 | 位置 | 说明 |
| --- | --- | --- |
| `PageId` | util/types.h | 4 字节页号，**唯一稳定的页标识**（不是文件偏移，页可搬迁） |
| `Lsn` | util/types.h | 8 字节日志序列号，崩溃恢复去重依据 |
| `Status` / `StatusOr<T>` | util/status.h | 统一错误传递，禁止用异常控制流程 |
| `PageHeader` / `PageTrailer` / `Slot` | storage/page.h | 页物理布局，**冻结契约**（`static_assert` 锁死尺寸） |
| `PageGuard` | storage/buffer_pool.h | RAII 页访问唯一入口，析构自动 Unpin |
| `IndexEntry` | index/index_entry.h | 统一逻辑记录：`key` + `MBR` + `payload` |
| `Query` | index/index_entry.h | 查询描述：点/一维范围/空间/相交/KNN |
| `TreePageOps` / `TreeOps` | index/tree_ops.h | 「通用」的核心契约，见下 |

---

## 3. 「通用」的定义（方案 D3）

抽象分两半：

- **`TreePageOps`（页内局部操作，无 I/O）**：`InitPage / Probe / InsertEntry / EraseEntry /
  IsOverflow / IsUnderflow / SplitInto / MergeFrom / BorrowFrom / ValidatePage`
  以及 M3 新增的路由/编码/定位挂钩 —— **树与树的差异全部收敛在这里**。
- **`TreeOps`（跨页算法，共享实现）**：下钻、分裂传播、下溢处理、根分裂/降级、
  页分配回收、latch 顺序、WAL 包围 —— 在 `src/index/tree_ops.cc` **只写一遍**。

### 通用性验收（硬指标）

新增一个树变体时：

```
src/storage/**            的 diff 必须为 0 行
src/index/tree_ops.cc     的 diff 必须为 0 行
```

`src/index/mock_tree.cc` 是这条规则的活样例：它是一个完整的第三种树变体
（纯内存实现，不碰页），只新增文件即可接入 `IndexRegistry`。
`tests/index/test_registry.cc` 运行期验证注册表；`scripts/check_layering.py` 静态验证分层。

### M3 之后：这条指标**已验证**，但只覆盖一半（诚实边界）

M3 加入 R 树时，跨页算法被真正上提到 `src/index/tree_ops.cc` 的 `IndexTree`
（17 行 → 1407 行，实测），B+ 与 R 都退化成「页算子 + 薄门面」。加入 R 树的实际 diff：

```
src/storage/**           0 行                    ← 指标成立（R 树未碰存储层）
src/index/tree_ops.cc    +R 树的挂钩调用（约 5 处分支），不是"0 行"  ← 见下
src/index/rtree.cc/.h    新增（R 树自己的文件）    ← 允许
```

> **口径澄清（M3 之后补记）**：上表的 "0 行" 指的是**「为加入 R 树这一个变体」所产生的
> 存储层改动**——这个数字是真的，且由 `check_layering.py` 的 R1–R6 持续保证（R 树不得
> 直接做文件 I/O）。它**不是**「`src/storage/**` 从此永不改动」：M3 之后有两处与树变体
> 无关的存储层改动，都不影响本指标——
> 1. `disk_manager.cc` 的 `SyncFstream` 补上真正的 fsync 设备屏障（此前 Windows 分支
>    只 `flush()`，不变量 I6 的物理一半并不成立）；
> 2. 源文件内字符串字面量的 ASCII 化（见 `docs/status.md`）。
> 判据是「新增变体是否需要动存储层」，不是「存储层的行数是否恒定」。

`tree_ops.cc` 并非"0 行"，原因是**第二个变体必然要修正第一个变体留下的隐含假设**：
本例中被修正的是「父页分隔条目只有一个来源」「借键四种情形下沉」「查找半边不能复用
单路径下钻」这三处。这些修正一旦完成，**第三个变体才是真正的 0 行**（`mock_tree.cc`
就是这样的第三例：它不碰页，因此根本没进这条路径）。

**共享面 = 结构维护半边**：插入下钻 / 分裂传播 / 借键合并 / 下溢循环 / 根升降 /
元数据 / `Validate` 骨架 / `Destroy`。

**查找半边不可共享**：`DescendToLeaf` 每层只走**一条**子路径 —— 这对插入成立
（按判据选出的去向是唯一的），对查找**不成立**：一个探测范围可能同时落在多个孩子里，
记录只存在其中之一。若 `Search`/`Erase`/`SpatialQuery`/`Knn` 复用单路径下钻，
**MBR 一旦重叠就漏记录**，而且不触发任何不变量报警（是"答案错"，不是"树坏了"）。
因此 `Search`/`Erase` 的定位与 `RangeScan`/`SpatialQuery`/`Knn` 由各树自己实现。

> 这条边界是被对抗性评审逼出来的：原计划宣称"跨页算法整层只写一遍"，
> 那会诱导下一个树变体去复用查找路径，从而引入一个静默漏记录的缺陷。
> 措辞收窄不是退让，而是把一个错误承诺改成一个可验证的承诺。

---

## 4. 页布局（冻结）

```
+--------------------------------------------------+ offset 0
| PageHeader (32B, POD)                            |
+--------------------------------------------------+ offset 32
| 槽目录 slot[k] 向下增长 ->                        |  每槽 4B: (offset:u16, len:u16)
|          ...                                     |
|          (free space)                            |
|          ...                                     |
| <- 条目区向上增长，条目紧凑排列                    |
+--------------------------------------------------+ offset 4080
| PageTrailer (16B): page_id_copy / crc / lsn_copy |  ← 撕裂页检测
+--------------------------------------------------+ offset 4096
```

### 三条容易踩的约定

1. **条目区自下而上生长**，槽目录自上而下；`free_space` 是两者之间的空隙。
2. **删除只清槽、不搬移条目**，废弃空间计入 `frag_bytes`，由 `Compact()` 回收。
   `Compact()` 会重建槽目录（`num_slots` 变小），因此 `free_space` 必须用
   `RecomputeFreeSpace()` **重算**而不是增量减法。
3. **页级算子不自动重算校验和**。`AppendEntry / InsertEntryAt / EraseEntry / Compact`
   改完页后校验和是旧的，由 `BufferPool` 在脏页 Unpin 时统一 `UpdateChecksum()`。
   绕过缓冲池直接改页的测试/工具必须自己调用 `UpdateChecksum()`。

`free_space` 的权威公式（`src/storage/page.cc` 的 `RecomputeFreeSpace`）：

```
free_space = kPagePayloadSize - num_slots * sizeof(Slot) - used_bytes
used_bytes = kPagePayloadSize - min{ slot.offset | slot 有效 }
```

---

## 5. WAL 记录格式（冻结）

**文件布局（M4-3 起）**：

```
+0                        +512                                    +wal_size
|  LogFileHeader          |  record, record, record ...           |
|  (128B 结构 + 补零)      |  (36B 记录头 + data)                  |
```

前 512 字节（`kWalDataStart`）是**保留区**，里面放 `WalLogHeader`：

```
+0  uint64 magic            "LTMWAL01"
+8  uint32 format_version   2（1 = M4-2 的无头部布局）
+12 uint32 header_bytes     保留区长度，自描述
+16 uint64 last_checkpoint_lsn
+24 uint8  clean_shutdown   1 = 上次干净关闭
+25 uint8  reserved[3]
+28 uint32 checksum         crc32c(前 28 字节)
```

头部自带校验和是硬要求：`last_checkpoint_lsn` 被写坏会让恢复**从一个错误的位置开始**，
而"少重放一段日志"是静默的。**非空但没有 magic 的文件一律拒绝**（`kCorruption`），
不猜起点 —— 把 M4-2 的记录当头部读，会把 LSN 读成一个随机数。

**记录（记录区，从偏移 512 开始）**：

```
+0  uint64 lsn            单调递增，全局唯一
+8  uint64 txn_id         逻辑事务号（一次索引操作）
+16 uint32 total_len      本记录总长度
+20 uint32 checksum       crc32c(本记录除 checksum 字段外全部字节)
+24 uint8  type           WalRecordType（含 M4-3 新增的 kClrRedo = 9）
+25 uint8  pad
+26 uint16 page_off
+28 uint16 data_len
+30 uint16 pad2
+32 uint32 page_id
+36 data[data_len]        after-image(REDO) / before-image(UNDO)
```

扫描器（`ForEachWalRecord`）逐条前移，遇到**长度不合法**或**校验和不符**就停：
后者就是"追加写到一半被强杀"的形态，也就是崩溃日志的尾部。所以扫描结果永远是
"可读前缀"，调用方不得假设它覆盖整个文件。

写路径：`BeginOp → AppendPageDelta(每页一条) → Commit`；
**提交前必须先 fsync 本事务全部记录，然后才允许修改数据页**（不变量 I6）。

### 5.0 一条页修改 = UNDO 记录在前、REDO 记录在后（**方向性问题，不是风格**）

崩溃矩阵抓到的反例（缺陷 #38，`wal_append n=40 / random`）：

```
VERIFY_FAIL validate: total entries in pages 6 != metadata count 5
```

最初写的是 REDO 在前。崩溃可以落在**两条记录中间**，于是日志里出现
"有 after-image、没有 before-image"的页：Redo 把未提交的插入重放上去，而 Undo
**根本不知道这一页需要回滚**（它的待回滚页集合是按 UNDO 记录定位的），
同时元数据页的 delta 排在更后面、压根没写进日志 —— 页里多一条、计数少一条。
数据看着是对的，不变量已经破了。

把 UNDO 写在前面，崩溃点落在中间时留在日志里的是 **UNDO 而没有 REDO**，这是安全方向：

* 这一页不进脏页表（没有 REDO），Redo 不碰它；
* Undo 找得到它，把 before-image 写回去；
* 而 before-image 正是这一页此刻**在盘上应当有的样子** —— 它在崩溃前一直被钉住，
  不可能带着未提交的改动落盘；单写者模型下 loser 必是最后一个事务，所以回滚
  也不会覆盖任何已提交的改动。

结论：两条记录只落一条的所有情形，在 UNDO 在前时都可恢复；REDO 在前时有一种不可恢复。
回归用例：`tests/storage/test_recovery.cc` 的 `G_EveryWalPrefixRecoversToAConsistentState`
（在**每一条记录边界**和**每一条记录中间**截断日志，逐个恢复并校验，152 个前缀全绿）。

### 5.2 恢复与检查点（M4-3）

```
RecoveryManager(options, disk)     构造：确保头部存在 → 建 WalWriter（续上 LSN 序列）
                                        → 把 clean_shutdown 清 0 并落盘（本次运行变"脏"）
Recover(pool)                      读头部 → Analysis → Redo → Undo → SyncPageFile
```

* **Analysis**：扫一遍日志，建 `dirty_page_table{pid → 该页最新 REDO lsn}`、
  `txn_table{txn → committed/aborted}`、`undo_first{(txn,pid) → 最早 UNDO lsn}`；
  遇到 `kCheckpointEnd` 就清空脏页表并把 Redo 起点前移到它 —— 这是"恢复时间不随
  日志长度线性增长"的唯一来源。
* **Redo**：只碰脏页表里的页，逐条应用 after-image，顺序固定为
  **写区间 → 置 `page_lsn` → 重算校验和**（CRC 覆盖 `page_lsn`，顺序反了会让每个
  被重放的页在下次打开时判成校验和不符）。`page_lsn >= rec.lsn` 跳过（幂等）。
  **损坏页不走幂等判断**：自描述不一致或校验和不符的页一律强制重放，否则"改坏但头尾完好"
  的页会被跳过、损坏永久留下。
* **Undo**：对"既没提交也没中止"的事务，按 (事务,页) 取该事务对该页**最早**的一条
  UNDO 的 before-image 写回，并写一条 **CLR**（`kClrRedo`，只有 REDO 半边）。
  CLR 不是装饰：它让被回滚的页获得一个比原更新记录更大的 `page_lsn`，于是**再次崩溃后
  再恢复**时 Redo 会正确地跳过那些已经回滚掉的更新。少了它，第二次恢复会把未提交事务的
  after-image 重新放回去，而且此后没有任何东西能再回滚它（事务已经写了 `kAbort`）。
* **干净关闭的短路**：打开时读到 `clean_shutdown == 1` 且没有 loser ⇒ 跳过 Redo/Undo。
  这是正确的（干净关闭时 FlushAll 已经把一切落盘），也是检查点能压缩恢复时间的同一原理。
  强杀永远走不到这里 —— `_exit(9)` 不会把 `clean_shutdown` 置 1。
  **注意判据取的是"打开时捕获的值"**：构造函数已经把盘上的标志清 0，在这里重新读只会读到 0。
* **检查点**：`begin 记录（记下偏移）→ pool->FlushAll() → end 记录 + fsync →
  更新头部 + 截断`。截断不能直接 `TruncateWal(keep_from)`（那会把保留区搬到偏移 0、
  冲掉头部），顺序是：读出要保留的字节 → `TruncateWal(WalSize())` 清空 →
  `PersistLogHeader()` 重写头部 → `AppendWal(kept)`。
  **不许在写事务中途做检查点**：截断会把这个事务的 UNDO 一起截掉，
  `RunCheckpoint` 因此直接拒绝（`write_op_active()`）。
* **恢复直接读写页文件，不经过缓冲池**，所以必须在任何索引打开之前调用；
  `Recover()` 在池里已有驻留页时会告警。
* **`kAllocPage`/`kFreePage` 记录会被写出但不重放**：空闲链的权威来源是页 0 自己的
  REDO 镜像（事务内的分配/回收都体现在它的 delta 里），两边都重放会重复施加。

### 5.1 写路径在 M4-2 之后的实际形态

上面的记录格式没变，但**谁在什么时候调用它**是 M4-2 定下来的，这里写死：

```
IndexTree::Open / Insert / Erase / Destroy    ─┐
RTree::Insert（Erase + Insert 的 upsert）      ─┤  ScopedWriteOp op(pool);
HybridIndex::Put（M5，两棵树）                 ─┘  ← RAII，析构必 Abort
                                                       │
                          BufferPool::BeginWriteOp()  ──┤ 可重入，只加深计数
                                                       │
  写页（mutable_page() 一律 MarkDirty）                 │
                          BufferPool::Unpin(pid,dirty) ─┤ 取 after-image
                                                       │  + memcmp 跳过未变页(D3)
                                                       │  + AppendPageDelta → 立刻追加
                          ... 操作内可能发生淘汰 ...    ─┤ FlushFrame：写盘前
                                                       │  若 page_lsn > flushed_lsn
                                                       │  先 wal->Sync()（写前规则）
                          CommitWriteOp()  ────────────┘ 最外层才真提交 + fsync
```

三条不能改的约束：

1. **`ScopedWriteOp` 必须是 RAII**。`Insert`/`Erase`/`Destroy` 里有十几处 `LTM_RETURN_IF_ERROR`
   提前返回；漏掉任何一处的手工 `Abort` 会让**深度计数泄漏**，于是**下一次操作**会join 进一个
   早已结束的事务 —— 症状与操作本身无关，极难定位。
2. **不能写 `return tree.Insert(...)`**。内层 `Commit` 只把可重入深度减回去，真正的提交要等外层；
   直接 `return` 会让本作用域的析构把它当成失败而 `Abort` 掉（`rtree.cc` 里 `RTree::Insert`
   的注释写明了这一点）。
3. **delta 采集必须在 `Unpin`，不能在 commit**。攒到 commit 再写，则小缓冲池里的页会在 commit
   **之前**被淘汰写盘，而它的 REDO 还不存在 —— I6 每次操作被违反上千次；且 commit 时帧可能
   已被复用，after-image 根本抓不到。语义自洽性在于：`Unpin(pid, dirty)` 的 `dirty` 位**恰好
   就是"缓冲池会写盘"的那个集合**，不脏的页不会落盘、对 I6 无害。

**delta 是整幅页镜像**（before + after 各一页），`PageImageDelta::offset` 字段**至今未用**。
方案曾把"只记变化区间"估成 20–50x，M4-2 实测只有 **1.2x / 1.3x**：槽目录从 payload 顶端
向下增长、条目区从底端向上增长，一次插入在页的两端都改字节。所以没做，且 M4-3 不要按
20–50x 规划日志体积（实测 ~8200 B/delta，一次 20000 操作的 B+ 压测就是 356 MB）。

---

## 5.3 混合索引（M5）

```
HybridIndex::Put(entry)                      HybridIndex::Get(key)
  ScopedWriteOp op(pool)                       ReadBPlusKey(key)        ← 一次 B+ 定位
    rtree->InsertReturningLeaf(entry)  ──┐       └─ 记录自带 key + MBR + payload
    bplus->Insert(key, ENCODE(mbr,payload))    （**不访问 R 树**）
  op.Commit()   ← 三层嵌套只提交一次         HybridIndex::Erase(key)
                                               ReadBPlusKey(key) → 精确 MBR
                                               ScopedWriteOp
                                                 rtree->Erase({key, mbr})
                                                 bplus->Erase({key})
                                               op.Commit()
```

**B+ 侧存的是整条记录**（`dim | payload_len | lo[dim] | hi[dim] | payload`），
R 侧存的是同一条记录的空间副本。这不是方案 §4.9 的原设计 —— 原设计是"B+ 叶条目只存
R 树叶页号，用时再去 R 树取"。**实测推翻了它**（缺陷 #41）：R 树的分裂会把一半条目搬到
新页，插入时记下的页号在批量装载后大面积失效（2000 条纯插入后命中率只有 50%），
每次失效要做一次 O(R 树叶页数) 的叶链全扫，于是 `Get`/`Erase` 有一半概率退化成全扫。
把 MBR 内联进 B+ 侧之后，`Get`/`Erase` **完全不访问 R 树**；叶页号降级为纯提示，
只给 `Validate()` 的 I8 交叉校验用（`RTree::Locate`：提示有效 O(1)，失效退化为全扫）。

**一致性（I8）**：`Put`/`Erase` 一个事务罩两棵树（`ScopedWriteOp` 可重入 ——
`RTree::Insert` 内部的 upsert 括号、两棵树的 `IndexTree::Insert` 都会 join 进它，
只有最外层写 kCommit）；`Validate()` 双向逐条交叉校验，报错带键号与原因；
崩溃矩阵用同一条判据（"恢复后全量遍历 == 旁路日志"）覆盖双树原子性。

**已知代价**：R 树的 upsert 仍是 O(叶页数)（它的路由键由 MBR 导出，同键旧记录可能落在
任何叶页里）；`Validate()` 是 O(n) 次定位（离线自检，不进热路径）。

---

## 6. 并发模型（三阶段）

| 阶段 | 模型 | 验收 |
| --- | --- | --- |
| C0（必达） | 全局 latch：读共享 / 写独占 | **已实测不可达，判据已改写**：原写"8 读线程线性扩展 ≥ 4×"，本机实测 **约 1.3–1.6×**，对照实验（零共享状态约 7–8×）证明瓶颈是共享缓冲池自身的原子热点而非机器或 latch 互斥。判据改为**正确性**：多读单写不撕裂 + 测量期零缺页 + 写线程真正串行化。详见 `docs/status.md` 的 M6-1 一节与 `report/m6_1/c0_read_scaling.csv` |
| C1（应有） | 页级 latch coupling，加锁序恒为根→叶；兄弟页恒为先左后右 | 约定范围已验收：Debug 连续 30 分钟无死锁/逆序、ASAN 全量、固定操作流对照、Linux TSAN 全量；具体版本及限制见 status.md |
| C2（可选） | 乐观读（版本号 + 重试） | 读吞吐相对 C1 提升 ≥ 30% |

写路径**永远单写者**（方案 D4），并发复杂度刻意集中在读路径。

> **历史 MSVC 验收口径（2026-09；当前已补 Linux TSAN）**：当时唯一可用的构建器是 MSVC，而 **MSVC 不支持
> ThreadSanitizer**（它只有 `/fsanitize=address`）。`CMakeLists.txt` 对
> `LTM_ENABLE_TSAN=ON + MSVC` 直接 `FATAL_ERROR`，不允许静默降级。
> 因此数据竞争的验收改为**这个工具链上真能跑的三条**：
>
> 1. `build-asan/` 全绿（越界 / UAF / 野指针；Windows 上 MSVC 无 LeakSanitizer，不覆盖泄漏）；
> 2. `build-dbg/`（断言在场）下启用 `LockOrderChecker`，加锁逆序立即 abort 并打印持锁链；
> 3. 混合读写压测 30 分钟**无死锁、无挂起**，且同一随机种子下多线程结果与单线程基准一致。
>
> 这三条覆盖不了"良性数据竞争"，这一点必须写进结题报告的"局限"，**不能记成
> "TSAN 干净"** —— 那正是 §7 里 ASAN 静默失效那条缺陷同一类的错误。

2026-10-07 已安装 WSL2 / Clang，新增真实 Linux TSAN 验收；该检测独立于上述三条，
必须先证明竞争对照能报警、同步对照干净，并保留全部插桩和原始输出。
当前状态及限定范围见 `docs/status.md` 与 `report/c1/tsan/README.md`。

---

## 7. 已知陷阱（踩过的坑，务必保留）

| 现象 | 真实原因 | 规避 |
| --- | --- | --- |
| MSVC 报成百上千条 "语法错误: 缺少类型说明符" 定位在头文件某行 | `add_compile_options(/utf-8)` 写在目标定义**之后**，`/utf-8` 没生效，MSVC 按 GBK 解析 UTF-8 源码，中文注释被拆成半个字符 | 编译选项段必须位于第一个 `add_library/add_executable` 之前 |
| 类内声明 `Status Query(const Query&)` 报 "假定为 int" | 成员函数名 `Query` **遮蔽**了同名类型 `ltm::Query`，参数类型解析成了成员函数名 | 类内一律写全限定名 `const ltm::Query&`；同类陷阱适用于任何与类型同名的成员函数 |
| `LOG_INFO("x=", 1)` 报 "项不会计算为接受 N 个参数的函数" | `LOG_INFO` 是函数式宏，预处理器吃下整对括号，把多余实参粘在展开式末尾 | `LogMessage` 提供变参 `operator()` 接住粘连实参（已在 `util/log.h` 实现） |
| `EXPECT_EQ` 报 `std::to_string` 无重载 | `std::to_string` 只有算术类型重载，遇 `enum class` / 指针即失败，且错误指向宏 | 测试框架用 `ostringstream`（`ltm::test::Show`）而非 `std::to_string` |
| `#include <functional>` 缺失但只在某些 .cc 报错 | `std::function` 是靠包含顺序"蹭"到的，换个包含顺序就炸 | 每个头文件必须自足：只用到的标准库头都要显式包含 |
| Ninja + MSVC 配置阶段长时间无输出 | CPU 探测/`showIncludes` 阶段本身慢，且被 `\| Select-Object` 缓冲住了输出，看起来像卡死 | 用 Visual Studio 生成器（`build-vs`），或让构建输出直接落文件 |
| 页只装了 28/32 条就报 `kPageFull`，随后 `Validate` 报 I3 填充不足 | `free_space` 是"最高地址 − 最低存活偏移"的**跨度**口径；分裂截断槽目录时留下的空洞被算成已用（实测 897 字节记成 3931） | 任何留下空洞的操作都要把腾出的字节记入 `frag_bytes` 并 `Compact()`；分裂点还要做合法性前置检查（存活数 < 2×阈值时拒绝分裂） |
| `ScanAll` 突然少几百条，而 `Validate` 全绿、点查正常 | 按"父页槽位相邻"修补叶链，把两页之间一整段叶页从链上摘掉了（父页槽位相邻 ≠ 叶链相邻，内部页借键会搬运槽位却不改变链上位置） | 修链必须按页自己记录的 `prev_page`/`next_page`，不能按父页槽号 |
| `Erase` 报"记录不存在"，但同一棵树 `ScanAll` 与真值表逐条一致 | 多维树的路由天然有假阳性候选；`EraseRec` 在某条候选子树走到"死胡同"时报 `kNotFound`，调用方 `LTM_RETURN_IF_ERROR` 直接中断，兄弟候选从未被访问 | "不在这里"要返回 Ok（或显式吞掉 `kNotFound`）并继续试兄弟候选；只有全部候选都试完才报 `kNotFound` |
| `num_entries` 比真值表大，但不变量全绿 | 多维树的插入去向由 MBR（不是 key）决定，页算子只能做**页内**去重，同键旧记录可能落在别的叶页 → 跨页幽灵副本 | `Insert` 的契约是"同主键覆盖"，通用/门面层必须做全局 upsert，不能依赖页算子 |
| 用 `EXPECT_EQ` 比较 `std::set` 编译失败 | `EXPECT_EQ` 要流式输出两侧操作数，而 `std::set` 没有 `operator<<` | 改用 `EXPECT_TRUE(a == b)`，失败时自己打印差异；同理适用于任何无 `operator<<` 的类型（`enum class`、指针同理） |
| 「断言在 Debug 下一定在场」——听起来不证自明 | `CMAKE_CXX_FLAGS_DEBUG` 里没有 `/DNDEBUG` **不等于**编译器真的收到了这个选项（缺陷 #37 就是"开关写对了、编译器没收到"） | **不要拿 flag 字符串当证据**。用一个临时 `assert(false)` 探针同时在两种配置下跑：Debug 退出码 3 且打印 `file:line`，RelWithDebInfo 退出码 0，才算证明。见 `report/m4_2/phase4_structural_and_debug.txt` |
| 「写盘前先自动 Sync，再断言 `page_lsn <= flushed_lsn`」——这个 I6 检查**永远通过** | `Sync()` 把 `flushed_lsn` 置为**本次运行已分配的最大 LSN**，于是该断言在本运行内恒真，**只在跨运行的陈旧 LSN 上响** —— 只在误报时响、对真 bug 全盲 | 拆成两条：① 写前规则（`page_lsn > flushed_lsn` → 先 Sync，**计数**，非零是正常的性能指标）；② 可证伪断言（事务内标脏的页落盘时 `page_lsn == kInvalidLsn` → 拒绝 + 污染池）。断言响了必须把池标 **poisoned**，否则 `~BufferPool` 的 `FlushAll` 会把改了一半的页写下去 |
| `guard.page().mutable_header()` 能编译，于是漏标脏 | `Page` 只是指针包装，返回非 const 版本时"写页但不标脏"在类型系统里完全合法 | `PageGuard::page()` 返回 **`const Page`**，想拿可写 `Page` 只有 `mutable_page()` 一条路，而它必定 `MarkDirty()`（M4-2 D9）。注意边界：`Page p = guard.page();` 仍可编译（拷贝出来是非 const），这是指针包装设计的固有边界 |
| `page.h` 注释称校验和"不含 `page_lsn`" | 注释与实现不符：实际是 `Crc32c(raw_, kPageSize - kPageTrailerSize)`，**包含整个 32B 页头（含 `page_lsn`）** | 已改正。对重放是硬要求：**写区间 → 置 `page_lsn` → 重算校验和**，照旧注释做会让每个被重放的页在下次打开时判为校验和不匹配 |
| 压测/守门同时跑，报出**真实**的 `I6 violated`，看起来像屏障崩了 | 两个 validator 用的是**硬编码相对路径** `log/_validate_<kind>.{data,wal}`：并发运行时一个进程删掉又重建页文件，另一个进程看到"页文件存在但全零"，走了重开路径 | 并发的验证运行必须**各有自己的工作目录**（`scripts/soak_m4_2.sh` 就是这么做的）；守门脚本继续用仓库根的 `log/` |
| 压测某一轮的所有判据同时变红（"计数没打印""没有 delta"…），但 WAL 有 200 MB | 那一轮的输出文件被**在进程持有它时删掉**了：写到已 unlink 的 inode，读回来是空的 | 驱动脚本的临时文件**只能放在自己的私有目录**里；另外把"输出为空但退出码 0"单独判成一类（`EMPTY_OUTPUT`），别让它伪装成计数检查失败 |

---

## 8. 构建

```powershell
scripts\build.bat configure      # 配置（Visual Studio 2026 生成器，输出到 build-vs/）
scripts\build.bat build          # 编译
scripts\build.bat test           # 编译 + ctest -E validate（10 项全绿）
scripts\build.bat faultinject    # 配置并编译带故障注入的 build-fi/（崩溃测试用）
scripts\build.bat ninja          # 可选：Ninja 生成器（见下方警告）
scripts\build.bat clean          # 清理全部构建目录
```

**`cmake` 不在 `PATH` 上**（它随 Visual Studio 安装）。`scripts\build.bat` 自己会调
`vcvars64.bat` 所以没问题；Python 脚本则用 `scripts\validate_tree.py` 的 `find_cmake()`：
先看 `CMAKE_EXE` 环境变量，再看 `PATH`，最后按 VS 的常见安装位置 glob。
找不到时报的是"请设置 `CMAKE_EXE`"，**不会再以 `FileNotFoundError` 的 traceback 收场**。

> **为什么默认用 Visual Studio 生成器而不是 Ninja**：本机 Ninja 生成器在 CMake 的
> MSVC ABI 探测阶段（`cl /showIncludes` 试编译）会连续数分钟无任何输出，看上去像卡死
> （实测进程 CPU 几乎不动，`build/.ninja_lock` 长期存在）。换 VS 生成器后配置稳定在 ~20s。
> 这不是死锁，但会严重拖慢开发节奏，因此把 Ninja 降级为可选项。
>
> 另外 `build.bat` 必须是**纯 ASCII 且无 BOM** 的文件：cmd.exe 会按当前代码页解析批处理，
> 带 BOM 时首个命令会变成 `﻿@echo off` 而报 "'' 不是内部或外部命令"。

可执行文件统一输出到构建目录**根**（`CMAKE_RUNTIME_OUTPUT_DIRECTORY = CMAKE_BINARY_DIR`），
即 `build-vs\ltm_smoke.exe`，而不是 `build-vs\RelWithDebInfo\ltm_smoke.exe`。

手动方式：

```powershell
& "D:\visual studio\IDE\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
  -S . -B build-vs -G "Visual Studio 18 2026" -A x64
& "...\cmake.exe" --build build-vs --config RelWithDebInfo
& "...\ctest.exe" --test-dir build-vs -C RelWithDebInfo --output-on-failure
```

**三个构建目录**，各自的用途不可互换：

| 目录 | 配置 | 用途 |
| --- | --- | --- |
| `build-ninja/` | RelWithDebInfo（Ninja） | 本机日常与守门的默认构建（见下面的环境说明） |
| `build-fi/` | RelWithDebInfo + `LTM_ENABLE_FAULT_INJECT=ON` | 崩溃矩阵（`ltm_crash_test` 只在这个构建里存在） |
| `build-asan/` | ASAN=ON | 内存错误（越界/UAF/野指针）；Windows 上 MSVC 无 LeakSanitizer，**不覆盖泄漏** |
| `build-dbg/` | Debug（`/Zi /Ob0 /Od /RTC1`，**无 `/DNDEBUG`**） | `assert()` 真正在场的构建；M4-2 的 1 小时压测跑的就是它 |
| `build-vs/` | RelWithDebInfo（VS 生成器） | 本机可用（`reg.exe` 可用），与 `build-ninja/` 等价，见 §8.1 |

### 8.1 本机的构建环境（迁回本机后按实测重写）

本机项目根为 `D:\db_project\LTM\ltm-index`，一条条实测的事实是：

1. **项目路径是纯 ASCII**。`cmd.exe` 与 bash 都不会在命令行上做代码页往返，因此
   `scripts\build.bat` 里的 `pushd "%~dp0.."` **可以正常工作**。
2. **Visual Studio 装在 `D:\visual studio\IDE`**（`VC/` 与 `Common7/` 都在这一层，
   不在盘的顶层目录）。生成器名 `Visual Studio 18 2026`，工具集 MSVC 14.51.36231。
3. **`reg.exe` 可用**，VS 生成器与 `vcvars64.bat` 都能正常定位 VS 实例与 Windows SDK。

所以本机**两条构建路径都等价可用**：

```bash
python scripts/build_local.py configure      # 或 build / test / all（Ninja，推荐）
python scripts/build_local.py fi             # 故障注入构建（崩溃矩阵用）
python scripts/build_local.py asan           # AddressSanitizer 构建
python scripts/build_local.py debug          # Debug（断言在场）
scripts\build.bat test                       # 同上，走 VS 生成器那条路
```

> **`scripts/build_local.py` 为什么是"Ninja + 自己扫盘发现 VS/SDK"这种写法**：
> M4-3/M5 是在**另一台机器**上完成的，那台机器上项目路径含中文
> （`D:\年度项目\LTM\ltm-index`）、VS 装在 `D:\VS产品`、安全策略禁止 `reg.exe` 启动
> —— 三条叠加使 VS 生成器那条路完全不可用（`vcvars64.bat` 会定位不到 Windows SDK，
> 得到一个"能跑但找不到 `crtdbg.h` / `stddef.h` / `stdio.h`"的编译器）。
> 那台机器上的三条约束在本机**都不成立**，但 `build_local.py` 仍然保留为推荐入口：
> 它不依赖注册表，且能用一条命令在三种配置之间切换。
>
> 一处因此而来的改动：`find_vs_root()` 原先只扫各盘**顶层**目录找 `<盘>:\VC\Tools\MSVC`，
> 而本机的 VS 在 `D:\visual studio\IDE\` 下**多一层**，会扫不到并报
> `cannot find a Visual Studio root with cl.exe and cmake.exe`。已改为向下多探一层子目录。

```bash
python scripts/build_local.py configure      # 或 build / test / all
python scripts/build_local.py fi             # 故障注入构建（崩溃矩阵用）
python scripts/build_local.py asan           # AddressSanitizer 构建
python scripts/build_local.py debug          # Debug（断言在场）
```

`scripts\build.bat` 保留（VS 生成器那条路），路径已同步到新目录；在 `reg.exe` 可用的机器上
两者等价。

> **跑 ASAN 之前必须让 ASAN 运行库在 `PATH` 上**：它随 MSVC 发布，在
> `VC\Tools\MSVC\<版本>\bin\Hostx64\x64\`。`build_local.py` 会把这个目录加进子进程的 `PATH`，
> 所以 `python scripts/build_local.py test build-asan` 能正常跑；**从裸 shell 直接敲
> `build-asan\xxx.exe` 会报 `0xc0000135`** —— 那是环境问题，不是代码问题。

> **为什么需要单独的 `build-dbg`**：Debug 本来就是 VS 生成器的可用配置，但 `CMakeLists.txt`
> 把所有配置的可执行文件都强制输出到 `CMAKE_BINARY_DIR`（见下），所以 `--config Debug`
> 会把守门脚本依赖的 RelWithDebInfo 二进制**逐个覆盖掉**。宁可多一个目录，也不动输出目录规则。
>
> **同上**：`build-asan/` 直接用仓库根的 `log/` 与同一批 `log/_validate_*` 文件名。
> 它**不会**与压测撞车（压测已跑在私有目录里，见 §7 的"压测/守门同时跑"一行），
> 但**两个守门运行之间**仍然会撞：不要同时跑两次 `validate_tree.py`，也不要在一个守门
> 运行期间手工敲 `build-*/ltm_validate_*.exe`。守门程序彼此之间没有隔离。

**跑 ASAN 之前必须让 ASAN 运行库在 `PATH` 上**。MSVC 的 `/fsanitize=address` 是**动态**运行库，
链接出来的 exe 依赖 `clang_rt.asan_dynamic-x86_64.dll`；找不到时**整个测试会以
`0xc0000135`（STATUS_DLL_NOT_FOUND）失败，看起来像 10 个用例全挂**，实际一行用户代码都没跑。

```bash
# VS 开发者命令行（vcvars64）里直接跑即可；或在 Git Bash 里显式加：
export PATH="/d/visual studio/IDE/VC/Tools/MSVC/<版本>/bin/Hostx64/x64:$PATH"
ctest --test-dir build-asan -C RelWithDebInfo -E validate
```

这是 #37 的同类问题（"门槛看起来跑了"与"门槛真的跑了"不是一回事）：**`0xc0000135` 不是测试
失败，是运行库没找到**，遇到它先查 `PATH`，不要去看失败用例的断言。

### ctest 用例构成（12 项）

| 用例 | 说明 |
| --- | --- |
| `test_status` `test_crc32c` `test_config` | util 层单测 |
| `test_page` | 页格式单测 + 属性测试（含随机操作与压实精确性） |
| `test_recovery` | **M4-3 崩溃恢复**：A 干净关闭无需恢复 / B 未提交事务被回滚 / C 恢复可重入 / D 撕裂页与坏校验和被修回 / E 检查点截断且恢复从 checkpoint_lsn 起 / F 日志头往返与格式判据 / G **152 个日志前缀逐个恢复**（7 项） |
| `test_hybrid_diff` | **M5 混合索引**：A 往返 / B 随机差分 / C 一维范围 / D 空间与 KNN / E Get 不访问 R 树 + 提示失效被兜住 / F 关闭重开 / G 检查点 / H 未提交 Put 双树一起回滚 / I I8 交叉校验可证伪（9 项） |
| `test_registry` | 通用性注册表 + MockTree 活样例 |
| `test_tree_ops_contract` | 契约冻结测试（默认实现、维度校验、策略解析） |
| `test_bplus_diff` | B+ 差分：顺序/随机/删空重插/区间/重复键/纯插入填充率/小池重开（7 项） |
| `test_rtree_diff` | R 树差分：增删对拍、空间查询对拍暴力、KNN 对拍、删空重插、小池重开 + 3 个回归钉子（9 项） |
| `test_wal_barrier` | **M4-2 写屏障**：A 无 REDO 的脏页被拒 + 池被污染 / B 同一页在事务内放行且**从盘上读回**验证 `flushed_lsn >= page_lsn` / C 真实负载后从**磁盘文件**逐页读回、每页带非零 `page_lsn`（3 项） |
| `test_smoke` | 集成冒烟：热度衰减、驱逐打分单调性、`HybridIndex::Open` 默认配置可开 |
| `validate_bplus` `validate_rtree` | 不变量自检（**两棵树都已退出码 0**）；带 900s 超时且属"守门"性质，`build.bat test` 用 `-E validate` 排除，单独用 `python scripts\validate_tree.py` 跑（顺带产出 `report/rtree_split_policy.csv`） |

### 崩溃矩阵（M4-3 起）

```bash
python scripts/build_local.py fi          # 必须：ltm_crash_test 只存在于故障注入构建里
python scripts/crash_matrix.py            # 42 个用例，产出 report/recovery_matrix.md
python scripts/crash_matrix.py --list     # 只看矩阵规模
```

判据是"恢复后的全量遍历 == 旁路日志里记录的**已提交**状态"，且结构不变量全绿。
旁路日志（`journal.txt`）在每次操作返回后立即追加并 fsync，而注入点全在树操作内部，
所以"崩溃"不可能落在"操作已提交但日志还没写"的窗口里 —— 这是判据成立的前提。
每个用例跑三个进程（work → 可选 corrupt → verify），由 Python 用 Unicode 传路径。

**两个引擎跑同一条判据**（`--engine=bplus|hybrid`）：单棵 B+ 树与混合索引。
对混合索引，这条判据还额外覆盖 I8 —— 两棵树要么都回来、要么都没回来。
矩阵规模：78 例（其中 36 例混合索引），其中 72 例真的把进程强杀了。

> `-E validate` 过滤掉的就是最后一行那两个**门槛程序**：它们各自要几十秒，
> 由 `scripts\validate_tree.py` 单独跑并把证据留到 `report/`。

### 长时压测（M4-2 起）

```bash
bash scripts/soak_m4_2.sh 3600 build-dbg     # 1 小时，Debug 构建（断言在场）
```

逐轮跑两个 validator，**每轮开跑前删日志**（WAL 永不截断、检查点未实现，一次长跑无界增长），
并把每条判据写进 `report/m4_2/soak_progress.txt`。两条自保设计见脚本头部注释与 §7：
进程的工作目录是**私有的**（不与守门脚本抢 `log/`），且每轮有超时
（没有超时的话，"挂起"与"跑得慢"在输出上无法区分 —— 压测会静默地什么证据都不产出）。

### 编译选项（`CMakeLists.txt`）

- MSVC：`/W4 /permissive- /utf-8 /Zc:__cplusplus /WX`
- 抑制：`/wd4127`（常量条件）、`/wd4702`（不可达代码，占位实现的固定形态；
  **M1..M6 逐个实现到位后应删除这一行**）
- 开关：`LTM_ENABLE_FAULT_INJECT` / `LTM_ENABLE_ASAN` / `LTM_ENABLE_TSAN` / `LTM_WERROR`
