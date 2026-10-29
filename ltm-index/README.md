# ltm-index

通用 **larger-than-memory** 树形索引结构 —— 哈尔滨工业大学（威海）大一年度项目。
项目负责人：褚瑞璇（2025211227）　指导教师：王金宝 副教授。

**KNN 实测更新（2026-10-16）**：新增固定总工作量的 1/2/4 读者、k=1/16/64、驻留/淘汰各三次矩阵；普通 54 次，连同 ASAN/TSAN 小矩阵共 **78 次测量与 78 次新进程重开**通过，结果逐项比较键、MBR 和载荷。驻留时本轮吞吐提高，频繁淘汰时多读者未获得扩展收益；下一步定位淘汰路径的实际成本。核心未修改，不能据此计算相对旧版的提速比例，见 [结果与边界](report/optimization/knn_matrix_20261016/README.md)。

**工程验收更新（2026-10-16）**：新增无额外写后探测的 mixed 轨迹和统一三配置验收入口，修复 Linux 主链路脚本；最终普通/ASAN 各 20/20、TSAN 相关专项 3/3、各双树结构守门及 18 项协议检查通过，两类轨迹共 36 次运行与 36 次新进程重开一致。清理可生成文件约 2.54 GiB，保留历史程序和证据。入口见 [工程验收说明](docs/acceptance.md)，完整范围与原始数据见 [本轮验收](report/acceptance/mixed_measure_20261016/README.md)。

**最新工程优化（2026-10-16）**：修复 KNN 同距边界选错记录的问题，候选集改用按 `(距离, 键)` 排名的大顶堆，单次接纳的维护开销从 O(k) 降为 O(log k)。三个新回归均先在旧实现复现失败；最终普通、ASAN 各 20/20，Debug、TSAN 专项各 4/4，四配置双树结构检查及新进程重开链路通过。尚未测量实际提速比例，见 [top-k 优化与后续工作](report/optimization/knn_topk_20261016/README.md)。

**最新工程优化（2026-10-08）**：KNN 查询已取消整段串行锁，多个读者可重叠执行；查询计数在结束时发布为安全值快照。新增三个并发回归，普通和 ASAN 各 20/20、Debug 和 TSAN 专项各 4/4，双树结构检查及新进程重开主链路通过。尚未量化吞吐量增幅，见 [KNN 优化与验证](report/optimization/knn_parallel_20261008/README.md)。历史实验仍按原版本引用。

**当前状态（2026-10-07）：M0–M5 已完成；M6-2 的约定 C1 验收范围已补齐；B+/Hybrid 查询、固定混合读写与限定恢复曲线已验证；M7 已进入结题准备，Word 正文初稿和配套实验材料已整理。扩展实验与 M7 正式提交尚未完成，可阶段性交付核心工程。** 当前支持 point/range/spatial/contain/knn 与 mixed 基准及严格的驻留/淘汰对照，见[基准说明](report/bench/README.md)、[Hybrid 实测](report/bench/hybrid_README.md)、[混合读写实测](report/bench/mixed/README.md)、[恢复性能](report/recovery/README.md)、[并发验收](report/c1/tsan/README.md)、[实施策略](docs/strategy.md)。独立只读实验 30/30 通过，恢复实测普通 15 场景与 ASAN 两场景通过；旧纯内存对照已取消。无探测混合测量与固定 KNN 多读者矩阵已补，下一步定位淘汰并发成本、扩大工作集并完成正式提交要求。混合索引的 M5 成果如下：`HybridIndex` 用**一个写事务**同时写
B+ 树与 R 树（要么全生效要么全回滚），记录（键 + MBR + 载荷）存在 B+ 侧，
**Get 按键定位不访问 R 树；Erase 从 B+ 读取记录后仍须删除 R 树条目**，I8 双树一致性有**双向且可证伪**的交叉校验。
崩溃矩阵扩到 **78 例全过**（72 例真的把进程强杀了），其中 36 例跑混合索引 ——
同一条判据（"恢复后全量遍历 == 旁路日志里的已提交集合"）顺带覆盖了双树原子性。
崩溃恢复（M4-3）此前已完成：WAL 有 512 字节日志头、Analysis/Redo/Undo + CLR、检查点截断。
实现进度唯一权威表见 [`docs/status.md`](docs/status.md)。

