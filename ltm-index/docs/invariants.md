# 系统不变量清单（Invariants）

这份清单是**评审、agent、测试三方的公共语言**：评审按此逐条问"你怎么保证的"，
每条都必须对应一个可执行的测试或断言。任何一条被破坏都属于 `StatusCode::kCorruption`。

命名与方案 `../终版方案.md` §5.3 一致。

| ID | 不变量 | 保障手段 | 对应用例 / 位置 |
| --- | --- | --- | --- |
| **I1** | 非叶页第 i 键 == 第 i 子页最小键 | 分裂/借键/合并后统一刷新父页分隔键（`RefreshSeparatorsForChild`，会刷新**所有**指向该子页的槽）+ `ValidatePage` | ✅ `BPlusTree::Validate` → `BPlusPageOps::CheckParentKey`；`ltm_validate_bplus` 退出码 0；`test_bplus_diff.cc` 6/6 |
| **I2** | 叶链 `next_page/prev_page` 双向一致且键严格升序 | 全量遍历断言（跳过墓碑槽，按存活槽计数） | ✅ `BPlusTree::Validate`；`test_bplus_diff.cc` 全量对拍 |
| **I3** | 任意非根页填充率 ≥ 50%（B+）/ ≥ 40%（R） | 删除路径循环借键/合并直到补足；`TreeStats.avg_fill_rate` + 校验程序 | ✅ `ltm_validate_bplus`（平均填充率 63.2%）；✅ `ltm_validate_rtree`（装载后 62.1%） |
| **I4** | 根到所有叶路径等长 | BFS 逐页校验 `level == parent.level - 1`，再与 `stats().height` 交叉验证 | ✅ `ltm_validate_bplus` / `ltm_validate_rtree` |
| **I5** | 页自描述：`header.page_id == trailer.page_id_copy` 且 `header.page_lsn == trailer.page_lsn_copy` | 每次 `Page::Validate()`；另加"取回的页必须自报家门"（`Fetch` 断言 `page_id == 请求页号`） | ✅ `test_page.cc`: `ValidateDetectsHeaderTampering` / `ValidateDetectsTornPageLsnMismatch` |
| **I6** | 写前日志：任何脏页落盘前，其 REDO 已 fsync | 物理屏障：`WalWriter::Commit()` 无条件 `Sync()` 后才返回，且 `DiskManager::SyncFstream` 在 Windows 走 `_commit`（`FlushFileBuffers`）、POSIX 走 `fsync`（失败返回 `kIoError` 而非静默 `Ok`）。M4-2 起再加**两条独立机制**：① **写前规则**（始终执行）—— `BufferPool::FlushFrame` 写盘前若 `page_lsn > flushed_lsn` 就先 `wal->Sync()`，计入 `wal_sync_on_flush`；② **可证伪断言**（`BufferPoolOptions::strict_i6_barrier`，默认开）—— 事务内标脏的页落盘时必须带非零 `page_lsn`，否则计 `i6_violations`、返回 `kCorruption` 并把池标为 **poisoned**，之后 `FlushPage`/`FlushAll`/析构一律拒绝再写 | ✅ 物理屏障：缺陷 #27 / #34 已修。✅ 接线与断言（M4-2）：`test_wal_barrier.cc` 三例（A 拒绝 / B 允许并**从盘上读回**验证 `flushed_lsn >= page_lsn` / C 真实负载后从**磁盘文件**逐页读回、每页带非零 LSN）；`ltm_validate_bplus` / `ltm_validate_rtree` / `ltm_demo` 的 `WAL` 行；Debug 压测 1 小时无触发（`report/m4_2/`）。**仍缺**崩溃恢复（M4-3） |
| **I7** | 空闲页不双分配：`kFreePage` 后该 pid 不再被任何页引用 | 合并/根降级先改父页再释放；`NewPage` 复用空闲页时复用同一帧并校验未被钉住；空闲链状态随页 0 的 REDO 镜像一起被重放 | 恢复后遍历引用集与空闲链求交为空（M4-3 只做到"页 0 镜像被重放"这一层，见下面的口径） |
| **I8** | 混合索引双树一致：B+ 树存在的 key 在 R 树必可定位（且两棵树对同一条记录的 MBR 与载荷一致） | `HybridIndex::Put`/`Erase` 用**一个写事务**罩住两棵树（`ScopedWriteOp` 可重入，只有最外层提交）；`Validate()` 做**双向**逐条交叉校验（B+→R 与 R→B+），不一致时报出具体键号与原因 | ✅ M5：`test_hybrid_diff` 9/9（A 往返 / B 随机差分 / C 一维范围 / D 空间与 KNN / E Get 不访问 R 树 / F 重开 / G 检查点 / H 未提交 Put 双树一起回滚 / **I 交叉校验可证伪**）；崩溃矩阵 78 例全过（其中 36 例 `--engine=hybrid`） |
| **I9** | 父页第 i 槽的 MBR **恰好等于**其第 i 个子页全部条目 MBR 的并集（不是"包含"，不能只增不减） | 自底向上递归校验：`RTreePageOps::CheckParentKey` 逐维比较"父页存的 lo/hi"与"遍历子页算出的并集"，任一维不等即 `kCorruption`；并集由 `MBR 并集累加器`（`rtree.cc:245`）唯一提供，`BuildSeparator` 与校验共用同一实现 | ✅ `ltm_validate_rtree` 退出码 0（M3） |
| **I10** | 崩溃恢复后全量校验通过且已提交数据不丢 | 崩溃矩阵（故障注入构建）：恢复后全量遍历必须**逐条等于**旁路日志（`journal.txt`）里记录的已提交集合，且 `Validate()` 全绿 | ✅ M4-3：`report/recovery_matrix.md` 42 例全过（其中 36 例真的崩了）；`test_recovery.G` 另有 152 个日志前缀的确定性版本 |

