> 当前口径（2026-10-06）：本文为 C1 前置阶段的历史证据说明；文中「C1 未实现」、
> 「30 分钟未跑」等表述仅指当时。当前已完成页闩、读者 crabbing 和 crabbing 版
> 30 分钟压测，M6-2 仍缺完整验收。以 [status.md](../../docs/status.md) 为准，
> 后续顺序见 [strategy.md](../../docs/strategy.md)。
# M6-2 / C1 · 页闩、LockOrderChecker、30 分钟压测驱动（前置件）+ C0 边界的实测证据

**状态：C1 本体仍未完成；但它的头号拦路石——缺陷 #46（帧表边界）——已修复。**
本目录既记录 C1 的三件**前置件**，也记录 #46 的两条红→绿实验与两次单变量证伪。
索引层的 latch coupling 一行都还没动。

Build: `build-ninja`, RelWithDebInfo, MSVC 14.51.36231 + Ninja, `/W4 /WX`。

| file | what it is |
| --- | --- |
| `green_page_latch.txt` | `test_page_latch` 全绿：8 passed, 0 failed |
| `red_no_locking.txt` | 证伪实验 A：把真实加锁关掉 → **3 passed, 5 failed** |
| `red_no_order_check.txt` | 证伪实验 B：把顺序检查器关掉 → **5 passed, 3 failed** |
| `c0_boundary_probe.txt` | C0 缺页路径不安全性的实测（**修复前**）：对照组绿，实验组 **5/5 红** |
| `soak_red_evicting.txt` | 压测驱动在 evicting regime 下（**修复前**）：**红** |
| `soak_green_resident.txt` | 压测驱动在 **resident** regime 下绿（并打印"没测到帧表路径"的 CAVEAT） |
| `soak_control_single_thread.txt` | **单变量对照**：同一小池子、**115 万次淘汰**、单线程 → 绿 |
| `ctest_after_page_latch.txt` | 全量 ctest（15/15，前置件（二）之后） |
| **`fix46_summary.txt`** | **缺陷 #46 修复的完整摘要（修了什么 / 判据 / 证伪 / 回归）** |
| **`probe_after_fix.txt`** | `c0_boundary_probe 8 16 20000` **修复后**：5 次跑 5 次绿 |
| **`soak_after_fix.txt`** | `soak_mixed 20 4 2 20000 64 6 5` **修复后**：5 次跑 5 次 `clean` |
| **`red_falsification_no_exclusive_lock.txt`** | 证伪 1：未命中路径改共享锁 → 立即变红（重现 #46 原始症状） |
| **`red_falsification_search_view.txt`** | 证伪 2：Search 不拷载荷 → 5/5 红（`wrong_value=1..2`，`not_found=0`） |
| **`ctest_after_fix46.txt`** | 全量 ctest（15/15，缺陷 #46 修复之后） |

> **先读 `fix46_summary.txt`**：它把"修了什么、怎么验的、判据怎么变红"讲全了。
> 下面第一到六节保留为前置件与修复前基线的原始记录。

---

## 一、为什么这两件事是 C1 的前置件

C1 落地之前，`LatchManager::Acquire` 返回的是一个**不持锁**的守卫：
`latch_manager.cc` 里写着 `// M6：这里做真实加锁；骨架返回一个不持锁的守卫`，
`Release()` 是空函数，`CheckOrder()` 是空函数。也就是说"页闩"这个名字在代码里
成立了很久，**一把锁都没加过**，顺序检查器一次都没跑过。

这正是本项目最怕的形状（缺陷 #37 的教训）：调用方按有锁去写，测试按有锁去断言，
而底下什么都没发生。所以这两件必须先落地并且**各自可证伪**，C1 才有地基。

另有一个现实理由：C0 的测试是靠断言"测量期零缺页"绕开缺页路径的
（`test_c0_latch.cc` 断言 `misses` 增量 == 0）。缺页路径不安全这件事被写进了
`docs/status.md`，但**从来没有被实测过**。第三节把它测了。

---

## 二、落地了什么

### 2.1 真的页闩（`src/concurrency/latch_manager.{h,cc}`）