| 里程碑 | 内容 | 状态 |
| --- | --- | --- |
| M0 | 工程骨架、构建、测试框架 | ✅ |
| M1 | 存储内核（页 / 缓冲池 / WAL / 驱逐） | ✅ |
| **M2** | **B+ 树页式实现** | ✅ 门槛通过 |
| **M3** | **R 树页式实现（+ 跨页算法通用化落地）** | ✅ 门槛通过 |
| **M4-1** | WAL 记录编码 / 顺序追加 / fsync 屏障 | ✅ |
| **M4-2** | **写屏障接入（把 WAL 接到真实写路径上，并证明非空转）** | ✅ 门槛通过 |
| **M4-3** | **崩溃恢复（日志头 / 检查点 / Analysis-Redo-Undo + 崩溃矩阵）** | ✅ 门槛通过 |
| **M5** | **混合索引（单事务双树 + I8 双向可证伪交叉校验）** | ✅ 门槛通过 |
| M6 | 并发与基准 | M6-2 约定 C1 验收完成；查询、固定 mixed 及限定恢复曲线已验证；扩展实验未完成 |
| M7 | 结题收尾 | 准备中：Word 正文初稿与实验材料已整理；正式提交待完成 |

2026-10-07 范围更新：用户取消旧纯内存索引对照，后续集中磁盘页式版本。新增 [恢复性能实验](report/recovery/README.md) 普通 15 场景与 ASAN 两场景通过，测量五种检查点后日志量，原始数据与曲线已加入结题材料；下一项补无探测混合读写测量。

---

## 这是什么

一个把「内存层 B+ 树 / R 树」重构为「**页式 LTM 存储内核 + 可插拔树算子**」的 C++17 工程：

- **页式存储内核**：定长页（slotted page）+ 页号引用 + 缓冲池，节点可持久化，不再依赖指针
- **通用树算子契约**：`TreePageOps`（页内差异）+ `TreeOps`（跨页算法只写一遍）
  —— 新增树变体**不需要改动 `src/storage/**`**（判据是变体本身是否触及存储层，
     不是存储层的行数恒定；口径见 `docs/architecture.md` §3）
- **持久化**：单写者页级 WAL（REDO）+ 操作级 UNDO + 检查点 + 撕裂页检测
- **冷热迁移**：页级热度 + CLOCK 驱逐 + 语义化打分（见 `docs/invariants.md`）
- **混合索引**：一维与多维统一寻址，一次事务跨双树原子提交

> 「跨页算法只写一遍」这条主张在 M3 **已经兑现，但只兑现一半**，`docs/architecture.md` §3
> 记录了实测 diff 形状与不可共享的那一半（查找路径）及其原因 —— 那是一条被对抗性评审
> 逼出来的边界，不是为了好看而含糊过去的话。

设计背景、重构理由、评审要点与实验设计见上层目录的 [`../终版方案.md`](../终版方案.md)。

## 一键跑通主链路

```powershell
python -B scripts/run_pipeline.py --build-dir build-verify-current
```

自动构建、写入 512 条二维记录、保存检查点、核对查询、关闭，再用新进程重开
同一数据库并核对全部记录。成功显示 `PIPELINE_VERDICT: passed`；数据库和日志
保留在新建的 `report/pipeline/` 子目录。普通/ASAN 实测和已发现的元数据校验边界
见[完整链路说明](report/pipeline/README.md)。

---

## 快速开始

```powershell
# 一键：配置 + 编译 + 测试
python scripts\build_local.py all build-verify-current  # 独立目录：配置 + 编译 + 普通测试（20 项）
python scripts\build_local.py test build-verify-current # 只跑测试；源码变更后必须先构建
scripts\build.bat test                                # Visual Studio 生成器入口

# 单独跑
build-ninja\ltm_smoke.exe                             # 冒烟：util / page / 序列化 / MBR / 注册表
python scripts\validate_tree.py --list                # 不变量自检会检查什么
python scripts\validate_tree.py                       # 跑不变量自检（结果写 report/validate_*.txt）
python scripts\check_layering.py                      # 分层依赖检查（当前 0 违规）
build-ninja\ltm_demo.exe                              # 演示：B+ / R / 混合索引三棵结构实跑 + WAL 证据
build-ninja\test_hybrid_diff.exe                      # 混合索引差分 + I8 双向交叉校验
python scripts\bench.py --records 2048 --ops 2000 --frames 12 512 --repeats 3
python scripts\bench.py --workloads spatial contain knn --records 1024 --ops 500 --frames 12 256 --repeats 3
python scripts\mixed_bench.py                          # 固定混合读写 + 每轮新进程重开核对
```

**崩溃矩阵**（M4-3 起，必须用故障注入构建）：

```bash
python scripts/build_local.py fi        # 配置并编译 build-fi/（-DLTM_ENABLE_FAULT_INJECT=ON）
python scripts/crash_matrix.py          # 78 例，产出 report/recovery_matrix.md
```