## I6 的落地口径（M4-2 新增）

I6 是**唯一一条无法靠"多测几遍"取信的约束**：屏障失效时程序照常跑完、照常读出正确结果，
只有断电才暴露。所以 M4-2 把它拆成两条**必须分开**的机制，而不是一条"自动补齐"的逻辑。

### I6 的唯一豁免（M4-3 定死）

**豁免判据只有一条：日志里一条记录都没有**（`wal->last_lsn() == kInvalidLsn`）。
那正好是"建库第一步"的精确判据 —— 页 0 刚被 `Init`，此刻不可能有任何 REDO 覆盖它。

M4-2 曾用"数据文件是本次新建的"作为豁免（`FlushFrame` 的 `enforce_i6` 参数），
M4-3 把它删掉了：那个判据与"这一页确实没有 REDO 覆盖"**并不等价** ——
数据文件被删掉、日志却留着时，它会让 I6 在最需要它的时候失明。
现在 `FlushFrame` 没有豁免参数，判据只有上面那一条，而且它不可能被"顺手同步"伪造
（`Sync()` 改变不了 `page_lsn` 是不是 0）。

配套的一处修正：`LoadMeta` 在"新建库"分支里重写页 0 时**保留 `page_lsn`**。
恢复可能把一个全零的页 0 留在盘上、同时给它盖上一条 CLR 的 LSN（未提交的建树事务被回滚），
若这里把 LSN 抹成 0，紧接着的落盘就会撞上 I6 检查 —— 而那个 LSN 恰恰是唯一能解释这一页的记录。

### 为什么"写盘前自动 Sync"不能兼任检查

`Sync()` 会把 `flushed_lsn` 置为**本次运行已分配的最大 LSN**。于是"先 Sync 再检查
`page_lsn <= flushed_lsn`"这个写法，在数学上**对本运行内的任何违反都是恒真的** —— 它只会在
跨运行的陈旧 LSN 上响，也就是**只在误报时响、对真 bug 全盲**（方案 D7 的评审结论）。

因此两条机制分开，各自的判据不同：

```
① 写前规则（production 也执行，计数 wal_sync_on_flush）
   page_lsn > flushed_lsn  →  wal->Sync()  →  然后写盘
   Sync 修得好"REDO 还没 fsync"，这是它该做的事。计数非零是**正常**的
   （事务中途淘汰脏页必然发生），它是性能指标，不是正确性指标。

② 可证伪断言（strict_i6_barrier，默认开，计数 i6_violations）
   事务内标脏的页落盘时 page_lsn == kInvalidLsn  →  拒绝写 + 池标 poisoned
   Sync **修不好**这件事：它只能把 flushed_lsn 往前推，给不了页一个 LSN 戳。
   这一条才是"页被写过、却没有任何 REDO 覆盖它"的真缺口。
```

### poisoned 是必需的，不是保险

断言响了之后，内存里已经有一半改过的页；若放任 `~BufferPool` 的 `FlushAll` 把它们写下去，
磁盘上就会出现**没有任何 REDO 能解释的页** —— 这正是 I6 要防的状态，而且是在"已经发现异常"
之后才造成的。所以池被污染后 `FlushPage` / `FlushAll` / 析构**一律拒绝再写**。

### 边界：页分配/回收不进日志（M4-2 有意为之，方案 D10）

`NewPage` / `FreePage` 不写 WAL。这不是疏忽，而是 M4-2 的划界；但它的直接后果是
**公开 API 里存在一条真实的 I6 违反路径**：事务外 `NewPage` 出来的页带着
`page_lsn = kInvalidLsn`、脏、且终将落盘。`test_wal_barrier.cc` 的用例 A 正是用这条真缝
构造违例的 —— 没有测试专用开关、没有 mock，因此它证明的是"生产路径上屏障会拦"，
而不是"我按测试开关时的行为符合预期"。

### 跨运行 LSN 单调（D8）

`WalWriter` 构造时若 WAL 文件非空，先扫出 `max_lsn` 再令 `next_lsn = max_lsn + 1`。
不做这一步，重开后旧页带着上一轮的 `page_lsn` 而 `flushed_lsn` 从 0 起，断言**必然误报** ——
而重开路径就在测试套件里（两棵树的 diff 测试与 `ltm_validate_rtree` 都有重开用例）。

### 仍然不覆盖的（M4-3）