* 每页一把 `std::shared_mutex`，按页号分 **64 路元锁**保护表本身。
  分的是元锁不是页锁：按页号去分页锁只会制造假冲突（两个无关页落进同一片，
  一个要写就把另一个的读挡住）。元锁只在查找/建项时拿，拿到页锁地址立刻放掉，
  **绝不持着元锁去阻塞页锁**——那会把元锁变成全局串行点，等于把 C1 退回 C0。
* 表的值是 `unique_ptr<shared_mutex>`：`unordered_map` 的节点在 rehash 时不搬，
  但那是实现细节，而"某线程正阻塞在这把锁上、锁的地址却被搬走"是 UB。
  用指针把地址钉死。
* `PageLatch` 是 move-only 的 RAII 守卫，析构即解锁。复用（本线程已持有时）
  返回 `owns() == false`，语义与 `C0LatchGuard` 的 `Counted` 分叉同一处理。
* 争用计数是真的：先 `try_lock`，失败才计数再阻塞。第五节用它自检。

### 2.2 LockOrderChecker

三条判据，全部在 **acquire 侧**（死锁只由获取顺序决定）：

1. **level 单调不增** —— 持锁栈自根向下。拿着 level=L 的页再去拿 level>L 的页
   就是"向上加锁"。
2. **同层 pid 不降** —— 兄弟交互的"恒先左后右"。
3. **不重复获取 / 不升级** —— 同一页不得二次加锁；已持共享再要独占是升级。

检出即 `++stats.order_violations` + 打印完整持锁链；DEBUG 下另外 `abort()`
（`abort_on_violation`，Release 下只计数，不把线上进程变成雷）。

---

## 三、实现中发现的两处**方案原文有问题**（不是顺手改的）

### 3.1 判据用 level，不用 pid 正序

方案 §6 写的是"发现 **pid 逆序**即 abort"。**pid 是分配序，不是树序**：
一次分裂里新分配的右兄弟会从空闲链上拿到更小的 pid，于是"正确的先左后右"
在 pid 上是递减的。拿 pid 当序，会把正确实现判成违规，而真正的逆序
（拿着叶再去拿根）反而可能看起来递增。

所以权威判据是 level；pid 序只保留为**同层兄弟**的辅助见证。
**已知边界**：它只在两个同层页被同时持有时判定，且不验证是否同父。
两个不同父的同层页被同时持有不是任何合法树操作，所以在本项目的形态下够用——
但这不是"任意逆序都能抓"，写在这里，不假装完备。

### 3.2 「释放顺序必须与获取顺序相反」是错的

方案 §6 的同一句还写着"下钻时先拿子页再放父页；**释放顺序必须与获取顺序相反**"。
这两句不能同时成立，而第二句恰好禁止了 latch crabbing 本身：

```
拿父 P(level 3) → 拿子 C(level 0) → 放父 P → 继续用 C
```

放 P 的时候 C 在栈顶，**P 不是栈顶**。若按"必须反序"卡死，这个动作直接违规 ——
而它正是 C1 要靠的模式。反过来，最朴素的"锁-读-放-再锁下一层"（一次只持一个）
也是合法的，它的释放顺序是**递增**的。两种都对，所以释放侧不存在一条统一的
顺序约束。

最初按原文实现了"释放必须卡栈顶"，是**测试日志抓出来的**：合法的 crabbing
序列被判成违规。现在释放侧只保留"确实持有"这一条 —— 它能抓住重复释放、
释放没持有的页、守卫被拿到别的线程去解这三种真 bug，又不误伤合法模式。

### 3.3 附带发现：thread_local 单槽会把别的管理器的持锁栈抹掉

第一版把本线程的持锁栈写成"一个槽，换 LatchManager 就重置"。同样是被测试日志
抓出来的：同一线程上第二个管理器一出现，第一个管理器的栈被整段抹掉，它已加的
闩再也解不开（日志里是两条 `released on a thread that never acquired it`）。
现在按管理器指针分栈。