> **本机构建请注意**：本机路径纯 ASCII，VS 装在 `D:\visual studio\IDE`，`reg.exe` 可用，
> 所以 `scripts\build.bat`（VS 生成器）与 `scripts/build_local.py`（Ninja）**两条路都可用**。
> `build_local.py` 是推荐入口——它不依赖注册表，且一条命令能切 ASAN / 故障注入 / Debug 三种配置。
> 细节见 `docs/architecture.md` §8.1。

**长时压测**（M4-2 起，需要在 Debug 构建下跑）：

```bash
python scripts/build_local.py debug          # 配置并编译 build-dbg/（无 /DNDEBUG）
bash scripts/soak_m4_2.sh 3600 build-dbg     # 1 小时，断言在场，逐轮留判据到 report/m4_2/
```

它要求 Debug 构建 —— 因为 RelWithDebInfo 带 `/DNDEBUG`，`assert()` 在那里是**被编译掉的**，
拿它证明"压测没触发断言"等于什么都没证明。

压测把每一轮的二进制都跑在私有目录 `build-dbg-soak/` 下，所以它和守门脚本**不再抢同一批
`log/_validate_*` 文件**（早期版本会抢，并因此制造过一次**看起来完全真实的 I6 违例**，
见 `docs/architecture.md` §7）。今天两者并发是文件安全的；不建议并发只是为了别让两边的
耗时数字互相污染 —— 压测的 `wal=` / 秒数列是它自己的证据之一。

**可执行文件在构建目录根**（`build-vs\ltm_smoke.exe`），不是 `build-vs\RelWithDebInfo\` 下。

工具链要求：C++17，MSVC 2019+（VS 2022/2026 均可）或 GCC 9+；CMake ≥ 3.20。
本机实测：Visual Studio Community 2026（MSVC 14.51）+ CMake 4.3.1。

### 当前验证结果（M5 门槛，均为实测）

```
python scripts\build_local.py test            -> 12/12 ctest passed (zero warnings, /W4 /utf-8 /WX)
python scripts\build_local.py test build-fi   -> 12/12 passed (fault-injection build)
python scripts\build_local.py test build-asan -> 12/12 passed (ASAN verified active, report\m4_3\asan_proof.txt)
build-ninja\ltm_validate_bplus.exe --quiet    -> exit 0; WAL delta 43024, LSN stamps 43024 (missed 0), I6 violations 0
build-ninja\ltm_validate_rtree.exe --quiet    -> exit 0; WAL delta 25551, LSN stamps 25551 (missed 0), I6 violations 0
build-ninja\test_recovery.exe                 -> 7 passed, 0 failed (including 152 WAL prefixes)
build-ninja\test_hybrid_diff.exe              -> 9 passed, 0 failed (differential + I8 + dual-tree atomicity)
python scripts\crash_matrix.py                -> 78/78 passed (72 real crashes, 36 of them hybrid), report\recovery_matrix.md
python scripts\check_layering.py              -> 46 source files scanned, 0 violations
python scripts\check_ascii_literals.py        -> 71 files scanned, 0 non-ASCII literals
build-ninja\ltm_demo.exe                      -> exit 0; B+ tree, R tree and the hybrid index all run
bash scripts/soak_m4_2.sh 3600 build-dbg      -> 1 hour Debug soak: 222 rounds / 0 failures / exit 0 (measured in M4-2)
```

「崩溃恢复真的能用」不看日志说了什么，看三条**互不替代**的证据（全部在 `report/m4_3/`）：
① 旁路日志（`journal.txt`）记录的已提交集合与恢复后的全量遍历**逐条相等**；
② 未提交事务被真的回滚（`clrs_written == undo_records_applied > 0`）；
③ 撕裂页与坏校验和被真的修回（`torn_pages_repaired + checksum_repairs > 0` —— 这条在用例里是**硬断言**，不成立就算那条用例空跑）。
第三条是硬要求 —— 本项目刚因为「ASAN 开关在 MSVC 下静默失效」把一条**从未真正执行**的
门槛记成"通过"（缺陷 #37，假绿），所以"跑完了没报警"本身不算证据。

「混合索引真的成一件事」同样看三条（全部在 `report/m5/`）：
① **按键定位不访问 R 树**：连做 100 次 `Get`，R 树的定位计数纹丝不动（用例 E）；
② **双树原子性**：未提交的 `Put` 恢复后两棵树一起没有（用例 H），崩溃矩阵 36 例混合索引全过；
③ **I8 交叉校验可证伪**：故意让两棵树对同一条记录说不同的话，检查必须报出具体键号（用例 I）。
第三条同样是硬要求：一个从不报警的交叉校验与没有交叉校验完全一样。

另一个配置 —— **ASAN**（`build-asan\`，`cmake -DLTM_ENABLE_ASAN=ON`）与
**Debug**（`build-dbg\`，无 `/DNDEBUG`，`assert()` 在场）：
两者都不与 `build-vs\` 共用输出目录，因为它们都被强制输出到各自的构建目录根。
ASAN 是 M3 门槛「ASAN 全绿」的实际证据；该项一度是**假绿**（开关在 MSVC 下静默失效，
见 `docs/status.md` 缺陷 #37）。口径提醒：Windows 上 MSVC ASAN **没有** LeakSanitizer，
它查的是越界 / UAF / 野指针，**不查内存泄漏**。

> 上述命令的**程序输出一律为 ASCII**（含 `report/` 下自动生成的证据文件）。中文只出现在
> **注释与 `docs/`** 里 —— 本机控制台代码页为 GBK(936)，中文字面量在 C++ 与 Python 两端
> 的渲染结果不一致，遇到 GBK 无法表示的字符时 Python 会直接 `UnicodeEncodeError` 崩溃。
> 判据与完整理由见 `docs/status.md`「已知开发陷阱」末两条。

实测规模（`ltm_demo.exe`，capacity=32，缓冲池 64 帧）：

```
B+ 树：插入 4000 条 -> height=2 pages=180 entries=4000 填充率 69.4%
       点查命中，范围扫描命中 4000 条，不变量自检 全绿