I6 只保证"脏页落盘前其 REDO 已持久"。它**不做任何重放**：没有 Analysis/Redo/Undo。
提交后崩溃不丢数据；事务中途崩溃之所以安全，仅仅因为那个事务从未提交。I7 的
"空闲页不双分配"因此也仍待 M4-3 的恢复后引用集校验。

## I9 的落地口径（M3 新增，措辞经过收窄）

方案 §5.3 原文写的是"精确包含"，实现时**按"精确相等"落地**，两者不是一回事：

```
断言：父页第 i 槽存的 MBR  ==  ⋃{ 子页 i 中每条存活条目的 MBR }     （逐维 lo/hi 相等）
不是：父页第 i 槽存的 MBR  ⊇  子页 i 的全部条目 MBR
```

理由：**"包含"是一个无法被证伪的弱断言**。父页 MBR 只增不减时它永远成立
（删除记录后不收缩也照样通过），于是校验通过、结构却已经漂移 —— 父 MBR 比真实并集大
会让查询多访问一些页（不致命），但真正的问题是**没有任何检查能发现"该收缩没收缩"**，
I9 就退化成一句空话。取"相等"后，多存/少存一个维度都会立刻 `kCorruption`。

三个配套约束，缺一不可：

1. **并集只有一处实现**：`rtree.cc:245` 的并集累加器，`BuildSeparator`（写）与
   `CheckParentKey`（校验）共用它。两处各算一遍必然漂移。
2. **`BuildSeparator` 必须在每次子页条目集合变化后调用**，且**两个方向都要**——
   删除后要收缩，借键时接收方与供体**两页的并集都变了**，只刷一侧是典型缺陷。
3. **空子页的约定**：`BuildSeparator` 对空页给出"零 MBR"，`CheckParentKey` 对空子页直接放行
   （两边同一约定）。否则根降级/删空重插路径会在"0 条子页"上误报。

I9 与 I1 是同一件事在两种树上的两种形态：B+ 的父页存**子页最小键**（一维、`CheckParentKey`
比较键相等），R 树存**子页 MBR 并集**（多维、精确相等）。二者都由
`TreePageOps::BuildSeparator` 这一个钩子作为唯一写入点 —— 这正是 D3 通用性指标在
"父页分隔信息"这一处的体现。

> 另注：I3 的 R 树阈值是 **≥ 40%**（不是 50%），且判据是**字节**口径
> （`used_bytes`）而非槽位数：多维树的条目变长，按槽位数会得出"槽位够但字节装不下"的
> 不可满足下溢判据，`Rebalance` 不进展、`Validate` 误报。B+ 树条目近似等长，按槽位即可。

## 页级记账恒等式（每一处页修改后都必须成立）

```
num_slots * sizeof(Slot) + free_space + used_bytes == kPagePayloadSize     (32 + 4032 = 4096)
used_bytes == kPagePayloadSize - min{ slot.offset | slot 有效 }
frag_bytes == Σ{ 已删除槽的原 len }        （Compact 后归零）
used_bytes >= Σ{ 存活条目 len }            （含碎片空洞；Compact 后取等号）
```

验证：`test_page.cc` 的 `RandomOperationsPreserveInvariants`（3000 步随机操作，逐字节断言）
与 `CompactionExactnessAfterRandomOps`（压实后取等号）。

## 数值安全约定

| 约定 | 原因 |
| --- | --- |
| `Mbr::Area()` 对非法/退化 MBR 返回 `0.0`，绝不返回 `inf`/`NaN` | `inf * 0 = NaN` 会让"最小面积增量"比较全部失效，选叶策略静默退化为"永远选第一个候选" |
| `Mbr::EnlargementToInclude()` 对非法/空 MBR 返回 `0.0` | 同上；调用方应先 `Validate()` |
| `Crc32cHasher::value()` 幂等，可重复调用 | 便于一页里多次校验 |
| 热度计数器饱和在 `uint8` 上限 255 | 防止长时间热点溢出回绕成冷页（`HotColdTrackerHeatSaturates`） |

## 驱逐策略的语义约定（`MigrationPolicy::EvictScore`）

```
分值越小 == 越应该被驱逐（PickVictim 取最小分）
    score  =  heat                  // 越热分值越大 → 越该保留
            + 100  若页为脏          // 驱逐需先刷盘
            + 1000 若非叶且 heat >= nonleaf_evict_penalty
                                    // 非叶携带路由信息，整体晚于叶页被换出；
                                    // 但它冷到低于阈值时取消惩罚，允许换出
钉住页 == INT32_MAX（不可驱逐）
```

符号方向极易写反（第一版实现把 `-heat` 当成"越热越该走"，结果最热的页成了首选受害者）。
锁死方向的是两个测试：`Integration.EvictionPolicyPicksColdestLeafVictim` 与
`Integration.MigrationPolicyScoresNonLeafAsCostlierToEvict` 的单调性断言。

## 分层不变量

见 `docs/architecture.md` §1 与 `scripts/check_layering.py`（R1–R6）。
最核心的一条：**`src/index/**` 禁止文件 I/O** —— 一切落盘必须经 `BufferPool`。