**同一个隐患在 C0 的 `g_c0` 里仍然存在**（换池就重置计数）。本次**没有**改它：
C1 落地后 C0 会被撤掉，且当前代码结构下一次索引操作只碰一个池，触发不了。
记在这里，不假装它不存在。

---

## 四、证伪实验（判据必须能变红）

### 实验 A —— 关掉真实加锁

单变量：`LatchManager::Acquire` 直接返回一个 `owns() == false` 的守卫
（即 C1 之前骨架的确切行为），其余一行不动。

`red_no_locking.txt`，4 个用例里 **5 个红**：

```
[FAIL] PageLatch.ExclusiveReallyExcludesShared
       EXPECT_EQ failed: violations.load() == 0 (left=550, right=0)
       ... 120 writer rounds, 4 reader threads x 150; violations=550
[FAIL] PageLatch.CorrectDescentIsSilentIncludingCrabbing
       EXPECT_EQ failed: mgr.HeldStack().size() == static_cast<size_t>(0) (left=3, right=0)
[FAIL] PageLatch.ReusedAcquisitionDoesNotRelatch        (6 failed assertions)
[FAIL] PageLatch.CrossThreadReleaseIsCaught
       EXPECT_TRUE failed: holder->value().owns()
[FAIL] PageLatch.ContentionCounterIsReal
       EXPECT_TRUE failed: shared_page > 0
========== 3 passed, 5 failed ==========
```

**550 次读写交叠**就是那个历史缺陷的量化形态。互斥、争用计数、RAII 语义三条
判据同时失守，说明它们都是承重的，不是装饰。

### 实验 B —— 关掉顺序检查器

单变量：`LatchManager::CheckOrder` 开头直接 `return`（即 C1 之前空函数的确切行为）。

`red_no_order_check.txt`，**3 个红，且恰好是需要"检出违规"的那 3 个**：

```
[FAIL] PageLatch.LockOrderCheckerDetectsInversion
       EXPECT_EQ failed: Violations(mgr) == 1u (left=0, right=1)
[FAIL] PageLatch.LockOrderCheckerDetectsRightToLeftSiblings
       EXPECT_EQ failed: Violations(mgr) == 1u (left=0, right=1)
[FAIL] PageLatch.ReusedAcquisitionDoesNotRelatch
       EXPECT_EQ failed: Violations(mgr) == 1u (left=0, right=1)
========== 5 passed, 3 failed ==========
```

三个**负向对照**（`CorrectDescentIsSilentIncludingCrabbing`、
`ExclusiveReallyExcludesShared`、`ContentionCounterIsReal`）保持绿 ——
证明实验只动了检查器这条轴，没有顺手弄坏别的东西。

两次实验都按字节还原：`src/concurrency/latch_manager.cc` sha256
`46f5159dcf0843bf6a9030eebfccde0d208c5f64c56a5cb84ba78c661cc56739`，
树里 `FALSIFICATION EXPERIMENT` 标记 0 个，还原后 `test_page_latch` 8 passed / 0 failed。

---

## 五、C0 缺页路径不安全 —— 从断言变成实测

### 5.1 为什么必须测

`docs/status.md` 的 M6-1 一节写着：C0 的读并发**只在"只读 + 测量期零缺页"下成立**，
缺页/淘汰路径会改 `page_to_frame` / `free_frames` / `clock_hand`，那条路在共享闩下
不安全。`test_c0_latch.cc` 甚至把它写成断言（`misses` 增量必须为 0）。

但这条**从来没有被实测过**。按本项目的规矩，一个"能出事的路径"如果没演示过它真出事，
就只是描述，不是证据。

`tools/c0_boundary_probe.cc` 补的就是这一步：把池子调小到装不下工作集，
让 8 个读线程在测量期**必然缺页**，看会发生什么。

**它不是 ctest 用例**，故意不注册进 `LTM_TEST_SOURCES`：C0 的边界是已知的、
待 C1 修的问题，让它常驻红灯会把测试套件变成一个"习惯了失败"的东西。
红是它的预期输出。