R 树 ：插入 4000 条 -> height=2 pages=196 entries=4000 填充率 63.8%
       空间相交[400,600]^2 命中 169 条，KNN(k=10) 与暴力对拍一致，
       KNN 访问 6 页 / 暴力 187 页（剪枝率 96.8%），不变量自检 全绿
```

> `ctest` 共 10 项，其中 `validate_bplus` / `validate_rtree` 是**不变量自检**，带 900s 超时、
> 属「守门」性质，`build.bat test` 用 `-E validate` 排除它们，改由 `validate_tree.py` 单独跑
> （它还会顺带产出三策略对照 CSV）。

---

## 目录结构

```
ltm-index/
├── CMakeLists.txt              构建定义（编译选项段必须位于第一个目标之前，见 docs/architecture.md §7）
├── src/
│   ├── util/                   status / log / crc32c / types / config        ← 已完整实现
│   ├── storage/                page(已实现) / disk_manager / buffer_pool / wal / recovery / fault_inject
│   ├── index/                  index_entry / tree_ops（跨页通用引擎 IndexTree）
│   │                           bplus_tree / rtree（薄门面 + 页算子） / mock_tree / hybrid_index / index_registry
│   ├── migration/              hot_cold_tracker / migration_policy            ← 已完整实现
│   ├── concurrency/            latch_manager（C0/C1/C2 统一入口）
│   └── bench/                  基准测试入口
├── tests/
│   ├── harness/                极简零依赖测试框架 + 冒烟程序
│   ├── util/ storage/ index/ integration/
│   └── index/test_{bplus,rtree}_validate.cc   里程碑专用验收入口（退出码 0/1/2）
├── scripts/                    build.bat / check_layering.py / check_ascii_literals.py
│                               validate_tree.py / bench.py / crash_matrix.py
│                               soak_m4_2.sh（M4-2 长时压测驱动）
├── docs/                       architecture.md / invariants.md / status.md
├── demo/                       结题演示脚本
├── report/                     实验产出（CSV / 图 / 校验报告）
└── data/ log/                  运行期数据与日志（不入库）
```

---

## 三条必须遵守的规则

1. **索引层禁止文件 I/O**。`src/index/**` 里出现 `fstream`/`fopen`/`pread`/`mmap` 一律拒绝，
   一切落盘必须经 `BufferPool`。由 `scripts/check_layering.py` 强制。
2. **禁止裸指针跨层**。跨函数传递页一律用 `PageId`；`Page*`/帧指针只在 `PageGuard` 生命周期内有效；
   索引层不得出现返回 `Page*` 的接口。
3. **页级算子不自动重算校验和**。由 `BufferPool` 在脏页 Unpin 时统一 `UpdateChecksum()`；
   绕过缓冲池直接改页的代码必须自己调用。见 `docs/architecture.md` §4。

## 提交前检查

```powershell
scripts\build.bat test                    # 编译（零警告，/WX）+ 全部 ctest
python scripts\check_layering.py          # 分层检查必须无违规
python scripts\check_ascii_literals.py    # 进控制台的字面量必须纯 ASCII
python scripts\validate_tree.py           # 不变量自检；M2/M3 起为硬门槛
```

## 踩过的坑

见 [`docs/architecture.md` §7](docs/architecture.md)。包括 MSVC `/utf-8` 顺序问题、
成员函数名遮蔽同名类型、函数式宏吞括号、`std::to_string` 对 `enum class` 失效等
—— 这些都是**真实发生过**并已修复的问题，改动相关代码前请先读一遍。