代码里对得上：`buffer_pool.cc` 的 `page_to_frame` 是裸 `std::unordered_map`
（第 61 行），`FetchChecked` 在共享闩下做 `find` → `++fr.pin_count`（734-736 行），
未命中时做 `im.page_to_frame[pid] = frame_idx`（763 行）。多线程无锁写同一个
`unordered_map`。

### 5.2 结果

**对照组**（4096 帧 ≥ 工作集 647 页，测量期零缺页）：**绿**。

```
pool: frames=4096 resident=647
ran 10.98s, misses during measurement=0
wrong_value=0 not_found=0 bad_status=0 exceptions=0
tree.Validate()=ok
VERDICT: no corruption observed (boundary not demonstrated)
```

**实验组**（16 帧 << 647 页）：**5 次跑 5 次红**，两种失败形态：

| run | exit | 结果 |
| --- | --- | --- |
| 1 | 127 | 进程无 verdict 直接死；Unpin 报错 122 |
| 2 | 127 | 同上；Unpin 报错 220 |
| 3 | **139** | **Segmentation fault**；Unpin 报错 144 |
| 4 | 127 | 进程无 verdict 直接死；Unpin 报错 132 |
| 5 | 1 | `wrong_value=26 not_found=219256`；`Validate: the fetched page claims page_id=596, but the requested page is 3`；Unpin 报错 208 |

每次跑到出错的墙钟时间约 **0.11 秒**。

三种失败里的关键数字：

* `not_found=219256` —— 绝大多数点查返回"没找到"。
* `wrong_value=26` —— **返回了别的记录的载荷**。不是崩溃，是静默的错数据。
* `tree.Validate()=kCorruption: the fetched page claims page_id=596, but the
  requested page is 3` —— 帧表把属于另一个页的帧交了出去。这就是机制本身。
* 每次 120–220 条 `Unpin failed on release`，两类：
  `Unpin count exceeds the pin count`（pin 计数被并发改坏）、
  `Unpin on a page that is not in the pool`（帧已经被并发淘汰掉了）。

### 5.3 这条证据的用途

原先交给 C1 的是一句话："缺页路径在共享闩下不安全"。现在交给它的是一条命令
和它的 5 种失败输出。**帧表改造完成与否的判据，就是这条命令从红变绿** ——
不需要再商量"改到什么程度算好"。

---

## 六、30 分钟压测的驱动（验收三件里的第三件）

C1 的验收有三件：`LockOrderChecker` 零逆序、ASAN 干净、**混合读写 30 分钟无死锁无挂起**。
第三件此前连工具都没有。这一节补上 `tools/soak_mixed.cc` —— 它同时是
**缺陷 #46 的第二条判据**（第一节那条 `c0_boundary_probe` 只做点查，这条覆盖
区间扫 + 写路径 + 淘汰）。

### 6.1 为什么结果取决于池子，而驱动会自己说出来

| regime | 条件 | 今天 | 含义 |
| --- | --- | --- | --- |
| `resident` | `frames >= 树的页数` | 绿 | 看不到帧表路径。**绿不是 C1 的成绩**，驱动会打印 `CAVEAT` |
| `evicting` | `frames < 树的页数` | **红** | 踩到缺陷 #46；C1 之后变绿才是成绩 |

**这是本节最重要的一行。** 一个只在 resident regime 下跑绿、却被人当成
"压测通过"的工具，就是缺陷 #37 的翻版。所以命令里**必须带池子参数**：
`soak_mixed.exe 20 4 2 20000 64 6 5` 里的 `64` 不是可选项。

### 6.2 单变量对照：红来自并发，不是"池子太小"

| 运行 | 池子 | 并发 | 淘汰次数 | 结果 |
| --- | --- | --- | --- | --- |
| `soak_control_single_thread.txt` | 64 帧 | **1 线程** | **1153388** | **绿**（`validate=ok`） |
| `soak_red_evicting.txt` | 64 帧 | 4 读 2 写 | 3368 | **红**（`validate=kCorruption`） |

同一个池子、同一个 regime、单线程跑出 **115 万次淘汰**且全绿；只把并发加回来就红。
这一对照把归因钉死了 —— 否则读者完全有理由怀疑"是不是池子小于工作集本身就不支持"。

### 6.3 红的样子（重复 5 次全红）

```
PageGuard: Unpin failed on release: kCorruption: Unpin count exceeds the pin count: 379
call_failures=2040562
final: live_entries=0 validate=kCorruption: the fetched page claims page_id=68, but the requested page is 67
SOAK_VERDICT: FAILED
```

5 次里 **3 次进程直接死在半途**（`exit=127`，输出停在 unpin 那行），
**2 次活到收尾**并报 `validate=kCorruption`。**不变的症状是
`Unpin count exceeds the pin count`** —— 两个帧的 pin 计数串了，
说明 `page_to_frame` 在并发淘汰下被写坏。这正是缺陷 #46。

> **一条未复现的观察，不作判据**：另有一次 512 帧的混合跑给出
> `read_wrong_payload=2` —— 读线程**拿到了别的键的载荷**，是数据级撕裂，
> 比结构报错更严重。随后 6 次重跑（含 512 帧）未再出现，**没有留证**。
> 记在这里只为一件事：这条路的失败**不只是**内部报错，也会表现为
> 调用方读到错数据，所以别只靠结构自检去判。

### 6.4 为了真能跑 30 分钟而必须解决的两件事

1. **日志会写爆盘。** 实测稳态约 2 MB/s，载入 17500 条一次就攒 **303 MB**；
   不截断的话 1800 秒会写出几十 GB。驱动周期调
   `CheckpointManager::RunCheckpoint()` 截断，并守两条既定前提：
   **不在写事务在途时做**（排空屏障：置 `pause` → 等 `active==0` → 取 C0 独占闩
   → 做检查点 → 放 `pause`）、**与写操作同取 C0 独占**。
   实测每个检查点周期占盘约 **10.6 MB**（5 s 间隔、带写线程）。
   排空等待顺带成了**第二个挂起判据**：进度心跳抓不到"某线程卡在调用里再也
   回不来"，排空等不到 30 s 就 abort。
2. **`stats().num_pages` 在 `Validate()` 之前是 0**（只在自检遍历里统计，
   `tree_ops.cc:1442`）。不先 Validate 一次，`REGIME` 会**永远判成 resident** ——
   一个永远说"没测到"的判据等于没有判据。**这个 bug 我自己写出来了一次**，
   是靠"故意用大池子和小池子各跑一遍看它会不会翻转"发现的。

---

## 七、仍未证明 / 仍缺什么

* **C1 没实现。** 索引层 11 个入口（`bplus_tree.cc:913,924,941,965,1029`、
  `rtree.cc:1486,1655,1672,1739,1815,2048`）**仍全部走 C0**。本目录的新机器
  还没被索引层用上。
* **帧表边界未解决。** 见第五、六节。**不解决它就不能撤 C0。**
* **索引层耦合未做**：下钻时先拿子页再放父页、分裂/合并的 latch crabbing。
* **30 分钟本身没跑过，ASAN 也没跑过。** 第六节的驱动最长只跑了 20 秒。
  方案 §6 的 C1 验收是"混合读写跑 30 分钟无死锁无挂起；ASAN 干净 +
  LockOrderChecker 零逆序"。
* **驱动只在 C0 下跑过。** 它作为"C1 判据"的身份要等 C1 落地后重新验一遍 ——
  今天的红是它在 C0 下的基线，不是它对 C1 的判决。
* **排空屏障只在"每个写操作各成一个事务"的负载下验过。** 将来若有跨多步的长事务，
  `active == 0` 这个判据要重新审。
* **跨线程的锁序环抓不到。** 检查器是**每线程**的持锁栈，两个线程各自合法、
  合起来成环（A 持页1 等页2，B 持页2 等页1）它看不见。那正是 30 分钟压测
  要覆盖的东西；MSVC 无 TSAN，这是本项目在竞争检测上的已知缺口
  —— 结题报告要写进局限，**不许记成"TSAN 干净"**。
* **帧表改造的设计里有一个已识别的坑，动手前必须先定序**：如果缓冲池加载页时
  去拿"页闩"，就会得到 `frame_table → page(子)` 的顺序；而索引层 crabbing 是
  `page(父) → frame_table → page(子)`。两者**成环**。绕开的方式是给加载步骤
  单独一把 load-lock（或"加载占位 + 条件变量"），让全局顺序保持
  `页闩(自根向下) → frame_table → 该页的 load-lock`。
  这条是今晚设计到一半识别出来的，没有动手实现。

---

## 八、一个环境陷阱（害我吃了一次假红）

`scripts/build_local.py test` **不会先构建**。证伪实验 B 之后我只做了 sha256 校验、
没有重新编译，于是 ctest 跑的是"检查器已关掉"的旧二进制，`test_page_latch`
在 ctest 里显示 5 passed / 3 failed —— 看着像回归，实际是陈旧产物。

**还原实验之后必须 `build` 再 `test`。**

---

## 九、缺陷 #46 已修复（帧表边界）

完整摘要见 `fix46_summary.txt`。要点：

### 9.1 两处修复

1. **帧表临界区**（`src/storage/buffer_pool.cc`）：新增 `Impl::table_mutex`
   （`shared_mutex`），保护 `page_to_frame` / `free_frames` / `clock_hand` 与
   `Frame` 的非原子字段。命中路径取共享（查表 + 原子加 pin），未命中/淘汰/安装取
   独占并二次查找 —— 于是"查到即加 pin"与"淘汰判定"在同一个临界区里完成。
   `StampPageLsn` 改为按帧号取帧（否则 `Unpin` 持锁时 `CollectDelta` 会递归获取
   同一把锁）。锁序：`页闩(自根向下) → frame_table → load-lock`。

2. **`RecordRef` 自持载荷**（`index_entry.{h,cc}` + 三棵树的 `Search`）：`Search`
   在守卫析构前把载荷拷进 `owned_payload`，返回值可安全带出调用。**这是第二条、
   独立的 bug**：原契约要求"调用方自行拷贝"，可调用方拿到指针时守卫已经放掉了，
   根本没有安全的拷贝窗口。

### 9.2 红 → 绿

| 判据 | 修复前 | 修复后 | 证据 |
| --- | --- | --- | --- |
| `c0_boundary_probe 8 16 20000` | 5/5 红（Segfault / `not_found=219256`） | **5/5 绿**（`wrong_value=0 not_found=0`） | `probe_after_fix.txt` |
| `soak_mixed 20 4 2 20000 64 6 5` | 5/5 红（`Unpin count exceeds…`） | **5/5 `clean`** | `soak_after_fix.txt` |

### 9.3 两次单变量证伪（判据必须能变红）

* **证伪 1**：把未命中路径的独占锁改成共享锁 → 立刻重现 #46 原始症状
  （`not_found=102811`、`claims page_id=617 but requested 3`、两次直接死掉）。
  `red_falsification_no_exclusive_lock.txt`
* **证伪 2**：让 `BPlusTree::Search` 不拷载荷 → 5/5 红（`wrong_value=1..2`，而
  `not_found=0`、`Validate()=ok`）—— 精确定位到第二条 bug。
  `red_falsification_search_view.txt`

两次还原均 sha256 逐字节一致（`buffer_pool.cc 9bf0328a…`、`bplus_tree.cc 95cb7d8e…`），
源码里 `FALSIFICATION EXPERIMENT` 标记 0 个。全量 ctest **15/15**（`ctest_after_fix46.txt`）。

### 9.4 仍未做

* **C1 本体**：索引层 11 个入口仍走 C0，页闩未接线。
* 30 分钟压测（本次最长 20 秒）与 ASAN 未跑。
* `HeatSource` 不在帧表锁范围内（`HotColdTracker` 自身还不是线程安全的）；当前两条
  判据都用 `heat=nullptr`，C1 放开读并行后要单独处理。
* 写路径里直接访问帧表的少数几处（`NewPage` / `FreePage` / `RollbackWriteOp` /
  `LoadMeta`）仍在临界区外 —— C0 下写被独占闩串行化，故安全；C1 若放开写并行需一并收进锁。

