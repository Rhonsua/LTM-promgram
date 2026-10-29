# 实现进度（唯一权威进度表）

**规则**：每完成一个任务就勾掉一行并写明提交/日期。里程碑有**硬门槛**，
不通过不得进入下一里程碑（方案 §6）。开始任何实现前先读 `docs/architecture.md`。

状态图例：`[ ]` 未开始　`[~]` 进行中　`[x]` 已完成并已验证　`[!]` 已实现但有未定位缺陷

---

## [x] 日常运行包与生成文件清理（2026-10-28）

- [x] 将既有普通版 `ltm_pipeline.exe`、`ltm_bench.exe`、`ltm_knn_bench.exe` 和配套 C/C++ 运行库集中到工程外层 `LTM-Run/`；4 个启动入口支持完整链路、复查上次库、混合读写及多读者 KNN，无需 Python、Visual Studio 或 Ubuntu。
- [x] 在移除开发环境 PATH 后实跑 4 个入口，均通过；既有库覆盖保护返回预期错误且文件哈希不变。复制到含英文空格目录的完整链路通过。发现现有普通版不可靠地支持中文目录，启动前已增加英文路径检查，使用说明明确列出限制；未改引擎核心。
- [x] 删除 **980 个**可重建符号/对象、Python 缓存及未被正式哈希引用的测试数据库，共 **5,692,091,485 字节，约 5.30 GiB**。保留 **41 个**被正式证据哈希引用的候选文件，跳过链接；测试源码、原始输出、正式数据库、可执行程序和库保留。额外的临时移动验证副本已清除。
- [x] 清理后核对原有 **102 个源码及脚本文件**、三配置 **69 个历史程序及库**哈希均未改变；独立运行包及清理清单复核通过。清单和复核结果在 [维护记录](../../维护记录/20261028/verification.json)，使用入口见 [使用说明](../../LTM-Run/使用说明.txt)。

**后续**：频繁淘汰时的多读者性能定位/优化、扩大工作集与负载分布、冷热策略对照仍待完成；中文目录兼容也可另立修复任务。此轮仅整理与实测运行包，不构成新一轮核心全量、长压测或强杀验收，正式提交状态不变。历史构建目录路径保留；调试符号已清理，需要调试或继续开发时新建构建目录，不覆盖冻结程序。

---

## [x] KNN 固定总工作量单/多读者矩阵（2026-10-16）

- [x] 新增独立 `tools/knn_bench/` 与 `scripts/knn_bench.py`。每轮先装载并关闭磁盘库，再由新进程执行固定总查询，最后由另一新进程复核全部记录和查询；保持相同数据/查询，读者按编号分摊工作量。
- [x] 参考计算与完整答案校验移到计时外，保留全部结果，按 `(距离, 键)` 排名逐项检查键、MBR、载荷。预先完整校验并补齐重开后的页数统计；实际驻留/淘汰与只读区间计数作为硬检查。
- [x] 普通配置 **1,024 条、4,096 次查询、256 次预热**，k=1/16/64、读者=1/2/4、16/256 帧各三次，共 **54 次测量 + 54 次新进程重开**通过。摘要保留最小值/中位数/最大值与延迟分位，未按最快一次挑选结果。
- [x] ASAN 与 Linux TSAN 各 **12 次小矩阵 + 12 次新进程重开**通过，时间不参与性能结论；每配置 **10 项真实程序协议检查**。ASAN 运行库导入、TSAN 竞争正反对照/符号、32 个编译数据库条目的检测参数已核对（实际构建 22 单元）。另补三配置共 **21 项单个非法参数替换**检查，分层/ASCII 通过。
- [x] 独立复核共 **78 次测量、78 次重开**，重建摘要与 CSV，核对源码/程序/数据库。原有 **102 个源码及脚本文件**和三配置历史程序哈希不变。ASAN 检查工具定位首轮失败保留；普通/TSAN 的冻结入口与当前脚本仅一行 Windows 工具定位差异，按实际快照严格桥接，见 [结果和原始证据](../report/optimization/knn_matrix_20261016/README.md)。

**发现与下一步**：本轮驻留场景多读者吞吐提高，淘汰场景增加读者未形成扩展收益。优先定位缺页/淘汰路径的等待成本后再优化；随后扩大工作集和冷热消融。此矩阵只比较当前版本不同读者配置，不证明候选堆相对旧实现的提速，也不代表超出系统内存、冷设备或长压测已完成。未改核心和结题材料，未重新执行核心全量/强杀矩阵；M6-3 全口径与 M7 正式提交仍未完成。

---

## [x] 可重复工程验收、无额外探测轨迹与清理（2026-10-16）

- [x] 清理可重新生成的对象文件、编译器中间符号、链接缓存和 Python 字节码，共 **1,530 个文件、2,726,689,349 字节（约 2.54 GiB）**；源码、可执行程序、库、根调试符号、数据库和验收证据保留。清理脚本逐路径检查、跳过链接并保护哈希证据，两份实际删除清单见 `report/maintenance/`。
- [x] mixed 新增 `--trajectory=measure`，在执行路径省去逐写 Get 探测，使用独立 schema 2 / scope；默认 checked 保留 schema 1。读返回仍完整核对，保留逐 API 统计采样，计时范围明确区分。原查询计数不包含未命中 Get，现单独记账并从键历史复算：每 300 次操作的完整读请求 checked=300、measure=210，后者探测次数、耗时和 I/O 均为零。
- [x] mixed 的 KNN 参考补为完整 `(距离, 键)` 排名及载荷校验，新增同距错键、顺序颠倒、错载荷回归；协议负向覆盖跨 schema、隐藏探测、漏计未命中、坏时间、缺字段、非法轨迹、覆盖保护和真实超时。
- [x] 新增顺序三配置验收入口 `python -B scripts/accept_mixed_measure.py --phase all`，每轮使用独立构建与证据目录。最终普通 **20/20**、ASAN **20/20**、Linux TSAN 相关专项 **3/3**；各两个结构守门和 **18 项协议检查**、分层/ASCII 通过。TSAN 51 编译单元插桩及运行库正反对照已确认。
- [x] 每配置两类轨迹 × 12/128 帧 × 三次，共 **36 次运行 + 36 次新进程重开**通过；输入、操作、查询和最终记录四指纹跨轨迹/配置一致。实际驻留和淘汰由页数与测量期 I/O 证明，样本、CSV、摘要和数据库保留。
- [x] 修复主链路包装器在 Linux 查找 Visual Studio 及固定 `.exe` 后缀的问题；Windows / Linux 共用入口。三配置主链路共六个进程通过，另实跑 Linux 跳过编译和从全新目录配置/编译的两条链路，共四个补验进程。
- [x] 独立复核 **306 对源码/快照**、72 份轨迹结果、CSV/摘要、数据库和程序哈希；兼容解析旧 v1 原始结果 **24 份**。确认 **428 项历史证据不变**，45 个核心文件与上轮 top-k 版本一致。首轮计数口径与旧 Linux 包装器失败保留，最终仅接受同一当前源码版本，见 [验收结果](../report/acceptance/mixed_measure_20261016/README.md)、[验收入口](acceptance.md) 和 `audit.json`。

**范围**：完成固定、有界、单线程 mixed 的工程验收扩展，保留内联参考与逐调用统计；尚非完全无观测性能。未改存储/索引核心、未补完整 Linux 套件、长压测或新强杀矩阵，也未完成系统内存超限、KNN 并发/大 k 性能和冷热消融。未修改结题材料，M6-3 全口径及 M7 正式提交仍按原待办。

---

## [x] KNN 同距 top-k 修复与候选堆优化（2026-10-16）

- [x] 修复 best-first 只按距离筛候选、导致同距边界保留错误键的问题；选择与输出统一按 `(距离, 键)` 升序。用有界大顶堆替换候选最大值扫描与移位，单次接纳的维护开销由 O(k) 降为 O(log k)。同距子树仍须保留，k=0 保持按 1 处理；未改页格式、存储层、WAL 或锁顺序。
- [x] 新增三个回归，旧实现三项均因错误答案失败。覆盖单页逆键和 uint64 键边界、三种分裂策略/三维/12 帧下删除前后的 132 组 fast/brute 完整记录对照，以及 Hybrid 检查点、关闭重开与提前停止。完整结果包含键、距离、MBR 和载荷，避免只比距离漏检。
- [x] 独立 `build-opt-topk*` 构建：最终普通 **20/20**、ASAN **20/20**、Debug 与 Linux 严格 TSAN 专项各 **4/4**；四配置各两个结构守门、分层与 ASCII 检查通过。TSAN 运行库正反对照、插桩和符号已确认；512 条记录、12 帧、新进程重开主链路通过。
- [x] 独立核对当前核心源码、测试、快照、原始输出、二进制及数据库。Windows 最终运行入口早于 Linux 对照加固，按实际执行版本记录并验证差异仅在 Linux 分支；未改写旧清单。确认上轮 182 项证据与 56 个更早的 Windows 二进制哈希不变，见 [本轮优化记录](../report/optimization/knn_topk_20261016/README.md) 和 `audit.json`。

**范围**：候选维护的复杂度改进已实现，尚未量化吞吐量提升。Debug / TSAN 是相关四项专项，未重跑完整 Linux 套件、长压测或强杀矩阵。初次沙箱构建失败、初版测试的临时引用问题、两次运行库正对照漏报均保留，不计作最终通过。后续优先做无额外探测的 mixed 测量、KNN 单/多读者与大 k 实测，再扩大工作集和冷热消融；本轮未修改结题材料。

---

## [x] KNN 读取并行优化（2026-10-08）

- [x] 移除 HybridIndex 整段 KNN 递归互斥锁；RTree 遍历与诊断计数按调用独立，结束时短锁发布，`knn_stats()` 返回受同步保护的值快照。保留共享接口门、写者串行和回调写升级拒绝规则，未改页格式、WAL 或查询排序。
- [x] 新增三个回归：两个读者的回调确实重叠，嵌套 KNN 和提前停止安全；12 帧、96 条记录关闭重开后四读者共 192 次 top-k 对照独立距离模型；优化/暴力查询与统计读取并发，保留快照不变。旧实现运行重叠用例预期失败，原源码与二进制保留。
- [x] 独立 `build-opt-knn*` 构建：普通 **20/20**、ASAN **20/20**，Debug 与 Linux 严格 TSAN 各 **4/4**，四配置各两个结构守门及分层/ASCII 检查通过。ASAN 运行库导入、TSAN 竞争正反对照和插桩已确认。
- [x] 当前普通构建重跑 512 条记录、12 帧主链路：写入、检查点、完整记录核对、关闭、新进程重开核对通过。独立审计核对 396 对源码/快照、原始输出、当前二进制与数据库，并确认 56 个历史 Windows 二进制哈希不变，见 [优化证据](../report/optimization/knn_parallel_20261008/README.md)。

**范围**：确认 KNN 并行能力与本轮正确性；尚未量化吞吐量增幅。Debug / TSAN 为受影响四项专项，未重跑完整 Linux 套件或长时间压测。统计值是最近完成调用的诊断快照，并发时不代表指定查询。初次编译与测试夹具失败记录保留，不计成功。本轮未修改结题材料。

---

## [x] P3-R 检查点与恢复测量限定场景（2026-10-07）

- [x] 新增独立入口 `tools/recovery_bench` 与 `scripts/recovery_bench.py`，在独立 `build-recovery` / `build-recovery-asan` 中构建，未修改存储与索引核心、主 CMake 或历史二进制。
- [x] 同一 Hybrid 操作输入固定为 512 次插入加 512 次更新；每场景一次显式检查点，保留 0/16/64/256/512 次检查点后更新，24 帧、4KB、两树容量 32。各组三次独立数据库、运行次序打乱，普通 **15/15 通过**。
- [x] Prepare 完成全部提交及日志同步后以退出码 9 立即退出，绕过 Close 与析构；Recover 和完整记录 Verify 分别由新进程执行。输入和最终记录指纹由独立 Python 生成，所有组一致；点查、全量扫描和 I8 均通过。
- [x] 普通恢复函数中位数依次为 **1.564 / 13.782 / 51.640 / 224.021 / 462.565 毫秒**，对应 WAL 为 592 / 827568 / 3349816 / 14248680 / 29463656 字节。零更新尾无重放，其余确实重放；存储构造与恢复总耗时另列。
- [x] ASAN 零尾与最大尾 **2/2 通过**且运行库导入已确认；新增结果协议 **17/17**、实际 CLI 拒绝覆盖/非法尾量两个负向检查、分层和 ASCII 守门通过。
- [x] 保存原始输出、参数、构建缓存、源码/二进制哈希、完整崩溃前快照及汇总复算，见 [恢复测量说明](../report/recovery/README.md) 与 `report/recovery/audit_20261007.json`。初次构建清单遇到 Windows 无法遍历 Linux log 链接，已收敛到历史二进制目录后修正，失败输出保留。

**范围**：固定已提交事务的异常进程退出，使用热系统文件缓存，未测事务中途退出的撤销曲线、冷启动、断电持久性或自动检查点阈值；ASAN 耗时不参与性能比较。M6-3 的完整扩展实验与 M7 正式提交仍未完成。

2026-10-07 用户明确取消纯内存基线实验。旧纯内存实现为中期过渡，后续集中磁盘页式版本；下方历史记录中的“纯内存待完成”不再代表当前待办。

---

## [~] M7 结题准备（2026-10-07）

材料统一保存在项目上级新建的 [结题材料](../../结题材料/README.md) 文件夹，本轮按要求准备 Word 正文和实验材料，暂不制作 PPT。

- [x] 可编辑 Word 正文初稿及文本版本，标题与正文统一宋体，覆盖背景、架构、持久化、并发修复、实验方法、查询与恢复性能、运行结果和成果边界；实际 Word 渲染与逐页检查记录保存在材料目录。
- [x] 正文改为项目设计与成果表述，将处理器、系统、编译器、绝对路径与验证版本说明移至配套实验材料；保留实验参数、计时边界和结论局限，新增实验环境与复现信息附件。
- [x] 普通构建补跑五种只读查询 × 两种帧数 × 三次重复，共 **30/30 通过**。参数为 1024 条记录、2000 次正式查询、200 次预热、12/256 帧、种子 42；不与项目内其他压力实验同时运行，但未隔离系统后台活动。
- [x] 保存只读原始输出与数据库快照、查询与恢复汇总 CSV、四幅图表、性能分析、运行与演示说明、成果证据索引及提交前待办。153 个只读快照文件及新增恢复日志/JSON/CSV 逐文件核对；完整恢复崩溃快照保存在工程证据目录。
- [x] 历史核心验收、fixture 补验和 mixed 验收按各自版本引用，未改写旧记录、未声称本轮重跑全部检查器和崩溃矩阵。新增恢复测量工具单独构建、补验与审计，核心文件及历史二进制保持不变。
- [x] 原纯内存对照按用户明确要求取消；固定已提交事务的恢复曲线已补。
- [ ] 学校模板、实际分工、经费与成果栏目、正式提交日期和导师审阅待确认；三倍机器内存规模、无探测混合测量及冷热消融仍未完成。

**本轮完成的是结题准备材料，不代表 M6-3 全口径或 M7 正式提交完成。** 原始实测位于 `report/closing_perf_20261007`，材料目录的 `实验材料/复核结果.json` 和 `_制作校验/排版检查.json` 分别记录数据核对与最终排版检查。

---

## P0：C1 验收补齐（2026-10-06 18:29，历史验收保留）

**P0 已完成其约定范围；当时 M6-2 整体验收尚未通过，当前补齐结果见下面的 Linux / TSAN 小节。** 原始输出与可复现入口见
[验收说明](../report/c1/acceptance/README.md)。

| 项目 | 本轮结果 | 证据 |
| --- | --- | --- |
| ASAN 普通全量 ctest | **16/16 通过**，232.33 秒；运行库导入已确认 | `report/c1/acceptance/asan/ctest.txt`、`manifest.json` |
| ASAN 结构守门 | B+ 与 R 两项退出码 0，I6 违反 0 | 同目录 `validate_bplus.txt`、`validate_rtree.txt` |
| Debug 正确性专项 | **4/4 通过**：混合索引、页闩、固定操作流重放、事务回滚；51.28 秒 | `report/c1/acceptance/debug/ctest.txt` |
| Debug 结构守门 | B+ 与 R 两项退出码 0 | 同目录 `validate_bplus.txt`、`validate_rtree.txt` |
| Debug 默认逆序即 abort | 独立子进程打印 held=10/L0 → requested=20/L2，以预期退出码 3 终止 | 同目录 `order_abort.txt`、`manifest.json` |
| 确定性并发重放 | 3 个固定种子；每种子 3 写 3 读、720 次写操作、12000 次读；逐条对照串行重放与独立 map 模型 | `tests/concurrency/test_c1_replay.cc`；两种构建的 ctest 均含该用例 |
| Ninja 头文件依赖 | 从 0 项修复为 145 项，包含 test_harness.h；触碰头文件的 dry-run 会安排重编译 | `header_dependencies.txt`、`incremental_dependency_proof.txt` |

**本轮修复**：① 中文 MSVC 输出与 Ninja 缓存前缀不一致，导致头文件修改不重编译；
新增编译启动器统一输出和依赖前缀，验收入口主动检查依赖表。② HybridIndex::Validate
在 R 树扫描回调内获取 B+ 根页，Debug 实测逆序；改为先收集键、释放页锁，再查 B+，
同时持 write_mutex 保证双树校验期间无写入。额外内存为 O(N) 个键，写者会等校验完成。
③ 测试框架禁用 Debug 断言弹窗并及时刷新 stdout，失败直接留日志。

**范围限制**：Debug 完整普通套件曾人工中止耗时的读扩展性测量，不记作全量通过；
最终 Debug 证据是上述 4 项专项及两个结构守门。确定性重放仅覆盖 B+、heat=nullptr、
互不相交的写键空间和不可变记录读；不证明 HybridIndex 全部并发 API 或同键冲突语义。
原 30 分钟 crabbing 压测本轮未重跑，保留为其原配置的历史证据。
P0 时 HeatSource、部分共享统计与组合查询并发尚待审计；当前进展见下面的 P1 小节。
MSVC 缺少 TSAN 的限制仍在。

## 当前状态速览

### [x] P2 扩展：固定、有界的混合读写基准（2026-10-07）

已接入 `python scripts/mixed_bench.py`：每百次固定 Get/range/spatial/contain/KNN/
Put/Erase 数量 35/15/10/5/5/15/15。独立 map 预先生成完整操作流，覆盖新插、
改 MBR/载荷、删除重插和不存在键删除；每次读/写后探测核对完整记录，末尾全量
扫描与结构/I8 校验，再启动新进程重开核对。使用独立 mixed schema，保留旧只读判据。
按操作逐次记录 API/探测计时及 13 项计数，CSV、分位数与汇总可由原始 samples 重算。
驻留依据整段轨迹的物理页地址上界且零淘汰；淘汰须有真实 API 页读及淘汰。

最终普通全量 **20/20（326.85 秒）**、ASAN 全量 **20/20（407.37 秒）**，两构建并行执行；
各两个结构守门、18 个 Python 协议检查及分层/ASCII 检查通过。
Linux 严格 TSAN/Werror 构建 51 编译单元，两个受影响参考测试通过；新参考回归在
三配置各四用例通过，包括三种子、每种子 100000 操作的小数据下界回归。
512 条/300 操作、12/128 帧各三次：三配置共 **18 轮 + 18 次新进程重开全部通过**。
输入及最终四指纹跨配置一致；最终 510 条、峰值 518 条、上界 640 条，I8 对数 510。
普通/ASAN 的旧 point/range/spatial/contain/knn 各 30 轮回归通过，共 60 轮。

首次验收发现新夹具默认 R 树容量无法容纳测试载荷；改为基准使用的容量 32。
长轨迹新增用例复现固定删除前缀跌破下界；随机删除现在为下一块保留一条余量。
失败轮次保留，不计作成功。独立复核 95 个源码/入口、全部原始结果/CSV/汇总和数据库，
45 个非基准核心 src 与核心库未变，112 个历史二进制保留。
本轮 Linux 为基准受影响路径补验；未重新执行历史完整 TSAN、强杀矩阵或长压力。
探测会改变缓存状态，本版仅报告带校验轨迹的观察；无探测性能、三倍机器内存数据集、
原纯内存实现对照及 M6-3/M7 全口径仍待完成。
参数、证据与复跑入口见 [混合基准说明](../report/bench/mixed/README.md)，独立复核为 audit.json passed。

### [x] Linux / TSAN 与 M6-2 C1 验收补齐（2026-10-07）

WSL2 Ubuntu 26.04.1 已安装且可运行；Clang 21.1.8、CMake 4.2.3、Ninja 1.13.2
及 TSAN 运行库已就绪。真实竞争对照按预期报告 data race 并退出 66，加锁对照退出 0。
严格 Clang 构建发现的前置声明、显式字节转换、缺失 include 和未使用函数已修复，
49 个编译单元均带 TSAN；Windows 普通与 ASAN 构建也已完成。
已发现并修复 RegisterRollback 未持 structure_mutex 重写 root/height 的真实竞争。
新增不存在键删除并发读回归：128 次回滚与 6144 次读；修复前 TSAN 退出 66，修复后通过。
完整日志另发现旧 Hybrid 恢复夹具持底层写锁进入上层 API 的锁序环；已改为真实 Put
页镜像 + 缺少最终 Commit 的 WAL 前缀，不再析构仍持锁的缓冲池，不冒称实际强杀。
修复后 TSAN 并发重放、恢复夹具、启用热度的混合 API 三项专项通过；Windows 普通
全量 19/19（155.82 秒）、ASAN 全量 19/19（220.39 秒）及两种构建的守门/链路通过，
Debug 五项专项、双树守门与预期逆序 abort 通过。
当前核心完整 Linux TSAN 19/19（679.39 秒）、双树守门与 512 条记录跨进程链路均通过，
故障注入矩阵 78/78（72 项实际强杀）通过。Debug 单库连续负载 1800 秒通过：
6 读 2 写、20000 键、512 帧，2441884 次操作、443124 次淘汰、179 次检查点；
最终 18754 条记录逐条核对，错误载荷、调用失败、I6 与锁序违反均为 0，无死锁/挂起。
验收含装载和结束校验共 1836.20 秒，证据为 long_soak_debug/。
完整 Linux 入口 scripts/accept_tsan.py；普通测试的 log 目录改用持久化 ext4 磁盘，
保留完整操作量和 fsync，不使用 tmpfs。位置见 report/c1/tsan/storage_location.json。
attempt_01/ 保留严格构建失败，attempt_02/ 保留锁序警报和中止轮次，当前全量在 attempt_03/；
Windows 最终回归为 windows_verified/，Debug 为 windows_debug/debug/。
详情见 [TSAN 说明](../report/c1/tsan/README.md)。本节覆盖旧章节的「本机无 TSAN」现状描述。

核心全量证据由 attempt_03/audit.json 独立复核：90 个 Linux 源码/入口文件、27 个
Linux 二进制、54 个 Windows 二进制、6 个跨进程链路结果及原始输出/数据库哈希。
之后仅补修三份测试夹具的清理顺序，避免在池/磁盘析构前删除文件；普通、ASAN、
Debug、Linux TSAN 各完整 HybridDiff 9/9 与两个原操作量守门程序均通过，无同步或
检测器错误，成功文件已清理。fixture_cleanup_audit.json 确认只改三份 tests/index 文件，
其余程序及静态库、核心与压力程序不变；旧全量 manifest 不改写成补修后的哈希。
这是冻结全量版本 + 受影响目标复测的组合证据，未冒称补修后重跑全量或 30 分钟压力。

| M6-2 门槛 | 当前证据 | 状态 |
| --- | --- | --- |
| 单库混合读写 30 分钟无死锁/挂起 | 本轮 Debug 淘汰配置 continuous soak | 通过 |
| Debug 锁序检查，合法路径零逆序、故意逆序 abort | 五项专项、双树守门、预期退出 3 的逆序子进程及长压力 | 通过 |
| ASAN 普通全量与结构守门 | 19/19；测试清理补修后受影响目标另复测 | 通过 |
| 固定输入多/单线程结果一致 | 三种子 B+ 重放、完整记录与独立 map 对照，含并发读；本轮套件再次执行 | 通过 |
| 实际数据竞争检测 | Clang TSAN 19/19、双树守门、跨进程链路、竞争正反对照及插桩证明 | 通过 |

**M6-2 的约定 C1 验收范围已完成。** 结论限于实际执行的配置、接口与操作流；
原始 pool/disk、底层直接调用和对象销毁仍须遵守外部同步契约。同键任意交错的
线性化证明、C2、多写者和后台任务未由这些有限测试覆盖；M6-3 全部实验及 M7 未完成。

### [x] 完整主链路入口（2026-10-07，正常落盘与跨进程重开）

`python scripts/run_pipeline.py` 已实跑通过：自动构建 → 写入 512 条二维记录 →
检查点 → 点查/范围/空间相交/包含/KNN 及完整记录核对 → 关闭 → 新进程重开
同一数据库 → 再次核对 → 留存数据库、日志和 JSON 报告。重开逻辑写入次数为 0，
两个进程的完整记录指纹一致；51 页双树工作集、12 帧缓存，页读/淘汰真实发生。
普通与 ASAN 链路均通过；协议测试 5/5、正确性专项 2/2、分层/ASCII 检查通过。
已有库保护、缺库、错误真实载荷及超时均有实测；错误结果不得生成成功报告。
首次入口轮次未改 48 个 src 文件，未重跑全量套件、结构守门或长压测；
后续元数据修复已复测全量套件、守门和链路，见下节，历史入口证据仍保留。
最终证据与复跑入口见 [完整链路说明](../report/pipeline/README.md)。

### [x] 已有库元数据页校验修复（2026-10-07，最终验收通过）

已修复 LoadMeta 跳过 CRC/自描述、将错误 magic 当作新库的问题。已有元数据在
修改前检查原始页完整性、物理页号、类型/层级/tree_id、magic 和版本；短文件拒绝。
首次初始化要求物理空文件且无 WAL 记录或历史检查点标记；日志仅剩旧检查点头
也不算新库。初始建树回滚的空壳须匹配同 LSN 的完整 CLR 镜像，才允许重新初始化。
有效 WAL 重放可修复非正常退出的损坏；没有可用镜像时明确拒绝，并保留数据文件。
最终源码普通全量 **19/19（148.18 秒）**、ASAN 全量 **19/19（235.55 秒）**；
两个构建的元数据专项各 **7/7**，两棵树守门、正常重开链路及损坏副本拒绝均通过。
故障注入矩阵 **78/78**，其中 **72 项实际强杀**；恢复后与旁路已提交记录逐条一致。
Python 协议 13/13、分层与 ASCII 检查通过。独立复核 88 个源码/入口文件、82 个
二进制及原始输出/保留数据库哈希；48 个 src 文件仅改存储层 3 个，其余 45 个不变。
修复前失败、检查点头补充用例和当前验收见 [元数据说明](../report/metadata/README.md)。
最终证据为 report/metadata/acceptance_complete/，中间失败与中止轮次未计入成功。
该历史轮次未重跑 30 分钟 C1 压测，未改变查询/迁移/页闩锁实现；当时未做 TSAN，当前见文首。
固定操作流混合读写的后续完成见文首；M6/M7 不据此整体完成。

### [x] P2 扩展：Hybrid 空间/包含/KNN 基准（2026-10-07）

已接入 spatial/contain/knn 的真实查询、独立几何参考计算及完整记录核对，
扩展 JSON/CSV 和严格协议校验；KNN 接受边界同距离候选，但拒绝遗漏更近记录。
报告如实区分 Hybrid 热度/迁移策略与 B+ CLOCK，并分别记录双树总页数和查询页集合；
缓存介于两者之间的配置不纳入驻留/淘汰对照。相交/包含样本确有不同结果集合。
1024 条、500 次查询、12/256 帧、每组三次，最终独立运行 18 次全部通过；
ASAN 512 条、200 次查询、8/128 帧同样 18 次全过，B+ 回归对照 12 次全过。
普通全量 ctest **18/18（139.12 秒）**、ASAN 专项 **2/2（9.36 秒）**、
两个结构守门及 8 个协议测试通过；负向参数、已有库保护、模糊缓存拒绝有实测。
第二次复核核对 48 次报告、测试日志、源码/二进制哈希及 45 个未变内核文件。
该历史轮次仅改基准/驱动/参考判据测试，未重跑 C1 长压测或全量 ASAN；当前 TSAN 见文首。
参数、数据和限制见 [Hybrid 基准说明](../report/bench/hybrid_README.md)。
M6-3 全口径与 P3/M7 仍未完成；元数据校验及固定混合读写的后续完成见文首。

### [x] P2：B+ 最小基准闭环（2026-10-07，限定 point/range）

已用真实查询替换 ltm_bench 骨架：固定输入、完整记录核对、独立装载/预热/测量、
查询计时与分位数、池/IoStats 测量差值、结构化 JSON、CSV 及重复分布均贯通。
bench.py 严格检查退出码、必填字段、参数、输入指纹、实际驻留/淘汰路径及结果，
空跑、缺指标与延期选项不再计为成功；每轮使用独立数据库和输出目录。
2048 条记录、100 页工作集，point/range、12/512 帧、每组三次：12 次对照全过。
ASAN 512 条记录、8/128 帧同样 12 次基准运行全过；协议测试 5/5，负向 CLI 与
超时失败判据通过，普通全量 ctest 17/17（125.19 秒），两个结构守门退出 0。
版本、参数与原始输出见 [基准说明](../report/bench/README.md)。
本轮只改基准入口和驱动/协议测试，未改索引、存储、热度与页闩实现；未重跑 C1 长压测。
该轮未覆盖 mix/spatial/knn、不同页大小、预取、自动检查点、恢复曲线或并发基准；
空间/KNN 的后续进展见上节；M6-3 全口径及 M7/P3 仍未完成。

### [x] P1：热度与 HybridIndex 并发边界（2026-10-07）

已实现 HotColdTracker 全部共享状态同步、值统计快照、HybridIndex 完整逻辑操作的
并发门及读回调重入边界；新增显式 DecayHeat 与启用热度的混合并发测试。
最终源码 Debug 正确性专项 **5/5（39.67 秒）**、两个结构守门及预期逆序 abort 均通过；
ASAN 普通全量 **17/17（186.83 秒）**及两个结构守门均通过，普通构建新增用例 4/4。
最终验收与源码哈希在 report/c1/heat/verified_acceptance/，详见
[P1 证据说明](../report/c1/heat/README.md)。
2026-10-06 两组 30 分钟压测通过：持续 crabbing 91710073 次操作、淘汰配置、
错误/I6/锁序违反均为 0；启用热度的有界重复负载 255 轮全部通过、1800.18 秒。
这些是生命周期补验修复之前的独立版本证据，不冒称最终源码完整重跑了 30 分钟。
补验定位并修复同对象 Close → Open 的资源释放顺序错误；新增同对象多次重开、
打开失败后重试回归，最终独立 ASAN 复现退出 0。失败与修复后输出均保留。
最终源码另补短时重复压力验证，18 轮全过、63.43 秒，证据在 report/c1/heat/soak_final/；
final_verification.json 已确认 ASAN/Debug 验收记录的 77 个源码文件哈希与当前文件一致。
P0 节的 HeatSource 待验描述是本轮开始前的状态；当前以本节为准。
该历史轮次 M6-2 尚未完成、未做 TSAN；当前门槛补齐见文首。原始 pool/disk 与底层树 API 不获得该轮
HybridIndex 接口门的同步保证，未实现后台衰减或主动刷脏。

| 层 | 状态 | 说明 |
| --- | --- | --- |
| `util`（status/log/crc32c/types/config） | `[x]` | 完整实现 + 单测 |
| `storage/page` | `[x]` | 完整实现 + 属性测试（随机操作、压实精确性） |
| `storage/disk_manager` | `[x]` | 真实文件读写、WAL 追加/截断、`IoStats` 全字段计数；**`SyncFstream` 是真正的设备屏障**（`_commit` / `fsync`，失败返回 `kIoError`），不再是裸 `flush()` |
| `storage/buffer_pool` | `[x]` | 帧表、pin/unpin、CLOCK 淘汰、帧占用检查、元数据页访问器、**复用空闲页时复用同一帧**（本轮修的静默损坏根因）；**M4-2 起持有写事务**（`BeginWriteOp`/`CommitWriteOp` 可重入）+ delta 采集（`Unpin` 时取 after-image）+ `FlushFrame` 的 I6 写前规则与可证伪断言；**M4-3 起 I6 只留一条豁免判据（日志为空），`enforce_i6` 参数已删除**；析构时 FlushAll 之后标 `clean_shutdown`（有未提交写事务则不标） |
| `storage/meta_page` | `[x]` | 页 0 的唯一权威布局（物理页管理 + 每棵树一个槽位）；**读取前必须 `EnsureMetaLoaded()`**；建库分支重写页 0 时**保留 `page_lsn`**（否则恢复刚写下的 CLR LSN 会被抹掉，紧接着撞 I6） |
| `storage/wal` | `[x]` | 记录编码 + 顺序追加 + 组提交 + **Commit 前 fsync 屏障**；M4-2 起**已接入真实写路径**（`log/*.wal` 非零、页头 `page_lsn` 真实存在、跨运行 LSN 单调）；**M4-3 起有 512 字节日志头**（magic/format/`last_checkpoint_lsn`/`clean_shutdown` + crc32c）、**带记录级 crc 的扫描器**、**CLR 记录**、检查点记录；**UNDO 记录写在 REDO 之前**（缺陷 #38） |
| `storage/recovery` | `[x]` | **M4-3 完整实现**：Analysis / Redo / Undo + CLR、撕裂页与坏校验和的强制重放、检查点（begin → flush_all → end → 更新头部 → 截断）、`MarkCleanShutdown`；`RecoveryManager` 持有 `WalWriter`（恢复期写的 CLR 与后续正常写共用一条 LSN 序列） |
| `index/tree_ops`（契约） | `[x]` | 契约 v3 + **`IndexTree` 通用引擎落地**（跨页算法只写一遍）；共享面 = 结构维护半边，查找半边各树自实现 |
| `index/bplus_tree` | `[x]` | 页内算子 + 跨页算法完整；**M2 门槛已通过**（20000 次随机读写后不变量全绿） |
| `index/rtree` | `[x]` | 页内算子 + 三种分裂策略 + best-first KNN；**M3 门槛已通过**（I9 精确包含 / I3≥40% / KNN 与暴力等价）；**M5 新增 `InsertReturningLeaf` / `Locate`**（按键定位：提示优先、失效退化为叶链全扫，带命中率计数） |
| `index/index_entry` | `[x]` | 统一记录表示（键 + MBR + 载荷）+ `PayloadCodec` 往返；**M5 起新增 `rtree_leaf`**（混合索引在 B+ 侧记录的定位提示，只给 I8 交叉校验用） |
| `index/hybrid_index` | `[x]` | **M5 完整实现**：`Open/Close/Put/Erase/Get/Query/Scan/Knn/Checkpoint/Validate/stats`；**一个写事务罩住两棵树**（`ScopedWriteOp` 可重入，三层嵌套只提交一次）；记录（键+MBR+载荷）存在 B+ 侧，**`Get` 不访问 R 树**（`Erase` 需先从 B+ 记录取精确 MBR 再删 R 树 —— 别读成"Erase 也不碰 R 树"）；I8 双向交叉校验且**可证伪** |
| `index/index_registry` + `mock_tree` | `[x]` | 完整实现（通用性活样例）；`HybridIndex::Open` 经它按名字建树 |
| `migration`（热度/驱逐策略） | `[x]` | 完整实现 + 测试 |
| `concurrency` | `[x]` | C1 约定范围完成：普通/ASAN/TSAN 各全量 19/19，Debug 专项 5/5、双树守门、预期逆序 abort、固定操作流对照和本轮 Debug 连续 30 分钟淘汰压力通过。清理补修只改测试，四配置受影响目标另复测并独立复核；底层直接 API 和对象销毁须外部同步，C2/后台任务未完成。 |

**可复现的验证结果**（M5 完成后全部实测，零编译警告）：

> **本机复现（2026-09-27）**：下面这组数字由交付方在其机器上产出；项目迁回本机
> （`D:\db_project\LTM\ltm-index`，MSVC 14.51.36231 + Ninja）后已**逐条重跑**：
> `ctest` 12/12、两个守门程序退出码 0 且 `WAL deltas` 与下面**完全一致**
> （43024 / 25551）、崩溃矩阵 **78 例全过（72 例真崩溃）**。
> 也就是说这些证据在本机是可复现的，不是只在交付方机器上成立。
>
> **本机对交付内容做过的一处口径修改**：M6-2 的验收原写"TSAN 干净"，但本机唯一可用的
> 构建器 MSVC 不支持 ThreadSanitizer，已改为「ASAN 干净 + `LockOrderChecker` 零逆序 +
> 30 分钟压测无死锁」——详见 `终版方案.md` §6 M6 脚注。

```
python scripts\build_local.py test        → 100% tests passed, 0 failed out of 12（79.0 s）
python scripts\build_local.py test build-fi  RelWithDebInfo → 12/12
python scripts\build_local.py test build-asan RelWithDebInfo → 12/12（ASAN 确已生效，见 report\m4_3\asan_proof.txt）
python scripts\build_local.py test build-ninja RelWithDebInfo
                                          → 100% tests passed, 0 failed out of 15（107.7 s）
                                            （2026-10-01，加完压测驱动后复跑：驱动与 CMake 改动
                                              不碰 src/，结果不变）
python scripts\build_local.py test build-ninja RelWithDebInfo
                                          → 100% tests passed, 0 failed out of 15（111.4 s）
                                            （2026-10-01，C1 前置件（二）的 8 条新用例已计入；
                                              原始输出 report\c1\ctest_after_page_latch.txt）
python scripts\build_local.py test build-ninja RelWithDebInfo
                                          → 100% tests passed, 0 failed out of 14（121.1 s）
                                            （同日早些时候，M6-1 的 3 条 + C1 前置项的 4 条新用例；
                                              原始输出 report\abort_rollback\ctest_after_fix.txt）
  注意：build_local.py 的 test 子命令**不隐含 build**。换过源码后必须先 build 再 test，
        否则 ctest 跑的是陈旧二进制 —— 本轮吃了一次这样的假红（见 report\c1\README.md 第八节）。
build-ninja\ltm_validate_bplus.exe --quiet   → 退出码 0（M2 门槛）
  参数 ops=20000 capacity=32 frames=128 seed=42
  规模 height=2 pages=157 entries=3176（插入 12097 / 删除 7903）
  结构 分裂 201 / 合并 47 / 借键 818，平均填充率 63.2%
  缓冲池 命中率 99.0%，驱逐 1617（脏页 626）
  I/O  读 1589 页 / 写 627 页
  WAL  delta 43024，LSN 戳 43024（漏 0），写前 Sync 3，**I6 违反 0**
build-ninja\ltm_validate_rtree.exe --quiet   → 退出码 0（M3 门槛）
  参数 ops=8000 capacity=32 frames=32 dim=2 policy=quadratic seed=42
  装载后 height=2 pages=90 entries=1788 平均填充率 62.1%
  KNN 访问 6 页 / 暴力 20 页，剪枝率 70.0%
  WAL  delta 25551，LSN 戳 25551（漏 0），写前 Sync 280，**I6 违反 0**
build-ninja\test_recovery.exe                → 7 passed, 0 failed
  A 干净关闭无需 Redo/Undo        B 未提交事务被 Undo 回滚（clrs == undo records）
  C 恢复连跑两次结果相同          D 撕裂页 1 + 坏校验和 1 被重放修回
  E 检查点把日志从 5440796 截到 592 字节，恢复从 checkpoint_lsn 起重放 122 条
  F 日志头往返 + 坏 crc 判损坏 + 无头旧日志被拒
  G **152 个日志前缀**（记录边界 + 记录中间）逐个恢复，152 个自洽、0 失败
build-ninja\test_hybrid_diff.exe             → 9 passed, 0 failed
  A Put/Get/Erase 往返（含覆盖式 Put 换 MBR）   B 随机差分 904 插入 / 209 删除，两树一致
  C 一维范围 vs 暴力逐元素相同                  D 空间相交/包含 + KNN(10) vs 暴力相同
  E **Get 连做 100 次不动 R 树的定位计数**（证明 MBR 来自 B+ 侧）
  F 关闭重开后全量可查                          G 检查点前后查询结果相同
  H 未提交的 Put 被**两棵树一起**回滚（双树原子性）
  I I8 交叉校验可证伪（故意让两棵树对同一条记录说不同的话，检查报出具体键号）
python scripts\crash_matrix.py                → **78 例全过**（72 例真的崩了），report\recovery_matrix.md
  其中 36 例跑 `--engine=hybrid`：同一条判据额外覆盖 I8
python scripts\check_layering.py              → 扫描 46 个源文件，0 违规
python scripts\check_ascii_literals.py        → 扫描 71 个文件，0 个非 ASCII 字面量
build-ninja\ltm_demo.exe                      → 退出码 0，实跑 B+ / R / **混合索引**三棵结构
```

**C1 前置件（四）· 缺陷 #46 修复的判据**（2026-10-04，原始输出在 `report/c1/`）：

```
build-ninja\c0_boundary_probe.exe 8 16 20000   → 5 次跑 5 次绿
  修复前：5 次跑 5 次红（Segfault / not_found=219256 / claims page_id=596...）
  修复后：wrong_value=0 not_found=0 bad_status=0 exceptions=0，tree.Validate()=ok
           misses during measurement ≈ 3.4e5（evicting：池 16 帧 << 工作集 647 页）
build-ninja\soak_mixed.exe 20 4 2 20000 64 6 5  → 5 次跑 5 次 SOAK_VERDICT: clean
  REGIME: evicting；read_wrong_payload=0 call_failures=0 i6_violations=0
  final: validate=ok scan=ok；checkpoints=4，peak_wal ≈ 55–61 MB
  修复前：5 次跑 5 次红（Unpin count exceeds the pin count / validate=kCorruption）
两次单变量证伪（均逐字节还原，源码里 0 个实验标记）：
  ① 未命中路径改共享锁 → 立刻重现 #46：not_found=102811、
     "claims page_id=617, but the requested page is 3"、另两次进程直接死掉
  ② Search 不拷载荷    → 5/5 红：wrong_value=1..2，而 not_found=0、Validate()=ok
python scripts\build_local.py test build-ninja RelWithDebInfo → 15/15 passed（244.8 s）
```

**M5 的证据文件**：

```
report\m5\summary.txt             M5 交付了什么、怎么验的、哪些边界是诚实的
report\m5\test_hybrid_diff.txt    混合索引 9 个用例的完整输出
report\m5\demo.txt                演示程序的完整输出（含混合索引的四条查询路径与实测规模）
report\recovery_matrix.md         崩溃矩阵（含 36 例混合索引）
```

**M4-3 的证据文件**（见 `report/m4_3/summary.txt`）：恢复单测、四道闸门、ASAN 生效证明、
78 例崩溃矩阵的逐例原始输出。

**注意 M2 基线数字已不可比**：M4-2 的 D4 修掉了"每次 `Fetch` 都无条件写 `tree_id`"
（即**每次读都在白写 4KB 回盘**）。因此命中率 99.4% → 99.0%、驱逐 902（**全部脏**）
→ 1617（脏 626）、写盘 627。这不是回归，是原来那些写盘本来就不该发生。
D4 的 before/after 单独产过一组表（`report/m4_2/bplus_before_d4.txt` /
`bplus_after_d4.txt` / `rtree_*_d4.txt`，漂移逐项解释在 `d4_d5_drift.txt`）——
这正是"把 D4 与接线两个效应分开解释"的做法。

**ASAN 配置下的复现**（`build-asan/`，配置命令 `-DLTM_ENABLE_ASAN=ON`）——
M3 门槛里「ASAN 全绿」这一项，是 M3 收尾时才**第一次真正跑起来**的（根因见缺陷 #37）：

```
python scripts\build_local.py test build-asan RelWithDebInfo  → 100% tests passed, 0 failed out of 11（97.9s）
build-asan\ltm_validate_bplus.exe --quiet                 → 退出码 0
build-asan\ltm_validate_rtree.exe --quiet                 → 退出码 0
```

（M3 收尾时是 9 项；M4-2 新增 `test_wal_barrier`、M4-3 新增 `test_recovery` 后为 11 项，已重跑。）

> **跑 ASAN 前必须让 ASAN 运行库在 `PATH` 上**：MSVC 的 `/fsanitize=address` 是动态运行库，
> 找不到 `clang_rt.asan_dynamic-x86_64.dll` 时**整个 ctest 会以 `0xc0000135` 全红**，
> 看起来像 10 个用例全挂，其实一行用户代码都没跑。`0xc0000135` 是 STATUS_DLL_NOT_FOUND，
> 遇到它先查 `PATH`（见 `docs/architecture.md` §8），不要去看失败用例的断言。
> 这与 #37 是同一类问题：「门槛看起来跑了」不等于「门槛真的跑了」。

「ASAN 确已生效」的判据不能只看"构建成功"（这正是 #37 的教训），要看到这两条之一：

```
ASAN_OPTIONS=verbosity=1 <exe>   → "AddressSanitizer: libc interceptors initialized" + shadow 内存布局图
exe 导入表含 clang_rt.asan        → 例：grep -a -c clang_rt.asan build-asan\test_status.exe 返回 1
```

> **两个口径限制，别把这条门槛读得比实际更强**：
> 1. **Windows 上 MSVC ASAN 不支持 LeakSanitizer**，运行时报
>    `detect_leaks is not supported on this platform`。故它覆盖的是越界 / UAF / 野指针等
>    **内存错误，不覆盖内存泄漏**；要覆盖泄漏须在 Linux/GCC 或 Clang 下跑。
> 2. **`clang_rt.asan_dynamic-x86_64.dll` 不是"需要另装 clang"**：它是 MSVC 自带的 ASAN 运行库
>    （随 VS 发布在 `VC/Tools/MSVC/<ver>/bin/Hostx64/x64/`）。`vcvars64` 会把它加进 `PATH`，
>    但从 bash/`ctest` 直接启动 exe 时不会 —— 缺它报 `0xc0000135`（STATUS_DLL_NOT_FOUND），
>    **那是环境问题、不是代码问题**，别误判成"ASAN 跑不起来"而放弃这条门槛。

> 上面每一行的**程序自身输出现在都是纯 ASCII**（`src/`、`tests/`、`demo/`、`scripts/` 的
> 字符串字面量已全量 ASCII 化，注释保持中文）。`report/*.txt` 与 `report/*.csv` 也已重新生成，
> 三个证据文件经扫描均为纯 ASCII。动机与判据见文末「已知开发陷阱」最后两条。

**量化证据**（`report/rtree_split_policy.csv`，由 `ltm_validate_rtree --policy-csv` 自动写出，
不经手誊抄）：三棵树跑同一份 3000 条条目、同一组查询点，只换分裂策略 ——

| 策略 | 页数 | 平均重叠面积 | 分裂 | KNN 页访问 | 剪枝率 |
| --- | --- | --- | --- | --- | --- |
| even | 154 | 24 076 993 | 151 | 8994 | 69.4% |
| linear | 153 | 723 193 | 150 | 2002 | 93.1% |
| quadratic | 145 | 65 761 | 142 | 1118 | 96.0% |

三个策略的重叠面积相差 366 倍、剪枝率相差 26 个百分点 —— 这是"三种策略确实是三份
不同的实现"的实测证据（若三者数值雷同，说明分裂策略其实没接上）。

M2 阶段另用三个临时诊断程序（定位完成后已删除）做了逐操作自检的加压验证：

- 顺序插入 → 全量点查 → 全量删除，**每步**跑一次不变量自检：
  `capacity = 4 / 8 / 16 / 32`、最多 1280 条，全部通过；
- 随机插入/删除差分负载（seed 20240926）：`capacity = 8 / 16 / 32` ×
  `frames = 8 / 32 / 128`、6000 步、**每步**自检，全部通过；
- M2 门槛同构负载（capacity 32 / frames 128 / 20000 步 / 键空间 20000）逐操作自检通过。

---

## ⚠️ 遗留缺陷

### 原 #2「首次分裂后的第一次删除会找不到键」——**已解决**（根因与本轮全部改动见下）

**根因（一句话）**：内部页路由方向反了。`ChildSlotFor` 返回的是"第一个键 >= key 的槽"
（LowerBound 的原始结果），而槽 `i` 的语义是"承载 `[keys[i], keys[i+1])`"，正确的承载槽
是**最后一个键 <= key 的槽**。方向反了以后，树第一次分裂之后**只有最右子页里的键能被找到**，
其余键的点查与删除都报 `kNotFound`。

为什么以前没被发现：顺序插入永远命中最后一个槽（`LowerBound` 返回 `num_slots` 后被夹到
最右槽），而"顺序插入 + 全量扫描"这条用例只依赖叶链、不依赖内部页路由 —— 两者一起把
这个错误掩盖了。修好路由之后，删除路径立刻暴露出下面一串真实缺陷。

### 原 #3「`wal.cc` 缺少 fsync 屏障」——**已解决**（物理一半也已补上）

`WalWriter::Commit()` 现在**无条件** `Sync()` 之后才返回（组提交只能推迟"批量刷盘"，
不能让"已提交"退化成"还没落盘"）。

**两处修复，缺一不可**：

1. **语义层**：`Commit()` 无条件同步（组提交只批量、不推迟"已提交"这个语义）。
2. **物理层（本轮补上）**：`disk_manager.cc` 的 `SyncFstream` 此前在**两个平台分支上
   都只 `flush()` 然后返回 `Ok`**。`flush()` 只把用户态缓冲交给内核，字节仍可能停在内核/
   设备缓存里，断电即丢 —— 也就是说 `Commit()` 的"已同步"此前是**不成立的**，
   不变量 I6 的物理一半是空的。现在 Windows 走 `_commit`（`FlushFileBuffers`），
   POSIX 走 `fsync`，失败会返回 `kIoError` 而不是静默 `Ok`。

`SyncFstream` 用**第二个句柄**做屏障的原因（Windows 按*文件*缓存脏页，POSIX `fsync` 作用在
open file description 上）、以及为什么不能直接裸 `flush()`，都写在 `disk_manager.cc` 的函数头注释里。

**这条修复按性质无法用单测证明** —— "断电后字节还在不在"不是进程内可断言的东西。
它的端到端证据是 M4-3 的崩溃矩阵（`scripts/crash_matrix.py` + `tests/crash/`）。

**仍存的实际边界**（诚实记录，不要误读为具备崩溃一致性）：

- ~~**WAL 这一层目前还没有任何调用方**~~ —— **M4-2 已接线**。`BeginOp`/`AppendPageDelta`/
  `Commit` 现在由 `BufferPool` 的写事务路径真实调用（入口是 `IndexTree::Open/Insert/
  Erase/Destroy` 与 `RTree::Insert` 里的 `ScopedWriteOp`），`log/*.wal` 已是非零字节；
  页 LSN 与 `flushed_lsn` 的断言也已落地（`strict_i6_barrier`）。详见 `report/m4_2/`；
- 没有 `LogFileHeader`、没有 checkpoint、没有 Analysis/Redo/Undo（M4-3）；
- **WAL 永不截断**：`max_wal_bytes` 声明了但无人执行，检查点入口全是 `NotImplemented`，
  所以一次长跑会无界增长（实测 20000 次操作 ≈ 356 MB）。这也是 `scripts/soak_m4_2.sh`
  每轮开跑前先删日志的原因；

也就是说：**"提交后强杀不丢已提交数据"在物理上已经具备条件，"崩溃恢复"仍未实现**
（崩溃后不会重放，只会按当前元数据打开，见 `tests/integration` 与 `scripts/crash_matrix.py`）。

### 已知结构性局限（非缺陷，记录备查）

- **`ScopedWriteOp::Abort` 不回滚内存页**：它只写一条 kAbort 记录。崩溃恢复是对的
  （未提交事务会被 Undo），但**运行期**在两棵树之间失败会留下内存不一致。
  M5 的 Put/Erase 把输入校验全部前置（维度、MBR 合法性），所以要触发它需要一次 I/O 错误。
  修法是页级 undo 缓冲（M4-4/M6 材料）。
- **`HybridIndex::Validate()` 是 O(n) 次按键定位**（逐条交叉校验，不是抽样）。
  它是离线自检，不适合放进热路径。
- **R 树 upsert 仍是 O(叶页数)**（见下面"R 树的 upsert 是 O(叶页数)"一条）；
  M5 没有改变它，只是把它从 Get/Erase 的路径上挪开了。
- **`kAllocPage` / `kFreePage` 记录被写出但不被重放**。空闲链的权威来源是页 0 自己的
  REDO 镜像 —— 事务内的分配/回收都体现在它的 delta 里，两边都重放会重复施加。
  事务外 `NewPage`（建库引导那条路）仍然不进日志；那条路径今天没有调用方。
- **检查点不能在写事务中途做**：截断会把这个事务的 UNDO 一起截掉，
  `CheckpointManager::RunCheckpoint` 直接拒绝（`write_op_active()`）。
  本设计的检查点由应用在两次操作之间发起，所以这条约束不带来不便。
- **`clean_shutdown` 的短路**：打开时读到干净关闭且无 loser ⇒ 跳过 Redo/Undo。
  这是正确的（干净关闭时 FlushAll 已把一切落盘），也是检查点能压缩恢复时间的同一原理；
  但它意味着"想验证 Redo 的测试必须制造一次不干净的结束"（崩溃矩阵就是这么做的）。
- **WAL 只在检查点截断**：`max_wal_bytes` 声明了但仍无人执行，长跑不检查点会无界增长。
- 内部页的 0/1 号槽是一对冗余的"最左子页"引用（0 号槽持哨兵键、1 号槽持该子页最小键），
  因此一个 `capacity = C` 的内部页最多承载 `C - 1` 个真实子页。容量 4 时每页只剩 1~2 个
  真实子页，树会退化成很长的链（实测 600 条记录高度 90：**正确但很慢**）。
  容量 >= 8 时表现正常（本阶段所有验收都在 8/16/32 上）。
- `capacity = 4` 属于 `kBPlusMinCapacity` 的极端配置，建议只用于单测边界。
- **R 树的 upsert 是 O(叶页数)，不是 O(log n)**。R 树的路由键由 MBR 导出，
  "这条键在不在树里"**无法用 MBR 相交剪枝**（同一个键的旧记录完全可能落在另一个叶页里），
  所以 `RTree::Insert` 只能走叶链全扫去查同键旧条目。这是保证 I8（混合索引中两棵树
  指向同一份记录集）必须付的代价，也是"R 树插入比 B+ 慢一个量级"的直接原因
  （M6 的对照实验会量化它）。若将来需要更快，正确做法是让混合索引维护一个
  key → R 树叶页 的映射（`BPlusLeafEntry::rtree_leaf_page` 字段就是为此预留的，
  方案 §4.9），而不是把 upsert 降级成"叶内去重"。
- **单条记录大于半个页时无法分裂**：`SplitLeafAndReinsert` 会拒绝让两半都低于
  下溢阈值的分裂并明确报错（见缺陷 #32 的第二半）。变长条目的溢出页支持尚未实现。
- **M4-2 的边界：页分配/回收不进 WAL**（方案 D10）。`NewPage`/`FreePage` 不改页 LSN、
  不写 delta。直接后果是公开 API 里**存在一条真实的 I6 违反路径**（事务外 `NewPage`
  出来的页带 `page_lsn == kInvalidLsn`、脏、且终将落盘）—— 负向测试正是用这条真缝
  构造违例的。**不要**把它读成"WAL 已覆盖所有写"：M4-3 的 redo 会重放一个从未被
  分配过的页，那一步必须先补 alloc/free 日志。
- **`OnWalAppend()` 的计数口径混了两种粒度**：一条 `AppendPageDelta` 实际命中 **3 次**
  （`wal.cc` 一次 + REDO/UNDO 两条记录各经 `disk_manager.cc` 一次）。今天只是统计噪音，
  但**崩溃矩阵要判"哪条记录落了盘"时，口径不统一就没法判读** —— 所以它是 M4-3 的前置项，
  不是可选项。

---

## 对后续里程碑的影响

- **M2 门槛已通过**（`ltm_validate_bplus` 退出码 0），B+ 树可以作为后续里程碑的地基。
- **M3 门槛已通过**（`ltm_validate_rtree` 退出码 0），R 树可以提供空间查询与 KNN。
- **M5（混合索引）已完成**：`HybridIndex::Erase` 依赖的删除路径、两棵树的完整语义、
  以及**单事务双树原子提交**都已验收（`test_hybrid_diff` H 用例 + 崩溃矩阵 36 例）。
  方案 §4.9 的"叶页号定位"被实测推翻、改为"记录存在 B+ 侧"（缺陷 #41）——
  M6 做基准时**不要**再按"按键定位 R 树是 O(log n)"去写预期。
- **M6（并发）**：`concurrency/` 仍是占位。M5 留下一条直接相关的待办：
  `ScopedWriteOp::Abort` 不回滚内存页（见已知局限），
  这在单线程下只是"失败后需要一次恢复"，在多线程下会变成真问题。
- **M4（WAL/恢复）**：写前日志屏障已补齐，**M4-2 已把 WAL 接进真实写路径**并证明非空转；
  剩下 M4-3（LogFileHeader/checkpoint/Analysis-Redo-Undo + 恢复后不变量校验）。
  M5 的 `HybridIndex::Put` 可以直接复用 `BufferPool` 的**可重入**写事务：
  两棵树各自开事务时只有最外层真正提交，这正是"双树原子"需要的东西。
- **D3「通用」指标的措辞已收窄**（见 `docs/architecture.md` §3）：共享的是
  **结构维护半边**，查找半边各树自实现 —— 这不是没做到，而是单路径下钻对
  多维查找在语义上就不成立（会静默漏记录）。

---

## 已完成的里程碑

## M0 · 骨架与基础设施　**已完成**

- [x] **M0-1 目录与构建**：`CMakeLists.txt`（C++17、MSVC `/W4 /utf-8 /WX`、ASAN/TSAN/故障注入开关）
- [x] **M0-2 util 层**：`status`、`log`（变参 `operator()` 接住函数式宏实参）、
      `crc32c`（SSE4.2 可选 + 软件表 + 增量 Hasher）、`types`、`config`
- [x] **M0-3 分层检查**：`scripts/check_layering.py`（R1–R6），45 个源文件 0 违规
- [x] **M0-4 文档**：`docs/architecture.md`、`docs/invariants.md`、`docs/status.md`

## M1 · 存储内核　**基本完成**

- [x] `DiskManager`：页读写（越界补零）、WAL 追加/读取/截断、`IoStats` 计数
- [x] `BufferPool`：帧表、pin/unpin、CLOCK + 策略打分淘汰、缺页加载与校验、
      帧占用检查（防止两个页号共享同一帧）、脏页淘汰前刷盘、**校验和重算**、
      **空闲页复用复用同一帧**
- [x] **元数据页统一**：`storage/meta_page.h` 定义页 0 的唯一布局，
      `BufferPool` 是唯一读写者，上层走 `WriteTreeSlot()` / `EnsureMetaLoaded()` / `meta()`
- [x] `HotColdTracker` + `MigrationPolicy`：热度衰减/饱和、CLOCK 打分单调性
- [ ] 后台时钟线程（周期性 `DecayAll` + 主动下刷冷脏页）

## M2 · B+ 树页式实现　**已完成（门槛通过）**

- [x] `BPlusPageOps` 全部页内算子：二分定位、插入、删除、分裂、借键、合并、
      页内自检、父键不变量检查、叶载荷读取
- [x] 跨页算法：自根向下定位、递归插入 + 分裂传播 + 根分裂、
      递归删除 + 借键 + 合并 + 根降级、叶链范围扫描、全树不变量自检
- [x] 对外接口：`Open/Insert/Erase/Search/RangeScan/ScanAll/Validate/Destroy`
- [x] **差分测试 6/6 通过**（顺序插入+扫描、随机插入删除对拍、删空再插、区间边界、
      重复键覆盖、小缓冲池重开）—— 全部是真实断言，无留白
- [x] **不变量自检通过**：I1（父键 == 子页最小键）、I2（叶链双向 + 升序）、
      I3（非根页填充率 >= 容量一半）、I4（根到叶等长）
- [x] **M2 门槛：`ltm_validate_bplus` 退出码 0**（20000 次随机读写，capacity 32 / frames 128）

## M3 · R 树页式实现　**已完成（门槛通过）**

跨页算法**上提到 `src/index/tree_ops.cc` 的 `IndexTree` 只写一遍**（方案 D3 的落地），
B+ 与 R 都退化成「页算子 + 薄门面」（行数为 `wc -l` 实测）：
`bplus_tree.cc` 1781 → 1013 行，`tree_ops.cc` 17 → 1407 行，`rtree.cc` 新增 1957 行。

- [x] **M3-1 契约 v3**：`SplitInto` 不再返回分隔键（双真相陷阱）、`BorrowFrom` 四种情形
      全部下沉进页算子、补齐路由/编码/定位挂钩；新挂钩只加在 `TreePageOps`，`TreeOps` 一字不改
- [x] **M3-2 `IndexTree` 通用引擎**：插入下钻 / 分裂传播 / 借键合并 / 下溢循环 /
      根升降 / 元数据 / `Validate` 骨架 / `Destroy` 全部搬入，B+ 行为逐位不变（见下）
- [x] **M3-3 `RTreePageOps`**：`lo[dim]|hi[dim]` 布局（MBR 只存一份、Z 码由 MBR 现算不落盘）、
      维度校验（`kDimensionMismatch`）、三种分裂策略（even/linear/quadratic）、
      借键合并、页内自检
- [x] **M3-4 `RTree` 门面**：`Search/Erase/ScanAll`（叶链）、`SpatialQuery`（相交/包含）、
      `Knn` best-first + `KnnBruteForce` 对拍、剪枝率统计
- [x] **M3-5 差分测试 9/9**（`test_rtree_diff.cc`）：随机增删对拍、空间查询对拍暴力扫描、
      KNN 对拍暴力 + 真值 oracle、删空再插、小缓冲池重开，含 3 个回归钉子
- [x] **M3 门槛：`ltm_validate_rtree` 退出码 0** —— I9（MBR **精确相等**，不是"包含"）、
      I3（非根页填充率 >= 40%）、I4、I2 叶链、KNN 与暴力逐元素等价（1e-9）
- [x] **M3 门槛的另一半：ASAN 全绿** —— 收尾时才发现此前**从未真正执行**（缺陷 #37，开关在
      MSVC 下静默失效）。修好开关后补跑：`build-asan/` 下 `ctest -E validate` 9/9（M4-2 加入
      `test_wal_barrier` 后重跑为 10/10）、两个守门
      程序均退出码 0。**注意口径**：Windows 上 MSVC ASAN 无 LeakSanitizer，故覆盖的是内存
      **错误**（越界/UAF/野指针）而非**泄漏**
- [x] **三策略对照证据**：`report/rtree_split_policy.csv`（由验收程序直接写出，非手工誊抄）
- [x] **验收程序不再是空跑**：旧版 `test_rtree_validate.cc` 用 `Open(nullptr, nullptr, ...)`
      且一条记录都不插 —— 空树上 I9/I3/KNN 等价全部平凡成立，退出码 2 也只是"未实现"。
      现为真缓冲池 + 真页算子 + 8000 次随机增删 + 真值表逐条比对 + 非平凡性护栏

## M4-2 · 写屏障接入　**已完成（门槛通过）**

把早已真实存在的 `WalWriter` 接到真实写路径上，并**证明它非空转**。证据全部在 `report/m4_2/`。

- [x] **阶段 0 快照**：`_bak_m4/`（本项目无版本控制，改动前先留底）
- [x] **阶段 1 · D4 + D5**（先于接线，改既有代码）：`Fetch` 不再无条件写 `tree_id`
      （原来**每次读都在白写 4KB 回盘**）；`PersistMeta` 不再无条件立即刷页 0；`LoadMeta`
      不再对页 0 调 `Init()`（那会把 `page_lsn` 清零，使最危险的页永远豁免断言）。
      before/after 两组数字表在 `report/m4_2/*_d4.txt`，漂移逐项解释在 `d4_d5_drift.txt`
- [x] **阶段 2 · D1/D2/D3/D6/D7(计数模式)/D8**：事务归 `BufferPool`（M5 的双树单事务要求它
      是唯一能看见全部帧的对象）；delta 采集挂在 `Unpin`（**不是 commit** —— 攒到 commit
      再写会让页在 commit 前就被淘汰，I6 每次操作违反上千次）；`before == after` 跳过；
      `StampPageLsn` 一律标脏（漏了会让 LSN 落在干净帧上被静默丢弃）；跨运行 LSN 单调
- [x] **阶段 3 · 断言打开 + 两个证伪性实验**：负向测试**走真缝**（事务外 `NewPage`），
      不用测试专用开关、不用 mock（`FaultInjector` 在两个构建目录里都是 OFF，用它等于自欺）；
      实验①抽掉一个采集点 → 用例 C 双通道变红；实验②把 strict 翻成 false → 用例 A 7 处断言变红。
      两次实验均**逐字节还原**（`cmp -s` + sha256 一致 + 0 个实验标记残留）
- [x] **阶段 4 · D9 + `build-dbg`**：`PageGuard::page()` 返回 `const Page`，编译期挡住
      **3 处真实误写**（`guard.page().UpdateChecksum()`），负向对照产生 C2662。
      Debug 下断言**确实在场**（单变量实验：同一个 `assert(false)` 探针，Debug 退出码 3
      并打印 file:line，RelWithDebInfo 退出码 0），全量测试 10/10 且无一条触发
- [x] **阶段 5 · 1 小时压测**：`scripts/soak_m4_2.sh`（Debug 构建、断言在场、
      **每轮开跑前删日志**、**私有工作目录**、每轮超时）。**实测 222 轮 / 0 失败 / 退出码 0**，
      覆盖 2 040 000 次操作、追加 4 994 290 条 delta（约 41 GB WAL，逐轮删）。
      逐轮原始证据 `report/m4_2/soak_progress.txt`（222 行），结论 `soak_summary.txt`，
      失败文件为空。**驱动本身也做了证伪**：注入同一个单变量缺陷后重跑，6 轮里 3 个 B+ 轮
      变红、3 个 R 树轮仍绿（原因见下），证据 `phase5_soak_falsification.txt`
- [x] **M4-2 门槛：Debug 断言开启下压测无触发**，且三条"零容忍"采集计数
      （`outside-txn` / `missing-before` / `unstamped-writes`）全程为 0。
      注意**不含** `identical-skipped` —— 它统计的是"标了脏但字节没变"（D3 的优化），
      非零是优化在起作用，把它也当失败判据会让压测恒红

**压测报告见 `report/m4_2/phase5_soak.txt`**（另有 `phase5_soak_falsification.txt`）。
其中三条必须知道的限制：① R 树的压测轮**不能**独立证明 `IndexTree::Insert` 的事务括号 ——
`RTree::Insert` 自己开 `ScopedWriteOp` 罩住了内层调用，内层采集点缺失被外层掩盖（证伪实验里
恰好只有 B+ 轮变红）；② 压测**不覆盖崩溃恢复**，它从不强杀进程，那要等 M4-3 的崩溃矩阵；
③ 300 s 的每轮超时对中位数（12 s）很宽裕，但对实测最慢的那一轮（160 s，复跑 21 s 证实是
系统被 205 MB 日志拖慢、不是负载性质）只剩 1.9x，它是兜底不是紧界。

**这一项的核心是"证明它非空转"**（本项目刚因缺陷 #37 把一条从未执行的 ASAN 门槛记成"通过"）。
因此判据不是"跑完了没报警"，而是：`log/*.wal` 出现真实非零日志、负向测试能证明**断言会报错**、
抽掉一个采集点能证明**缺口会被抓住** —— 三条缺一不可，见 `report/m4_2/phase3_falsifiability.txt`。

**两条被实测推翻的方案前提**（详见 `report/m4_2/` 与方案附录 C8）：

1. 方案的性能第 1 条「只记变化区间 → 20–50x」**实测只有 1.2x/1.3x**，因为槽目录向下增长、
   条目区向上增长，一次插入在页的两端都改字节。**不做**，且 M4-3 不要再按 20–50x 规划体积。
2. 方案的 `flush_page` 断言写法「先自动 Sync 再断言 `page_lsn <= flushed_lsn`」**对真 bug 全盲**：
   `Sync()` 把 `flushed_lsn` 推到本运行最大值，该断言在本运行内恒真。已拆成"写前规则（计数）"
   与"可证伪断言（拒绝 + 污染池）"两条。

---

## M4-3 · 崩溃恢复（日志头 / 检查点 / Analysis-Redo-Undo）　**已完成（门槛通过）**

方案 §6 的 M4-3 是「LogFileHeader + checkpoint + Analysis/Redo/Undo」，
验收门槛是「`report/recovery_matrix.md` 生成且无失败项」。全部落地，证据在 `report/m4_3/`。

- [x] **日志头**（`wal.{h,cc}`）：512 字节保留区 + `WalLogHeader`（magic / format_version /
      header_bytes / `last_checkpoint_lsn` / `clean_shutdown` / crc32c）。头部自带校验和是硬要求：
      `last_checkpoint_lsn` 被写坏会让恢复从错误位置开始，而"少重放一段日志"是静默的。
      **非空但没有 magic 的文件一律拒绝**（不猜起点）。
- [x] **记录扫描器**（`ForEachWalRecord`）：自动跳过保留区；逐条做记录级 crc 校验，
      长度不合法或校验和不符即停 —— 后者就是"追加写到一半被强杀"的形态。
- [x] **CLR 记录**（`kClrRedo`，只有 REDO 半边）：让被回滚的页获得一个比原更新记录更大的
      `page_lsn`，否则**第二次恢复会把未提交事务的 after-image 重新放回去**，
      而且此后没有任何东西能再回滚它（事务已经写了 `kAbort`）。用例 C 专门钉这一点。
- [x] **Analysis**：`dirty_page_table` + `txn_table` + `undo_first{(txn,pid)}`；
      遇 `kCheckpointEnd` 清空脏页表并把 Redo 起点前移。
- [x] **Redo**：只碰脏页表里的页；顺序固定为**写区间 → 置 `page_lsn` → 重算校验和**
      （CRC 覆盖 `page_lsn`，顺序反了会让每个被重放的页在下次打开时判成校验和不符）；
      **损坏页不走幂等判断**（自描述不一致或校验和不符一律强制重放，否则损坏会被永久跳过）。
- [x] **Undo**：对 loser 事务按 (事务,页) 取**最早**一条 UNDO 的 before-image 写回 + 写 CLR；
      回滚完写 `kAbort`，下次恢复不再重复回滚。
- [x] **检查点**：`begin（记偏移）→ pool->FlushAll() → end + fsync → 更新头部 + 截断`。
      截断不能直接 `TruncateWal(keep_from)`（那会把保留区搬到偏移 0、冲掉头部），
      顺序是「读出保留字节 → `TruncateWal(WalSize())` 清空 → 重写头部 → 追加保留区」。
- [x] **干净关闭标记**：`BufferPool` 析构时先 `FlushAll()` 再标 `clean_shutdown`（顺序不可换）；
      有未提交写事务或池被污染时**故意不标**，让下次打开去做 Undo。
- [x] **I6 的豁免口径定死**：唯一豁免 = "日志里一条记录都没有"（建库第一步）；
      M4-2 的 `enforce_i6=false` 逃逸口已删除（"数据文件是新建的"与"这页没有 REDO 覆盖"并不等价）。
- [x] **崩溃矩阵**：`tests/crash/crash_test.cc`（三个相位：work / corrupt / verify）
      + `scripts/crash_matrix.py`（Python 用 Unicode 传路径，绕开非 ASCII 路径的命令行编码往返）。
      **42 例全过，其中 36 例真的崩了**（`_exit(9)`，不跑析构、不刷盘）。
- [x] **M4-3 门槛：`report/recovery_matrix.md` 无失败项** —— 判据是"恢复后全量遍历逐条等于
      旁路日志记录的已提交集合 + `Validate()` 全绿"，而不是"跑完了没报错"。
- [x] **顺带补上方案 §5.3 要求的 I10 证据**：`test_recovery.G` 把矩阵的离散反例扩成
      **152 个确定性日志前缀**（每条记录边界 + 每条记录中间），逐个恢复并校验，全绿。

**M4-3 定位并修掉的缺陷**（详见下面缺陷表 #38–#40）：

1. **#38 UNDO/REDO 的记录顺序**：REDO 在前时，崩溃落在两条记录中间会留下
   "有 after-image、没有 before-image"的页 —— Redo 把未提交的插入重放上去，
   而 Undo 按 UNDO 记录定位待回滚页，根本不知道要回滚它。**由崩溃矩阵发现**
   （`wal_append n=40 / random`：页里 6 条、元数据说 5 条），修法是把 UNDO 写在 REDO 之前。
2. **#39 "不可达崩溃现场"会制造假失败**：最初的前缀用例直接把"跑完让池析构刷盘"的页文件
   配上任意截断的日志，那等于"数据文件比日志更超前"—— 而写前日志规则保证真实崩溃
   永远不会这样。测试于是测的是"恢复能不能修复一个不可能发生的世界"。
   改为**以检查点那一刻的页文件快照为基准**，每个前缀才对应一个可达的崩溃现场。
3. **#40 检查点缺少"写事务在途"的拒绝**：截断会把在途事务的 UNDO 一起截掉。

---

## M5 · 混合索引与对外接口　**已完成（门槛通过）**

方案 §6 的 M5 是「`IndexEntry` 序列化 / `HybridIndex` / `IndexRegistry` / 持久化端到端」，
验收是「I8 成立；关闭后重开全量数据可查」。四条全部落地，证据在 `report/m5/`。

- [x] **M5-1 `IndexEntry` 序列化**：`PayloadCodec` 往返属性测试（M0 就有）+ M5 新增
      混合索引私有的 B+ 侧记录编码（`dim | payload_len | lo[dim] | hi[dim] | payload`）。
- [x] **M5-2 `HybridIndex`**：`Open/Close/Put/Erase/Get/Query/Scan/Knn/Checkpoint/Validate/stats`；
      **一个写事务罩住两棵树** —— `ScopedWriteOp` 可重入，Put 内部三层（外层 + R 树的 upsert
      括号 + 两棵树的 `IndexTree::Insert`）只提交一次。
- [x] **M5-3 `IndexRegistry`**：`HybridIndex::Open` 经注册表按名字建树（`bplus` / `rtree`），
      存储层零改动；`mock` 变体仍在，通用性活样例未退化。
- [x] **M5-4 持久化端到端**：关闭后重开全量可查（用例 F），检查点前后查询结果一致（用例 G），
      空间查询在重开后依然可用。
- [x] **M5 门槛：I8 成立** —— `Validate()` 做**双向**交叉校验（B+ 里有的 R 必须有，反之亦然，
      且同一条记录的 MBR 与载荷必须一致），并且这条检查本身**可证伪**（用例 I 故意让两棵树
      对同一条记录说不同的话，检查报出具体键号）。
- [x] **崩溃矩阵扩到混合索引**：`crash_test` 新增 `--engine=hybrid`，用**同一条判据**
      （恢复后全量遍历 == 旁路日志）覆盖双树原子性。矩阵从 42 例扩到 **78 例，全过**，
      其中 36 例跑混合索引。

**M5 定位并修掉的设计问题**（详见下面缺陷表 #41–#43）：

1. **#41 方案 §4.9 的"B+ 叶条目只存 R 树叶页号"不成立**。实测：R 树的**分裂会把一半条目
   搬到新页**，于是插入时记下的叶页号在批量装载后就大面积失效 —— 2000 条纯插入之后提示
   命中率只有 50%（`report/m5/demo.txt`：1001 命中 / 999 失效）。失效一次就要做一次
   O(R 树叶页数) 的叶链全扫，Get/Erase 因此有一半概率退化成全扫。
   **修法**：把**整条记录（键 + MBR + 载荷）放在 B+ 侧**。Get 一次 B+ 定位即可（不访问 R 树，
   用例 E 用"定位计数纹丝不动"证明），Erase 从 B+ 记录里拿到精确 MBR 再去删 R 树；
   R 树只回答空间问题。叶页号降级为**纯提示**，只给 I8 交叉校验用（有效时 O(1)，
   失效时退化为全扫但结果仍然正确）。代价：B+ 侧载荷多 `4+16*dim` 字节。
2. **#42 `Validate` 的 I8 报告说不清是哪一条不一致**：原来回调返回 false 就停扫，
   于是"少了一条"和"两条对不上"都会退化成"条目数不一致"。现在分别记录
   `missing` / `disagree` / 首个不一致的键号，报错信息里带键号与原因。
3. **#43 崩溃矩阵的 B+ 引擎漏调 `Recover()`**（重构出 `Engine` 抽象时引入）：
   恢复没跑，索引直接读崩溃现场，被 I6 检查当场抓住（page 0 全零而日志非空）。
   这类"重构时漏掉一个必要步骤"的问题，是靠**同一条判据覆盖两个引擎**暴露的 ——
   如果两个引擎各写一套判据，漏掉的那一边不会被另一边发现。

---

## M6-1 · C0 全局 latch（多读单写）　**已落地；验收判据未达成，且判据本身不成立（见下）**
**（历史记录：C0 已被 C1 取代，索引层已不再取它 —— 见「C1 本体 · 已落地（含读者侧 crabbing）」一节）**

方案 §6 的 C0 是"全局 latch：读共享 / 写独占"。实现已落地：闩归 `BufferPool`
（帧是它拥有的，只有它能保证"一次索引操作期间帧不被换掉"），索引层在公开入口加锁 ——
`BPlusTree`/`RTree` 的 `Search`/`RangeScan`/`ScanAll`/`SpatialQuery`/`Knn` 取共享，
`Insert`/`Erase` 取独占。守卫 `C0LatchGuard`（`concurrency/latch_manager.h`）在
线程内做重入计数，因为 `HybridIndex::Put` 会嵌套调用两棵树的入口，而
`shared_mutex` 不可重入 —— 不处理就是自死锁。

### 测量结果：8 读线程约 **1.3–1.6x**，远低于方案写的 ≥ 4x

证据：`report/m6_1/c0_read_scaling.csv`（20000 键 / 池 1024 帧 / 工作集 647 页全部驻留 /
每轮总操作数相同，只有线程数不同）。下表是其中一轮：

| 线程 | 查询数 | 秒 | ops/s | 扩展比 |
| --- | --- | --- | --- | --- |
| 1 | 200,000 | 0.142 | 1,412,703 | 1.00x |
| 2 | 400,000 | 0.240 | 1,668,825 | 1.18x |
| 4 | 800,000 | 0.399 | 2,003,858 | 1.42x |
| 8 | 1,600,000 | 0.902 | 1,773,035 | **1.26x** |

**逐轮波动不小**：重复跑四次，8 线程的扩展比分别是 1.26 / 1.30 / 1.53 / 1.56，
而 4 线程在 1.42–1.74 之间。所以可信的说法是"8 线程约 1.3–1.6x、且 8 线程不比 4 线程好"，
而不是某个精确值 —— 单次运行的数字在这里**不足以**支撑"调优后能到 4x"这种判断。

**这不是机器不行，也不是 latch 把读串行化了**（串行化会给出恒定的 1.00x，而这里到过 1.42x）。
对照实验（同机、同负载、同代码，唯一区别是"每个线程用**自己的**池与树"）：

```
control (independent pools, no shared state): 1 thread≈1.05-1.40M ops/s
                                              8 threads≈8.3-9.6M ops/s -> 6.86x / 8.36x / 7.95x
```
（三轮实测，逐轮有波动。用例的判据写成 `>= 3.0x`，不拿单次数值当门槛。）

**零共享状态时 8 线程稳定拿到约 7–8x（8 核效率 85%+）**，所以瓶颈是共享缓冲池自身的
可变状态。本机 24 核（Intel Core Ultra 9 275HX），核数不是约束。

### 为什么 C0 达不到 ≥4x —— 每次搜索的共享缓存行写次数

一次点查要 `FetchPage` 3 次（根 → 内部 → 叶），每次都会：

1. `++fr.pin_count`（原子 RMW）—— 其中**根页与全部内部页的帧被所有读线程共用**
   （20000 键 / capacity 64 时是 1 个根 + 约 5 个内部页），这几条缓存行必然乒乓
2. `++im.stats.hits`（原子 RMW）—— **同一个计数器被所有线程打**
3. 取/放一次 `C0LatchGuard`（MSVC `shared_mutex` 底层 SRWLOCK 的原子计数）

即每次搜索约 7 次**命中同几条缓存行**的原子 RMW。单线程 ~700ns/次，而跨核搬运一条
缓存行是几十 ns 量级 —— 数量级上正好把扩展比压在 1.3x 附近。

**上面三条里哪一条是主要矛盾，还没有单独排序**（要排序得做"把 `stats.hits` 改成分片/
按线程累加后重测"这类对照，本轮没做）。已确定的是瓶颈**在共享缓冲池的可变状态**这一类里，
不在机器、不在 latch 的互斥语义、也不在树的算法。

**该用例的运行时间**：`test_c0_latch` 单独约 140 秒（其中对照实验 ~100 秒，
而对照的装载阶段是 WAL 组提交主导的 —— `group_commit_ms=2`，每条插入的提交要等满这个
窗口）。装载已改为 8 个池并行（289 秒 → 101 秒）。要再快就调小 `kKeys`，
但 20000 键对应的 647 页工作集是这个实验想覆盖的量级，不建议随便减。

**结论：≥4x 不是"再多跑一会儿/调优一下"就能到的，它在 C0 的结构下不可达。**
要达到它必须先消掉上面三处热点：计数器分片或按线程累加（1、2），以及
**不再为每次搜索重复钉住根/内部页**（3）—— 后者正是 C1（页级 latch coupling）的内容。
因此方案里 M6-1 的这条判据应当改写为"C0 正确性判据（多读单写不发生撕裂、测量期零缺页）"，
把扩展比留给 C1/C2 去比。**这一点已按原样记录，未做粉饰。**

### 已验证的部分（3 条新用例，`tests/concurrency/test_c0_latch.cc`）

- `C0.ReadScaling`：并发读结果与单线程装载逐条一致（mismatches=0）；**测量期零缺页**
  （断言而非记一笔 —— 缺页/淘汰路径要改帧表与空闲链，而共享闩保护不到它，
  跑出非 0 就是踩到了 C0 的定义边界，属报错不属记数）。这条边界写在
  `buffer_pool.h` 的 `c0_latch()` 注释里，不是暗坑。
- `C0.ConcurrentWritersAreSerialized`：4 写线程 × 500 插入 ⇒ 2000 条、`Validate()` 通过、
  逐条点查正确。**这条顺带验证了 `BufferPool::Impl::write_op`（普通成员、不是
  thread_local 的事务深度计数）在独占闩下确实一次只被一个线程碰。**
- `C0.LatchGuardIsReentrant`：同模式嵌套只最外层真加锁；独占下再要任意模式不死锁；
  多线程能同时持有共享闩（观测到 8 个并发持有者）。

---

## C1 前置项 · 写事务 Abort 的运行期回滚　**已完成**
**（不占 M6 的编号：方案里 `M6-2` = C1 页级 latch coupling。本节是它的前置条件 ——
C1 本体已于 2026-10-04 落地，见「C1 本体 · 已落地（含读者侧 crabbing）」一节）**

M6-1 一节末尾点名了一条"必须在 C1 之前修"的待办：`ScopedWriteOp::Abort` 只写一条
`kAbort` 记录、**不回滚内存**。本轮把它修完了，因为 C1 一旦放开写并行，
`HybridIndex::Put`（先写 R 树、再写 B+ 树）的第二步失败就会从"够不到"变成常态路径。

### 这条缺陷比原来记的还重一档

原来的记录是"运行期内存不一致，崩溃恢复是对的"。**实测发现后半句救不了它**：
abort 之后 `FlushAll` + 重开，幻影记录是真的躺在页文件里的（`entries=600` 而基线
只有 400）。原因是**干净关闭不跑恢复** —— 日志里那条 `kAbort` 永远不会被任何人读到。

### 页的 before-image 不够 —— 三类状态，只有第一类在页里

1. **页帧**：重放 before-image。快照点在"事务内首次钉住时"，正是事务开始态。
2. **分配器**：`free_list_head` / `free_pages` / `next_unallocated` 住在 `Impl` 上，
   不在任何页里，`NewPage`/`FreePage` 在写事务里改它们。不回退会让空闲链与分配游标
   停在已中止的事务里（不变量 I7）。
   **事务里新建的页是特例**：它们没有可用的 before-image（`NewPage` 刻意在 `Init()`
   之前快照，否则 before == after、新页会带着 `kInvalidLsn` 落盘），重放它等于把
   一帧遗留垃圾当回滚目标。正确做法是"这页从未存在"——丢脏位、页号还给分配器。
   *这一条不是推出来的*：先按"还原"写，实测就撞上 I6 并把池毒化了。
3. **索引层派生状态**：`IndexTree::Impl` 的 `root` / `leftmost_leaf` / `height` /
   `num_entries`。它们**不在任何页里** —— 页 0 的树槽位只在 Open/Close/Checkpoint
   时同步，不是每次插入都写。所以缓冲池的 before-image 再完整也够不着它们，
   不回滚的报错是 `Validate: total entries in pages 400 != metadata count 600`。

第 3 类决定了这个修复**出不了缓冲池**。缓冲池因此新增一个很小的**参与者协议**：
`write_op_generation()` + `RegisterWriteUndo()` —— 树在**加入**事务时（改动任何东西
之前）把事务开始态注册进来，池在 Abort 时按后进先出执行。注册时刻的正确性靠
"注册必须发生在改动之前"这条纪律，它写在 `IndexTree::Impl::RegisterRollback()` 的
注释里，调用点就在 `Insert`/`Erase`/`Destroy` 的 `ScopedWriteOp` 之后一行。

### I6 交互 —— 看着像坑，其实不是

还原本就是**整页**还原（含 32B 页头），所以 `page_lsn` 一并回到事务前的值。这是
有意的：`FlushFrame` 的判据是 `page_lsn > flushed_lsn ⇒ 先 Sync()`，page_lsn 变小
只会让这个条件**更容易**满足，回滚不可能制造出新的 I6 违反。反过来（只还载荷、
留 LSN）才是坑：内容回到事务前，页头却声称自己被一条已中止事务的 REDO 覆盖着。

**不写 CLR 是对的**：运行期 Abort 没有"已经重放到盘上的更新"（改动都还在内存里）。
中止事务的 REDO 记录配上 `kAbort`，恢复时走 Undo 分支，与内存回滚的结果一致。
CLR 是给**恢复期**的 Undo 用的，不该在运行期这套里再实现一遍。

`AbortWriteOp` 内部**先回滚内存、再写 `kAbort`**：反过来的话有一个窗口，日志已经
certify"这个事务没了"而内存还带着它。C0 的独占闩关着这个窗口，C1 就关不住了。

### 可证伪性（`report/abort_rollback/`）

单变量实验：把 `AbortWriteOp` 里的 `im.RollbackWriteOp(this)` 换成 `Status::Ok()`，
其余一律不动 ⇒ 4 条用例 **1 过 3 红**（幻影键 200、重开仍是 200、分配游标 14→20
不回头），`CommitStillApplies` 保持绿（说明实验只动了 abort 这条轴）。红输出逐字留档
`report/abort_rollback/abort_undo_red.txt`；还原后 `cmp -s` 逐字节一致、sha256
`57a701e5…bbb01`、源码里 0 个实验标记。

四条判据各自都不是"数一个计数器"：
- `RollsBackMemoryPages` 先断言事务**确实改到了内存**（否则"查不到"可以是插入根本没生效
  这种假绿），再断言 abort 后查不到，并且**基线一条不少**（全回滚不是回滚）。
  它还断言 `Validate()` 通过 —— 这正是这条缺陷危险的地方：被中止的树**结构完好**，
  I1..I9 一条不响，只有"与事务前状态比对"抓得住。
- `AbortedStateDoesNotSurviveReopen` 从页文件重开，内存里修好骗不过它。
- `RollsBackAllocatorState` 先断言事务期间游标**确实动过**，再断言它回来了。
- `CommitStillApplies` 反向对照，过度回滚会被它抓住。

### 仍未证明的

- ~~**C1 本体一行没写。** 本轮只清了它的前置条件。C0 的读扩展判据依然不可达（M6-1）。~~
  —— **已过时：C1 本体已于 2026-10-04 落地**（见「C1 本体 · 已落地（含读者侧 crabbing）」一节）；
  C0 的读扩展判据仍不可达，但 C0 已被撤掉，那条判据随 C0 一起作废。
- **无并发**。四条用例都是单线程，abort 路径跑在独占闩的保护下。回滚的"两趟"顺序
  （先池内页、再事务中被换出的页）正是为了不刷出未回滚的脏帧 —— 那条推理假设没有
  别人在写。**C1 已放开写并行（单写者 + 页闩），但这条推理没有在并发下重新测过**
  （30 分钟压测的 abort 只出现在"删不存在的键"上，那条路径不改页，故覆盖不到
  "部分修改后 abort" 与并发读者的交叉）。**这是 C1 之后仍未验证的一条**。
- **`HybridIndex::Put` 的失败路径没有被端到端驱动**。它是这条缺陷的动机，但要让它
  失败需要一个在所有构建目录里都被编译关闭的故障点；把负向测试走 `FaultInjector`
  会空过（缺陷 #37），所以本轮用的是"在公开 API 上手动开事务再 Abort"，比换一种
  空过的方式诚实。

---

## C1 前置件（二）· 页闩与 LockOrderChecker　**已完成（C1 本体已于 2026-10-04 落地，见「C1 本体」一节）**
**（同样不占 M6 的编号：`M6-2` = C1 页级 latch coupling，本节是它的前置件）**

C1 落地之前，`LatchManager::Acquire` 返回的是一个**不持锁**的守卫：
`latch_manager.cc` 里原话是 `// M6：这里做真实加锁；骨架返回一个不持锁的守卫`，
`Release()` 与 `CheckOrder()` 都是空函数。也就是说"页闩"这个名字在代码里成立了很久，
**一把锁都没加过，顺序检查器一次都没跑过** —— 正是缺陷 #37 那个形状。本轮把它补上。

### 落地了什么

* **真的按页闩**：每页一把 `std::shared_mutex`，按页号分 **64 路元锁**保护表。
  分的是**元锁**不是页锁（按页号分页锁只会制造假冲突：两个无关页落进同一片，
  一个要写就把另一个的读挡住）。元锁只在查找/建项时拿，拿到页锁地址立刻放掉，
  **绝不持着元锁去阻塞页锁** —— 那会把元锁变成全局串行点，等于把 C1 退回 C0。
  表的值是 `unique_ptr<shared_mutex>`：`unordered_map` 节点 rehash 时不搬是**实现细节**，
  而"某线程正阻塞在这把锁上、锁的地址却被搬走"是 UB，用指针把地址钉死。
* **`LockOrderChecker` 三条判据**（全在 acquire 侧，因为死锁只由获取顺序决定）：
  ① level 单调不增（拿低层页再去拿高层页 = 向上加锁）；② 同层 pid 不降（兄弟恒先左后右）；
  ③ 不重复获取 / 不升级。检出即计数 + 打印完整持锁链，DEBUG 下另加 `abort()`。
* **争用计数是真的**：先 `try_lock`，失败才计数再阻塞，不是"想过要等"。

### 实现中发现方案 §6 有两处不成立（不是顺手改的）

1. **判据不能用 pid 正序。** 方案写"发现 pid 逆序即 abort"，但 **pid 是分配序、不是树序**：
   一次分裂里新分配的右兄弟会从空闲链上拿到更小的 pid，于是"正确的先左后右"在 pid 上
   是递减的 —— 拿 pid 当序会把正确实现判成违规，而真正的逆序（拿着叶再去拿根）反而可能
   看起来递增。改以 **level** 为权威判据，pid 只作同层兄弟的辅助见证。**已知边界**：
   这条辅助判据只在两个同层页被同时持有时判定，且不验证是否同父；两个不同父的同层页
   被同时持有不是任何合法树操作，故在本项目形态下够用 —— 但它是"够用"，不是"完备"。
2. **「释放顺序必须与获取顺序相反」是错的。** 同一句话还写着"下钻时先拿子页再放父页"，
   两句不能同时成立，而第二句恰好禁止了 latch crabbing 本身：
   `拿父 P(L3) → 拿子 C(L0) → 放父 P`，放 P 时 C 在栈顶、**P 不是栈顶**。反过来，
   最朴素的"锁-读-放-再锁下一层"的释放顺序是**递增**的。两种都合法，释放侧不存在
   统一的顺序约束。最初按原文实现了"释放必须卡栈顶"，**是测试日志抓出来的**：
   合法的 crabbing 序列被判成违规。现在释放侧只保留"确实持有"一条。
   另外第一版把持锁栈写成"每线程一个槽、换管理器就重置"，也是被测试日志抓出来的 ——
   同一线程上第二个管理器一出现就把第一个的栈抹掉，它的闩再也解不开，锁永久泄漏。
   现在按管理器指针分栈。**同一个隐患在 C0 的 `g_c0` 里仍然存在**（换池重置计数），
   本轮**没有**改：C1 落地后 C0 会被撤掉，且当前结构下一次索引操作只碰一个池，触发不了。

### 判据必须能变红（两次单变量证伪，逐字节还原）

| 实验 | 单变量 | 结果 |
| --- | --- | --- |
| A | `Acquire` 返回不持锁的守卫（= 原骨架的确切行为） | **3 passed, 5 failed**；`ExclusiveReallyExcludesShared` 报 `violations=550` |
| B | `CheckOrder` 开头直接 `return`（= 原空函数的确切行为） | **5 passed, 3 failed**，且恰好是需要"检出违规"的那 3 条；3 条负向对照保持绿 |

两次都按字节还原（`latch_manager.cc` sha256 `46f5159d…b56739`，`FALSIFICATION EXPERIMENT`
标记 0 个，还原后 8 passed / 0 failed）。逐条输出：`report/c1/red_no_locking.txt`、
`report/c1/red_no_order_check.txt`、`report/c1/green_page_latch.txt`。

### 仍未证明

* ~~**C1 本体一行没写。** 索引层 11 个入口（`bplus_tree.cc:913,924,941,965,1029`、
  `rtree.cc:1486,1655,1672,1739,1815,2048`）**仍全部走 C0**，本节的新机器还没被用上。~~
  —— **已过时：C1 本体已于 2026-10-04 落地，11 个入口的 C0 已全部撤掉**
  （见「C1 本体 · 已落地（含读者侧 crabbing）」一节）。
* ~~**帧表边界未解决**（缺陷 #46），**不解决它就不能撤 C0**。~~ ——
  **已解决（见「C1 前置件（四）」）**：帧表进临界区 + `Search` 返回自持载荷，
  两条判据 5/5 红→5/5 绿。撤 C0 的前置条件已满足。
* **30 分钟压测的驱动已经写好了，但 30 分钟本身没跑过**（见下一节「C1 前置件（三）」）；
  **ASAN 也没跑过**。本节的用例是秒级的。
* **跨线程锁序环抓不到**：检查器是每线程的持锁栈，两个线程各自合法、合起来成环它看不见。
  那正是 30 分钟压测要覆盖的；MSVC 无 TSAN，这是本项目在竞争检测上的已知缺口 ——
  结题报告要写进局限，**不许记成"TSAN 干净"**。
* ~~**帧表改造有一个已识别、动手前必须先定的坑**：缓冲池加载页若去拿"页闩"，会得到
  `frame_table → page(子)`；而索引层 crabbing 是 `page(父) → frame_table → page(子)`，
  两者**成环**。绕开方式是给加载步骤单独一把 load-lock（或"加载占位 + 条件变量"），
  让全局顺序保持 `页闩(自根向下) → frame_table → 该页的 load-lock`。~~
  —— **已按这条锁序落地，但不需要单独的 load-lock**（见「C1 前置件（四）」）：
  那个"成环"的担心建立在"缓冲池加载步骤会去拿页闩"的前提上，而本实现里页闩只由
  索引层持有，缓冲池**从不**取页闩，于是 `页闩 → frame_table` 是单向的，天然不成环。

---

## C1 前置件（三）· 30 分钟压测驱动　**驱动已完成并留下红/绿两侧证据；30 分钟已于 C1 落地后跑过（见「C1 本体」一节）**

C1 的验收有三件：`LockOrderChecker` 零逆序、ASAN 干净、**混合读写 30 分钟无死锁无挂起**。
第三件此前连工具都不存在（前两件见上一节）。本轮补上：`tools/soak_mixed.cc`，
同样**故意不注册进 `LTM_TEST_SOURCES`**（30 分钟量级的工具不是单元用例）。

### 它一个命令两个身份

结果**取决于池子相对工作集的大小**，驱动会自己打印 `REGIME` 行把这一次属于哪种写出来：

| regime | 条件 | 结果 | 含义 |
| --- | --- | --- | --- |
| `resident` | `frames >= 树的页数` | 绿 | **只证明驱动工作 + C0 命中路径串行化成立**；它看不到 C1 要改的代码 |
| `evicting` | `frames < 树的页数` | **红 → 绿（C1 落地后已复验）** | 修复前踩到缺陷 #46；帧表锁落地后 5/5 `clean`；**C1 撤 C0 之后又按本节命令跑过 30 分钟，见 `report/c1/soak_c1_30min.txt`** |

所以它既是**帧表改造的第二条现成判据**（第一条 `c0_boundary_probe` 只做点查，
这条覆盖区间扫 + 写路径），又是 C1 之后的压测主体。`REGIME` 行的存在就是为了
**防止有人拿 resident 的绿去交 evicting 的差**。

### 证据（`report/c1/`）

* `soak_green_resident.txt` —— `soak_mixed.exe 20 4 0 20000 4096 5 5`，
  564 页的树 / 4096 帧，`SOAK_VERDICT: clean`，并打印
  `CAVEAT: resident regime -- this run did not exercise the frame-table path`。
* `soak_control_single_thread.txt` —— **单变量对照**：`20 1 0 20000 64 6 5`，
  同一个 64 帧的小池子、同一个 `evicting` regime、**1153388 次淘汰**、
  `read_wrong_payload=0 validate=ok`，**绿**。
* `soak_red_evicting.txt` —— `20 4 2 20000 64 6 5`，同一池子只把并发加回来：

```
REGIME: evicting (frames < tree pages -> miss/evict path is exercised)
[ERROR][buffer_pool.cc:1193] PageGuard: Unpin failed on release: kCorruption: Unpin count exceeds the pin count: 379
call_failures=2040562
final: live_entries=0 validate=kCorruption: the fetched page claims page_id=68, but the requested page is 67
SOAK_VERDICT: FAILED
```

**这一对照把归因钉死了**：单线程 115 万次淘汰全绿，加上并发立刻红 ——
红来自并发竞争，不是"池子太小所以什么都不能跑"。这与缺陷 #46 的描述一致。

**重复 5 次全红**（`4 2 20000 64`，每次 12 s）：3 次进程直接死在半途（`exit=127`，
输出停在 `Unpin count exceeds the pin count`），2 次活到收尾并报
`validate=kCorruption`。**不变的症状是 `Unpin count exceeds the pin count`**；
`claims page_id=...` 是次要症状。

> 另有一次 512 帧的混合跑出现 `read_wrong_payload=2`（读线程**拿到了别的键的载荷**，
> 是数据级撕裂，比结构报错更严重）。随后 6 次重跑（含 512 帧）**未再复现，故未留证、
> 不作判据**，只记在这里提醒：这条路的失败**不只是**结构报错，也会表现为调用方读到错数据。

### 为了能跑 30 分钟而必须解决的两件事

1. **日志会写爆盘。** 实测这份负载稳态约 2 MB/s，载入 17500 条一次就攒下 **303 MB**。
   驱动因此周期性调 `CheckpointManager::RunCheckpoint()`（截断日志），并遵守检查点
   的两条既定前提：**不在写事务在途时做**（用排空屏障：置 `pause` → 等 `active==0`
   → 取 C0 独占闩做检查点 → 放 `pause`），**与写操作同取 C0 独占**。
   实测每个检查点周期占盘约 **10.6 MB**（5 s 间隔、带写线程），30 分钟跑得下。
   排空等待本身也是**第二个挂起判据**：进度心跳抓不到"某个线程卡在调用里再也回不来"，
   排空等不到 30 s 就 abort。
2. **`stats().num_pages` 在 `Validate()` 之前是 0**（只在自检遍历里统计，
   `tree_ops.cc:1442`）。不先 Validate 一次，`REGIME` 会**永远判成 resident** ——
   一个永远说"没测到"的判据等于没有判据。所以驱动在载入后先 Validate，
   这同时也让"载入坏了"与"并发跑坏了"分得开。

### 顺带补的一个假绿口子

`scripts/check_ascii_literals.py` 的默认扫描根是 `["src","tests","demo","scripts"]`，
**不含 `tools/`** —— 而 `c0_boundary_probe` 与 `soak_mixed` 都是往控制台打字的诊断程序。
于是"所有控制台字面量必须是纯 ASCII"这条铁律**对它们静默失效**，与缺陷 #37 同型
（绿灯来自检查没跑到）。已把 `tools` 加进默认根：现在默认扫 **76 个文件**（原 74），
0 违规。

### 仍未证明

* ~~**30 分钟没跑过**，ASAN 没跑过。~~ —— **C1 落地后都跑过了**（见「C1 本体 · 已落地（含读者侧 crabbing）」：
  30 分钟压测 `report/c1/soak_c1_30min.txt`、ASAN `build-asan` 干净）。
* **驱动现在跑在 C1 下**（C0 已撤），它的绿是 C1 的成绩，不再是"C0 基线"。
  排空屏障（`active==0` 判据）只在"每个写操作各成一个事务"的负载下验过。
* 排空屏障只在**没有长事务**的负载下验证过（本驱动的每个写操作各成一个事务）。
  将来若有跨多步的长事务，`active==0` 这个判据要重新审。

---

## C1 前置件（四）· 帧表边界修复（缺陷 #46）　**已完成**
**（同样不占 M6 的编号：`M6-2` = C1 页级 latch coupling。本节清掉它最后一块拦路石；
C1 本体已于 2026-10-04 落地）**

「下一步」把 C1 的开工顺序写成 (a) 定锁序 → (b) 改帧表 → (c) 两条现成判据从红变绿 →
(d) 再动索引层耦合。本轮把 **(a)(b)(c) 做完了**，C1 本体（d）仍未开工。

### 修了两处，第二处是修复过程中被暴露出来的

1. **帧表临界区**（`src/storage/buffer_pool.cc`）：新增 `Impl::table_mutex`
   （`shared_mutex`），保护 `page_to_frame` / `free_frames` / `clock_hand` 与
   `Frame` 的非原子字段（pid/valid/dirty）。
   * 命中路径取**共享**锁：查表 + 原子加 pin。
   * 未命中/淘汰/安装取**独占**锁并**二次查找**（别的线程可能已经读进来了）。
   于是"查到即加 pin"与"淘汰判定"在同一个临界区里完成 —— 这正是 #46 的根因：
   查到帧之后、加 pin 之前被淘汰，帧就被交给了别的页。
   `StampPageLsn` 改为按 `frame_idx` 取帧（否则 `Unpin` 持锁时 `CollectDelta` 会
   递归获取同一把 `shared_mutex`，直接死锁）。`Unpin` / `FlushPage` / `FlushAll` /
   `resident_pages` / `dirty_pages` 一并纳入临界区。
   **锁序写进了代码注释**：`页闩(自根向下) → frame_table → 该页的 load-lock`；
   缓冲池自己**不**取页闩，所以不会与索引层的 crabbing 成环 —— 这正是「C1 前置件（二）」
   末尾点名的那个坑的绕开方式（不需要单独的 load-lock，因为加载步骤压根不碰页闩）。

2. **`RecordRef` 自持载荷**（缺陷 #47，`index_entry.{h,cc}` + 三棵树的 `Search`）：
   `Search` 在内部守卫析构前把载荷拷进 `RecordRef::owned_payload`。原契约写的是
   "payload 在守卫释放后失效，调用方自行拷贝"，可调用方**拿到指针时守卫已经放掉**，
   根本没有安全的拷贝窗口 —— 并发淘汰下就是 use-after-free。回调式接口
   （`RangeScan`/`SpatialQuery`/`Knn`）保持零拷贝。

### 判据（两条，命令必须带池子参数）

| 命令 | 修复前 | 修复后 | 证据 |
| --- | --- | --- | --- |
| `c0_boundary_probe 8 16 20000` | 5/5 红 | **5/5 绿**（`wrong_value=0 not_found=0`，`Validate=ok`） | `report/c1/probe_after_fix.txt` |
| `soak_mixed 20 4 2 20000 64 6 5` | 5/5 红 | **5/5 `clean`**（`read_wrong_payload=0`，`i6_violations=0`） | `report/c1/soak_after_fix.txt` |

### 判据必须能变红（两次单变量证伪，均逐字节还原）

* **证伪 1**：把未命中路径的独占锁改成共享锁 → 立刻重现 #46 的原始症状
  （`not_found=102811`、`claims page_id=617 but requested 3`、两次进程直接死掉）。
* **证伪 2**：让 `BPlusTree::Search` 不拷载荷 → 5/5 红（`wrong_value=1..2`，而
  `not_found=0`、`Validate()=ok`）—— 精确定位到 #47 是独立于 #46 的一条。

还原后 `buffer_pool.cc` sha256 `9bf0328a…f9972d`、`bplus_tree.cc` `95cb7d8e…cefca`，
源码里 `FALSIFICATION EXPERIMENT` 标记 0 个。全量 ctest **15/15**。

### 仍未证明

* ~~**C1 本体一行没写**：索引层 11 个入口（`bplus_tree.cc:913,924,941,965,1029`、
  `rtree.cc:1486,1655,1672,1739,1815,2048`）**仍全部走 C0**。~~
  —— **已过时：C1 本体已于 2026-10-04 落地，11 个入口的 C0 已全部撤掉。**
* ~~**30 分钟压测与 ASAN 未跑**（本轮最长 20 秒）。~~
  —— **已过时：C1 落地后都跑过了**（30 分钟 `report/c1/soak_c1_30min.txt`、ASAN soak 干净；
  **ASAN 下的全量 ctest 仍未跑完**）。
* **历史 HeatSource 缺口（P1 已修复，当前验收见文首）**：当时两条判据都用
  `heat=nullptr`，未覆盖 HotColdTracker；现在由热度表自身保护全部共享状态。
* **写路径里直接访问帧表的几处**（`NewPage` / `FreePage` / `RollbackWriteOp` /
  `LoadMeta`）仍在临界区外 —— C0 下写被独占闩串行化，故安全；C1 若放开写并行需一并收进锁。

---

## C1 本体 · 已落地（含读者侧 crabbing）（2026-10-06）　**C0 已从 11 个入口撤掉；页闩 + crabbing；两条判据绿、ctest 15/15、30 分钟压测绿、ASAN(soak) 干净**（ASAN 下的全量 ctest 未跑完，见文末）

完整记录见 `report/c1/c1_landed.txt`（第一轮的探索过程见 `c1_groundwork.txt`）。
一句话：**页闩承担"写与读互斥"，写串行化交给 `BufferPool::write_mutex`（D4 单写者），
C0 全局闩从索引层撤掉。**

### 落地的东西

1. **页闩表归 `BufferPool` 持有**（此前生产代码里**没有任何 LatchManager 实例**）；
   `latch_manager.cc` 移进 `ltm_core`（否则 `buffer_pool.cc` 引用它会形成链接环）。
2. **页闩随 `PageGuard` 走**：`FetchPage(pid, mode)`，索引层传 kShared/kExclusive，
   缓冲池内部取页用 kNone；闩在 `table_mutex` **之外**取（锁序 页闩 → frame_table）。
   页 0 / 空闲链页 / 新页用 kNone（页 0 的 level 是 1，写路径持叶闩时取它会判成向上加锁）。
3. **写路径整体标记**：`WritePathScope` + `thread_local g_write_path`，`Fetch` 据此
   把默认共享闩升为独占闩（二十多个取页点不用逐个标注）。**必须 thread_local** ——
   用 Impl 成员会让同树的读线程误取独占闩。
4. **写串行化 `BufferPool::write_mutex`**（D4 单写者）：**先取锁再看深度**，
   `Commit/AbortWriteOp` 的每一层退出都还锁。
5. **结构元数据闩 `structure_mutex`**：`root`/`height` 作为一致的一对快照。
6. **写路径的帧表访问收进临界区**：`NewPage` / `FreePage` / `RollbackWriteOp` 的
   `page_to_frame` / `free_frames` / `clock_hand` / `free_list_head` / `free_pages` /
   `next_unallocated` 访问。
7. **`thread_local g_write_op_depth`** 取代共享的 `write_op.depth` 做"我在写事务里"的判断。
8. **`WalWriter` 内部加 recursive_mutex**（读者会经 `FlushFrame` 调 `Sync/flushed_lsn`）。
9. **`LockOrderChecker` 两处修正**：修掉"合法复用被判成向上加锁"的**假阳性**；
   **删除"同层 pid 不降"**（pid 是分配序不是树序）。测试改为
   `PageLatch.SiblingPidOrderIsNotAViolation`。
10. **读者侧 crabbing（2026-10-06 补完）**：`DescendToLeaf` 改成"拿住父页闩直到子页闩
    到手"（方案 §4.6），并把**闩与 pin 解耦**（新增 `PageGuard::take_latch()`）：
    放掉父页 pin 让帧可复用（每读者仍只钉 1 帧），但继续持父页闩挡住写者改路由。
    朴素做法（父+子两帧一起钉）会把 16 帧的池子钉满 → `kOutOfSpace`，第一轮就卡在这。
    证据 `report/c1/c1_crabbing.txt`。

### 撤 C0 时 ASAN 抓到的两个 heap-use-after-free（都不是页闩本身的问题）

* **bug 1**：`NotePinned` 读**共享**的 `write_op.depth` 判断"我在写事务里"。写事务进行时
  并发读者会看到 depth>0、误入写者私有的 before-image 表（`unordered_map`），与写者并发
  改同一个 map → rehash 释放节点 → UAF。**1 写 1 读即崩**。
  修法：`thread_local g_write_op_depth`。
* **bug 2**：`BeginWriteOp` **先无锁读 `write_op.depth`、再决定是嵌套还是开新事务**。
  两个写线程会同时看到 depth>0 而**都走"嵌套"分支** → 同时动同一个 map → UAF。
  修法：**先取 `write_mutex_`，再看 depth**。

### 按分析补上的一处（ASAN 看不见）

`WalWriter` 内部原本**完全没有同步**。C1 后读者的淘汰路径会经 `FlushFrame` 调
`wal->Sync()/flushed_lsn()`，与写者的 `Append/Commit` 并发即数据竞争。已加 recursive_mutex。
**它是"按分析加的"，不是"被工具证明必需"的** —— 详见下面的局限。

### 判据（C1 落地后）

```
c0_boundary_probe 8 16 20000   → 3/3 绿（wrong_value=0 not_found=0，Validate=ok）
soak_mixed 20 4 2 20000 64 6 5 → 3/3 clean（read_wrong_payload=0，order_violations=0）
                                 contentions≈4.2e3 > 0 —— 页闩真的在争用，不再是空转
ctest（RelWithDebInfo）        → 15/15
ASAN（build-asan）             → **soak 干净**（无 AddressSanitizer 报告）；
                                 **ASAN 下的全量 ctest 未跑完**（只跑了 ASAN 的 soak）

30 分钟压测（撤 C0 那版，`soak_c1_30min.txt`，soak_mixed 1800 6 2 20000 512 10 10）：
  墙钟 30m50s（1800s 只算测量期，前面有载入、后面有收尾），exit=0
  total_ops=121,640,523  read_wrong_payload=0  call_failures=0  i6_violations=0
  final: validate=ok scan=ok   re-read 18,778 keys, 0 wrong payload
  latch: order_violations=0  contentions=1,357,475
  checkpoints=179  peak_wal=80 MB  →  SOAK_VERDICT: clean（REGIME: evicting）
  **注：这一份是 crabbing 之前的代码跑的，已被下面这份取代。**

读者侧 crabbing（2026-10-06 补完）：
  c0_boundary_probe 8 16 20000   → 3/3 绿，**没有 kOutOfSpace**（第一轮正是卡在这）
  soak_mixed 20 4 2 20000 64 6 5 → 2/2 clean，order_violations=0
                                   contentions≈2.7e4（crabbing 前 ≈4.6e3，涨 6 倍 ——
                                   读者真的在父页闩上排队了）
  ctest                          → 15/15
  30 分钟压测                    → `soak_c1_crabbing_30min.txt`：**`SOAK_VERDICT: clean`**，
                                   exit=0，墙钟 30m54s；`read_wrong_payload=0`、
                                   `call_failures=0`、`i6_violations=0`、`order_violations=0`、
                                   **`contentions=7,594,225`**（撤 C0 那版 1,357,475 的 5.6 倍）、
                                   `final: validate=ok scan=ok`、`checkpoints=179`
```

### M6-2 验收口径 vs 历史证据（本轮补齐前；当前见文首「C1 验收补齐」）

方案 §6 的 M6-2 口径，加上 §6 脚注（按本机工具链把"TSAN 干净"改写后的三条），一共 5 项：

| # | 口径（`终版方案.md` §6 及脚注） | 现有证据 | 状态 |
| --- | --- | --- | --- |
| 1 | 混合读写 30 分钟无死锁无挂起 | `report/c1/soak_c1_crabbing_30min.txt`（crabbing 版；撤 C0 那版是 `soak_c1_30min.txt`，已作废） | **有证据** |
| 2 | `LockOrderChecker` 零逆序 | RelWithDebInfo 下 probe / soak / 30 分钟的 `order_violations` 均为 0 | **只算一半**：脚注要的是 `build-dbg/` 下"逆序即 abort"的配置，本轮只跑了 RelWithDebInfo（该配置下检查器**只计数、不 abort**） |
| 3 | `build-asan/` 全绿 | ASAN soak 干净；**ASAN 下的全量 ctest 未跑完** | **未完成** |
| 4 | （脚注③后半）**同种子下多线程结果与单线程基准一致** | `soak_control_single_thread.txt` 是"帧表修复"时的对照，**不是 C1 落地后按同种子做的多/单线程一致性对照** | **未做**（`soak_mixed` 目前也不收种子参数，做这条要先给它加确定性种子） |
| 5 | （脚注）数据竞争检测 | 本机 MSVC 无 TSAN，`CMakeLists.txt` 对 `TSAN+MSVC` 直接 `FATAL_ERROR` | **本机不可达**，写进结题报告"局限" |

**结论的准确说法**：判据 1 有证据；判据 2 只在本构建配置下成立；判据 3、4 未完成；
判据 5 本机做不到。**不要把这一节读成"M6-2 已通过"。**

### 当时未做 / 已知局限（当前进展见文首「C1 验收补齐」）

* **ASAN 下的全量 ctest 未跑完**（交班前中止；只跑了 ASAN 的 soak）。命令：
  `python scripts\build_local.py test build-asan RelWithDebInfo`。
  **环境注意**：`build-asan\*.exe` 需要 `clang_rt.asan_dynamic-x86_64.dll`
  （在 `D:\visual studio\IDE\VC\Tools\MSVC\<ver>\bin\Hostx64\x64\`），已拷了一份到
  `build-asan\`，清理过目录就要重新拷。只做 configure+build 用仓库根的临时脚本
  `_asan_build.py`（当时的临时入口；已于 2026-10-07 清理，原内容留存在清理 manifest 中）。
  当前入口为 `scripts/build_local.py`，可分别执行配置、构建和测试。
* **未做数据竞争检测**：本机 MSVC 无 TSAN（`CMakeLists.txt` 对 TSAN+MSVC 直接 `FATAL_ERROR`）。
  "ASAN 干净 + `LockOrderChecker` 零逆序 + 30 分钟无死锁"**覆盖不了良性数据竞争**，
  结题报告必须写进局限，**不许记成"TSAN 干净"**。
* ~~读者仍是"一次只持一个页闩"（`DescendToLeaf` 取子页前先放父页），**没有做 crabbing**。~~
  —— **已做（2026-10-06）**：读者改成"持父页闩直到子页闩到手"，并用 `take_latch()`
  把闩与 pin 解耦，帧预算仍是每读者 1 帧。见上面「落地的东西」第 10 条与
  `report/c1/c1_crabbing.txt`。
  **代价与新的风险**：crabbing 让"读者持父页闩、写者要父页闩"这条交互变多，
  **跨线程锁序环的风险比之前高**；而 `LockOrderChecker` 只抓单线程逆序，
  这条只能靠 30 分钟压测覆盖（本机无 TSAN）。
* 锁序判据只剩"level 单调不增 + 不重复不升级"；"同层 pid 不降"已删（pid 不是树序）。
* 历史 `HeatSource` 缺口已在 P1 修复；当时两条判据均用 heat=nullptr，当前启用热度
  的实现与实测见文首 P1 小节及 report/c1/heat/，不反向扩大旧证据的覆盖范围。
* **死代码**：`BufferPool::c0_latch()` 与 `C0LatchGuard` 现在只剩
  `tests/concurrency/test_c0_latch.cc` 在用（该测试内容已改成量 C1，但文件名/用例名还叫 C0）。
  要不要改名/删码，下一班定。

### 本轮改动的文件（交接用）

`src/storage/buffer_pool.{h,cc}`（`FetchPage(pid,mode)`、`PageGuard` 持闩、
`write_mutex`、`table_mutex` 覆盖写路径帧表访问、`thread_local g_write_op_depth`）、
`src/storage/wal.{h,cc}`（`Impl` 加 recursive_mutex）、
`src/index/tree_ops.{h,cc}`（`Fetch(pid,mode)`、`WritePathScope` + `thread_local g_write_path`、
`structure_mutex`）、`src/index/bplus_tree.cc` / `rtree.cc`（撤 11 处 `C0LatchGuard`、
`RangeScan` 改"读完一页先放闩再取下一页"）、`src/concurrency/latch_manager.{h,cc}`、
`tests/concurrency/test_page_latch.cc`、`tools/soak_mixed.cc`、`CMakeLists.txt`
（`latch_manager.cc` 移进 `ltm_core`）。

---

## 本阶段定位并修掉的缺陷

| # | 缺陷 | 后果 | 状态 |
| --- | --- | --- | --- |
| 1 | `InitPage` 用 `is_leaf` 改写 `header->level` | 新根页应有的 `level=2` 被覆盖成 1 | 已修 |
| 2 | 根分裂后未把旧根降到"原高度"那层 | 层级自相矛盾 | 已修 |
| 3 | 内部页路由让哨兵槽参与二分比较 | 哨兵被当普通键搬走 | 已修 |
| 4 | `CheckParentKey` 对哨兵槽也套用 I1 | 误报损坏 | 已修 |
| 5 | 递归下钻后沿用旧 `Page` 值（悬垂视图） | 静默写坏别人的页 | 已修 |
| 6 | `AcquireFrame` 返回"未钉住但仍属某页"的帧 | 两个页号共享帧内存 | 已修 |
| 7 | `BPlusTree` 与 `BufferPool` 各自 `reinterpret` 页 0 | 建库时读到全零页误判损坏 | 已修（`meta_page.h`） |
| 8 | **内部页分裂让哨兵与真实键共用槽 0** | 右页哨兵丢失，树损坏 | 已修（`separator_out` + 双哨兵） |
| 9 | 内部页条目搬运用 `Page::AppendEntry`（按 8 字节键算空间） | 条目区与槽目录互相覆盖 | 已修（改走 `InsertEntry`） |
| 10 | 分裂搬运时逐条直写目标页 | 踩掉尚未读取的源条目（左页变 `{68,68,68,68}`） | 已修（先进临时缓冲） |
| 11 | `Validate()` 把"同父页哨兵槽与首键槽指向同一子页"误判为环 | 误报损坏 | 已修（按 parent 区分） |
| 12 | 合并后父页分隔键未更新为合并子树的最小键 | I1 被破坏、路由漏键 | 已修（`RefreshSeparatorAt`） |
| 13 | **`ChildSlotFor` 路由方向反了**（取"第一个 >= key"而非"最后一个 <= key"） | 树第一次分裂后只有最右子页的键可被找到：点查/删除大面积 `kNotFound`（**原遗留 #2 的真正根因**） | 已修 |
| 14 | 删除叶页最小键后父页分隔键不刷新 | I1 被破坏（Validate 报"父页键 != 子页最小键"） | 已修（`RefreshSeparatorsForChild`） |
| 15 | 同一个子页在父页里可能占 0/1 两个槽，只刷新其中一个 | 另一槽立刻陈旧、I1 报错 | 已修（一次刷新**所有**指向它的槽） |
| 16 | `EraseEntry` 只留墓碑槽、`num_slots` 不变 | 删除不再触发借键/合并（树无法降级）、Validate 条目计数与叶链键序误报损坏 | 已修（删除后立即 `Compact()`；Validate 按存活槽计数并跳过墓碑） |
| 17 | 借键只要求"供体当前不下溢" | 借完供体自己下溢，I3 报"填充不足" | 已修（供体必须 `num_slots > 阈值`） |
| 18 | 借来的键被追加到页尾（左兄弟借键场景） | 页内键序被弄反 | 已修（左借插到 0 号槽 / 内部页重开哨兵对） |
| 19 | 相邻槽未必是兄弟（0/1 号槽是一对冗余引用） | 借键"借自己"、合并"把自己并进自己"（实测叶页键整块复制一遍 `{2897,...,2932, 2897,...,2932}`） | 已修（按页号排除自己） |
| 20 | 合并从父页抽走一个槽后不再上报下溢 | 祖先链不再借键/合并，树停在"内部页填充不足"状态 | 已修（`EraseRec` 合并后重新判定并继续上报） |
| 21 | 单轮借键/合并无法把远低于阈值的页补满（可能只剩 1 条槽） | I3 持续报"填充不足" | 已修（`Rebalance` 循环：借到补足或供体无余量，再合并，直到满足或没有兄弟） |
| 22 | 重复键覆盖式插入仍然 `++num_entries` | 元数据计数被撑大，Validate 报"条目总数 != 元数据计数" | 已修（覆盖不增加计数） |
| 23 | 合并/借键/分裂新页通过 `guard.page()` 改内容 | 守卫不被标脏 → 修改不落盘（重开丢数据）、校验和不重算 | 已修（一律走 `mutable_page()`/`mutable_header()`/`mutable_payload()`） |
| 24 | `PrepareVictim` 写脏帧前不重算校验和 | 盘上留下"内容与 CRC 不一致"的页，读回即 `kChecksumMismatch` | 已修（与 `FlushPage`/`FlushAll` 一致，写前 `UpdateChecksum()`） |
| 25 | **`NewPage` 复用空闲页时另取一块帧**（该页刚被 `FetchPage` 读进池里） | 同一个页号占两块帧：孤儿帧被淘汰时把**陈旧内容**写回该页的磁盘位置，并抹掉新帧的映射 → 页被读回旧版本（`type=kFree`、只剩 1 个槽），表现为"父页指向早已释放的页""根页 level 与 height 不符""父页键序被破坏"等静默损坏（**只在小缓冲池/大负载下出现**） | 已修（复用同一帧，并校验空闲页未被钉住） |
| 26 | `BPlusTree::LoadMeta` 直接读 `pool->meta()`，未先触发元数据页加载 | 内存镜像是全零 → 槽位 `in_use == 0` 被解释成"树不存在" → **重开数据库时新建一棵空树覆盖原数据**（`SurvivesReopenWithSmallBufferPool` 重开后 0 条） | 已修（新增 `BufferPool::EnsureMetaLoaded()`，`LoadMeta` 先调用） |
| 27 | `wal.cc` 的 `Commit()` 不做 fsync | 不变量 I6 不成立（强杀可能丢最近提交） | 已修（无条件 `Sync()`） |
| 28 | `Fetch` 不校验"取回的页是否自报家门" | 帧被复用时只能靠后续校验和偶然发现 | 已加断言（`page_id` 与请求页号不符立即报 kCorruption） |
| 29 | **`RTree::Insert` 只做叶内去重**：R 树的插入去向由 MBR 决定、与 key 无关，同一条旧记录可能落在**另一个叶页**里 | 跨叶"幽灵副本"：Search 命中一份、ScanAll 看到两份、Erase 只删掉一份，另一份永久残留。**所有不变量都不报警**，只有 `num_entries` 比真值表大（实测 934 vs 844） | 已修（`Insert` 改为全局 upsert：叶链扫同键 → 按精确 MBR 先删再插） |
| 30 | **`EraseRec` 在候选子树的"死胡同"上报 `kNotFound`**，调用方的 `LTM_RETURN_IF_ERROR` 把它当致命错误直接中断 | R 树 MBR 重叠天然产生假阳性候选：先探到的那条路走到底没有候选，整次删除就被误报"记录不存在"，**真正装着记录的兄弟候选根本没被访问**。症状：Validate 全绿、ScanAll 与真值表一致、计数也对，只有 Erase 说不存在 | 已修（"不在这里"返回 Ok + 显式吞掉递归的 `kNotFound` 继续试兄弟候选） |
| 31 | **合并/借键后按"父页槽位相邻"修补叶链**（`left->next = right->next`） | 父页槽位相邻 **不等于** 叶链相邻：内部页借键会把一个子页从一棵父页的槽搬到另一棵父页的槽，该子页的叶链位置纹丝不动。于是把两页之间一整段叶页从链上摘掉，被摘段末端仍指向已释放（可能已重分配）的页，链缠成一团。症状：ScanAll 突然少几百条，而 Validate 全绿（I9 与链序无关） | 已修（改用 `right->prev_page` 找链上真前驱再嫁接） |
| 32 | **`SplitInto` 丢掉槽 [mid,n) 时不记 `frag_bytes`、不压实源页**：条目区从页尾向下生长，槽 offset 取决于到达顺序而与键序无关，被丢槽的偏移与保留槽**交错**，留下空洞；而 `free_space` 的口径是"最高地址 - 最低偏移"的**跨度**，空洞被算成已用（实测一页 28 条、真实占用 897 字节被记成 3931，`free_space` 只剩 5） | 页只装了 28/32 条就报 `kPageFull`，于是在 28 条处中点分裂成 14/14，**两半都低于阈值 16**，`Validate` 报 I3"页填充不足"。触发条件是**纯插入**（没有删除/合并去顺手压实页），混合负载的差分测试与 M2 验收都照不到 | 已修（分裂时把腾出的字节记入 `frag_bytes` 并 `Compact()` 源页；另加"分裂合法性前置检查"：分不出两半都合格时明确报错而非静默拆） |
| 33 | B+ 的「字节先满」路径原本**直接返回 `kPageFull` 报错**，从不分裂 | 变长条目的树在页槽数未到 `capacity_` 时卡死；且它与 #32 是同一现场的两面 | 已修（新增通用协议：插入 → `kPageFull` → 分裂 → `ChooseHalf` → 选半重插，仍放不下则明确报"条目过大"） |
| 34 | **`SyncFstream` 在两个平台分支上都只 `flush()` 就返回 `Ok`** —— `#27` 只修好了"调用 `Sync()`"这个语义，物理屏障本身是空的 | `flush()` 只把用户态缓冲交给内核，字节仍可能停在内核/设备缓存；断电即丢。即不变量 I6 的物理一半**不成立**，而代码与日志都声称"已同步"（比崩溃更糟：错了还说对了） | 已修（Windows `_commit` / POSIX `fsync`，失败返回 `kIoError`；`SyncFstream` 函数头写明为何必须用第二个句柄）。**按性质无法单测**，端到端证据是 M4-3 崩溃矩阵 |
| 35 | `bench_main.cc` 仍打印 **"[M0] 存储层与索引层尚未实现，基准测试暂无数据"** | M1–M3 早已完成，这句话把已交付的能力说成未实现 —— 是对外的错误陈述 | 已修（改为"HybridIndex 属 M5，故暂无基准数据"） |
| 36 | 三处**中文标点残留在已经"翻译过"的英文消息里**（`buffer_pool.cc` 的 `… cursor 1）`、`test_harness.h` 的 `… (两侧均为 `、`bench.py`/`crash_matrix.py` 的 `∈`） | 第一轮提取器只认汉字区间 U+4E00–U+9FFF，**全角标点与数学符号全部漏网**；`）` 一路进了 demo 的实际输出。`∈` 更糟：GBK 代码页无法表示它，Python 直接 `UnicodeEncodeError` 崩溃 | 已修（提取器扩到全角/中文标点与 `—`/`×` 等；并加了一道"字面量内不得出现任何非 ASCII"的全量兜底扫描） |
| 37 | **`CMakeLists.txt` 的 ASAN 开关静默失效**：写作 `if(LTM_ENABLE_ASAN AND NOT MSVC)` 且**没有 `else` 分支**，而本工程唯一的构建器就是 MSVC | `-DLTM_ENABLE_ASAN=ON` **成功接受、静默什么都不做** —— 配置成功、构建成功、测试全绿，但那是**假绿**。后果比"开关不存在"更严重：方案 §6 把「**ASAN 全绿**」写成 M3 门槛之一，M3 却**从未在 ASAN 下跑过**，门槛被记录成"通过" | 已修（MSVC 走 `/fsanitize=address`，GCC/Clang 走 `-fsanitize=address`；`LTM_ENABLE_TSAN=ON + MSVC` 改 `FATAL_ERROR`，使"开关与平台不匹配"再不可能静默降级）。**已补跑**：`build-asan/` 下 9/9 通过（M4-2 后为 10/10）、两个守门程序均退出码 0（见上面「ASAN 配置下的复现」） |
| 38 | **WAL 把 REDO 记录写在 UNDO 记录之前** | 崩溃可以落在两条记录**中间**，于是日志里出现"有 after-image、没有 before-image"的页：Redo 把未提交的插入重放上去，而 Undo 的待回滚页集合是**按 UNDO 记录定位**的，根本不知道要回滚它；同时元数据页的 delta 排在更后面、压根没写进日志。症状：**页里的条目数比元数据计数多 1**，而键集合完全正确 —— 数据看着对，不变量已经破了。**由崩溃矩阵发现**（`wal_append n=40 / random`：`total entries in pages 6 != metadata count 5`） | 已修（`AppendPageDelta` 改为 **UNDO 在前、REDO 在后**）。方向性理由写在 `wal.cc`：UNDO 在前时"两条只落一条"的所有情形都可恢复（页不在脏页表 ⇒ Redo 不碰它；Undo 找得到它；而 before-image 正是这一页此刻在盘上应当有的样子），REDO 在前时有一种不可恢复。回归用例 `test_recovery.G`（152 个日志前缀） |
| 39 | **前缀测试构造了"真实崩溃到不了的状态"**：把"跑完让池析构刷盘"的页文件配上任意截断的日志 | 那等于"数据文件比日志更超前"，而写前日志规则（`FlushFrame` 写页前必先 `Sync`）保证真实崩溃永远不会这样。用这种状态去测恢复，测的是"恢复能不能修复一个不可能发生的世界"，**只会得到假失败**（实测 168 个前缀报"页里 37 条、元数据说 45 条"） | 已修（页文件快照改取**检查点那一刻**：检查点的 flush_all 已把此前一切写进数据文件，描述它们的 begin/end 记录又在所有待测前缀之前，于是每个前缀都对应一个**可达**的崩溃现场）。这条本身是"测试假设错了"，与 #38 是两件事，分开记 |
| 40 | **检查点不检查"写事务是否在途"** | 截断的语义是"end 之前的一切都已落在数据文件里"，对已提交改动成立；但对一个还开着的写事务，它的 **UNDO 会被一起截掉** —— 而页里可能已经带着未提交的改动（steal 刷盘），崩溃后回滚这件事就失去了依据 | 已修（`RunCheckpoint` 在 `pool->write_op_active()` 时直接拒绝并说明理由）。本设计的检查点由应用在两次操作之间发起，这条约束不带来不便 |
| 41 | **方案 §4.9 的"B+ 叶条目只存 R 树叶页号"这条设计不成立**：R 树的**分裂会把一半条目搬到新页**（借键与合并同样会搬），于是插入时记下的叶页号在批量装载后大面积失效 | 实测 2000 条纯插入后提示命中率只有 **50%**（1001 命中 / 999 失效，`report/m5/demo.txt`）；每次失效都要做一次 O(R 树叶页数) 的叶链全扫，于是 `Get`/`Erase` 有一半概率退化成全扫 —— "混合索引按键定位是 O(log n)"这句话不成立。**这条是设计文档错了，不是实现错了**，所以记在缺陷表里而不是悄悄改掉 | 已修（记录改为**整条存在 B+ 侧**：键 + MBR + 载荷。Get 一次 B+ 定位、不访问 R 树；Erase 从 B+ 记录里取精确 MBR 再删 R 树；R 树只回答空间问题。叶页号降级为纯提示，只给 I8 交叉校验用）。证据：用例 E 断言"100 次 Get 之后 R 树的定位计数纹丝不动"，用例 B/E 给出提示命中率（混合负载 74%、重合并负载 43%） |
| 42 | `Validate()` 的 I8 报错**说不清是哪一条不一致**：扫描回调返回 false 就停扫，于是"少了一条"与"两条对不上"都会退化成"条目数不一致" | 报错信息指向错误的原因，排查时得自己再写一遍交叉校验 | 已修（分别记录 `missing` / `disagree` / 首个不一致的键号，报错带键号与原因）。用例 I 直接断言报错信息里含 "I8" 与键号 |
| 43 | **崩溃矩阵的 B+ 引擎漏调 `Recover()`**（把 `crash_test` 重构成 `Engine` 抽象时引入） | 恢复没跑，索引直接读崩溃现场：page 0 全零而日志非空，被 I6 检查当场抓住并毒化池。若没有这条检查，症状会是"恢复后的数据莫名其妙少了一截" | 已修（`BPlusEngine::Open` 里补上 `Recover()`，并把"恢复必须先于任何索引访问"写进注释）。**它之所以能被发现，是因为同一条判据同时覆盖两个引擎** —— 两个引擎各写一套判据时，漏掉的那一边不会被另一边发现 |
| 44 | **`C0LatchGuard` 嵌套析构时按"请求的模式"而非"实际加过的计数"去还锁**：已持独占时再要共享，构造函数加的是**独占**计数，析构却去减共享计数 —— 独占计数永远减不到 0，锁**永久泄漏** | 表现是"下一个取独占闩的线程永久挂起"。最容易误判成死锁或性能问题（"跑得慢"），实际是锁泄漏。生产路径当时没踩到（写路径不嵌套），但只要出现"独占闩内嵌一个取共享闩的读调用"就会立刻踩到 —— 而 `HybridIndex::Put` 将来正要做这件事 | 已修（守卫改记 `Counted{kNone,kShared,kExclusive}`，严格按实际加过的那个计数还）。**是新写的 `C0.LatchGuardIsReentrant` 用例抓出来的，不是推理出来的** —— 该用例里"所有守卫析构后 `try_lock()` 必须成功"这一句是关键判据；没有它，泄漏会一路潜伏到某人加读调用时才以"挂起"的形式爆出来 |

| 45 | **写事务 `Abort` 只写 `kAbort` 记录、不回滚内存状态**（页帧、分配器游标/空闲链、索引层的 `root`/`height`/`num_entries`） | 运行期留下"日志说没有、内存里却有"的索引。**比原记录更重**：abort 之后干净关闭会把幻影记录刷进页文件，而干净关闭不跑恢复，那条 `kAbort` 永远没人读 —— 重开后 `entries=600` 而基线 400。`HybridIndex::Put` 的"R 树成功、B+ 失败"是常态触发路径 | 已修（见「C1 前置项」一节）。**三类状态只有第一类在页里**，第三类决定了修复出不了缓冲池，因此新增了 `RegisterWriteUndo` 参与者协议。**两个关键事实是实测出来的、不是推出来的**：① 把新建页按"还原 before-image"回滚会撞 I6 并毒化池（→ 改为"收回脏位"）；② `Validate` 报的 `400 != 600` 才暴露出派生状态这一整类。红/绿输出与逐字节还原证明在 `report/abort_rollback/` |

M0 阶段另有 4 个缺陷（`Compact` 漏算槽目录空间、`frag_ratio` 分母口径、
`Mbr::Area()` 返回 `inf`、`EvictScore` 热度符号写反），见各自回归用例。

### 缺陷 #46 与前面所有条目的性质不同：**它先是"已知边界"、被实测证实，随后本轮修复**

> **状态更新（2026-10-04）**：下面这段保留为"修复前"的原始记录。**#46 已修复**，
> 见「C1 前置件（四）」一节与 `report/c1/fix46_summary.txt`；两条判据已 5/5 红→5/5 绿。
> 修复过程中还牵出缺陷 #47（`Search` 返回悬垂视图），一并修掉。

| # | 缺陷 | 后果 | 状态 |
| --- | --- | --- | --- |
| 46 | **C0 的共享闩保护不到缺页/淘汰路径**：`BufferPool::Impl::page_to_frame` 是裸 `std::unordered_map`，`FetchChecked` 在共享闩下做 `find` → `++pin_count`，未命中时做 `page_to_frame[pid] = frame_idx`；`free_frames` / `clock_hand` 同样无保护 | 多读单写不出事，但**多读 + 工作集放不下**时帧表被并发写坏（实测 `not_found=219256`、Segfault、`claims page_id=596 but requested 3`） | **已修**（见「C1 前置件（四）」）：新增 `Impl::table_mutex` 临界区（命中共享 / 未命中-淘汰-安装独占 + 二次查找），"查到即加 pin"与"淘汰判定"同临界区；`StampPageLsn` 改按帧号取帧。两条判据 **5/5 红 → 5/5 绿**，含两次单变量证伪（`report/c1/`） |
| 47 | **`Search` 返回 `RecordRef` 时 payload 指向页帧、而守卫已析构**：契约写"调用方自行拷贝"，但调用方**拿到指针时守卫已经放掉**，根本没有安全的拷贝窗口 | 并发淘汰下 use-after-free —— 读回**别人的载荷**。缺陷 #46 修好后由 `c0_boundary_probe` 单独暴露（`wrong_value=1`，而 `not_found=0`、`Validate()=ok`，说明帧表已经安全、问题在返回的视图） | **已修**：`RecordRef` 增加 `owned_payload`，`Search` 在守卫析构前把载荷拷进去（回调式接口保持零拷贝）。证伪：让 `Search` 不拷 → 5/5 红（`report/c1/red_falsification_search_view.txt`） |

这条此前一直写在 M6-1 一节的"**定义边界**"里，措辞是"缺页路径在共享闩下不安全"，
`test_c0_latch.cc` 甚至把它编码成断言（测量期 `misses` 增量必须为 0）。
**但它从来没有被实测过** —— 按本项目的规矩（缺陷 #37 的教训），一个"能出事的路径"
如果没演示过它真出事，就只是描述，不是证据。本轮把它测了：

```
build-ninja/c0_boundary_probe.exe 8 4096 20000   → 绿（工作集 647 页全驻留，缺页 0）
build-ninja/c0_boundary_probe.exe 8 16   20000   → 5 次跑 5 次红
```

实验组（16 帧 ≪ 647 页）每次约 **0.11 秒**就跑出结果，两种失败形态：
一次 **Segmentation fault**，一次给出

```
wrong_value=26 not_found=219256
tree.Validate()=kCorruption: the fetched page claims page_id=596, but the requested page is 3
```

其余三次进程直接死掉、没打印 verdict。每次另有 120–220 条
`Unpin failed on release`（`exceeds the pin count` / `not in the pool`）。
`not_found` 是"返回没找到"，而 **`wrong_value` 是"返回了别的记录的载荷"** ——
不是崩溃，是静默的错数据。

探针故意**不注册进 `LTM_TEST_SOURCES`**：它是已知待修问题的诊断工具，红是预期输出；
让它常驻红灯会把测试套件变成一个"习惯了失败"的东西。逐条输出在
`report/c1/c0_boundary_probe.txt`，完整分析在 `report/c1/README.md` 第五节。

---

## 下一步（严格按序，2026-10-07 更新）

详细任务和退出条件见 [后续实施策略](strategy.md)。1–5 已完成其限定范围；主链路已实跑，
元数据页校验缺口已修复并验证，WSL/TSAN 与 C1 验收已补齐；固定混合读写限定范围也通过，进入第 6 步。

1. **ASAN 证据已补齐**：16/16 普通测试与两个结构守门通过，见文首本轮验收。
2. **Debug 专项已通过**：4/4 正确性测试、两个结构守门、故意逆序的独立子进程预期 abort。
3. **B+ 确定性并发对照已通过**：三种子固定操作流，单/多线程及独立模型比较完整最终记录；
   同种子本身不保证调度或最终结果一致。具体设计见 strategy.md §3。
4. **P1 约定范围已完成**：热度跟踪、统计快照和组合查询同步边界已修复；
   启用热度、回调重入、检查点与双树校验、同对象重开及失败后重试均有实测。
5. **P2 B+、Hybrid 查询与固定 mixed 基准已完成限定范围**：真实负载、独立结果核对、
   计时、结构化指标与严格驱动贯通；mixed 在三配置共 18 轮与 18 次新进程重开通过。
6. **有效实验与 M7 收尾**：先分离正确性与测量轨迹，再扩大数据量/分布，补显式检查点恢复曲线；冷热实验依赖第 4 步。
   8K/16K 页大小实验延期（BufferPool 仅支持 4KB），预取和自动检查点须先证明路径已实现。
7. **后续可选任务**：后台衰减/刷脏页、自动检查点、C0 死代码和测试命名清理。

2026-10-06 因页扫描源码变化已复测 crabbing 30 分钟，并完成热度重复轮次验证；
其版本哈希与 2026-10-07 生命周期修复后的最终源码验收分别保留。P2 B+ 与 Hybrid 查询闭环
及固定混合读写均已完成限定范围，后续扩独立测量实验；历史 P1 源码哈希仅对应当时版本，基准修改后的版本见 report/bench/。
2026-10-07 已安装 WSL2 / Clang TSAN，修复真实回滚竞争并完成本轮全量、Debug 连续压力
及夹具清理补验，M6-2 约定门槛已补齐。ASAN、锁序检查和压测仍不替代 TSAN。
固定操作流和测量口径见 [混合读写约定](../report/bench/mixed_design.md)，实测见 [混合基准说明](../report/bench/mixed/README.md)。

---
## 已知开发陷阱（会反复踩，务必先读）

见 `docs/architecture.md` §7。本阶段新增：

- **不要在递归下钻前后缓存 `Page` 值**：它只是帧的视图，帧会被淘汰复用。只用
  `guard.page()` 现取。
- **页 0 只能由 `BufferPool` 读写**：上层用 `WriteTreeSlot()` / `EnsureMetaLoaded()` /
  `meta()`；**读 `meta()` 之前必须 `EnsureMetaLoaded()`**，否则读到的是全零镜像。
- **内部页条目必须经 `InsertEntry` 写入**，不能用 `Page::AppendEntry`（尺寸口径不同）。
- **分裂/合并搬运前先把源条目拷进临时缓冲**：源页与目标页条目区在同一帧两端，直写会互相踩。
- **改页内容必须走守卫的 `mutable_*` 接口**：用 `guard.page()` 拿到 `Page` 再改内容，
  守卫不会被标脏 —— 修改不落盘、校验和也不重算（重开丢数据 / 读回 `kChecksumMismatch`）。
- **一个页号只能占一块帧**：`NewPage` 复用空闲页时必须复用它的**同一个帧**，
  否则会出现"孤儿帧把陈旧内容写回磁盘、顺手擦掉新帧映射"的静默损坏
  （只在缓冲池需要淘汰时出现）。
- **下溢页可能远低于阈值**：合并会一次从父页抽走一个槽，页可能只剩 1 条槽，
  "借一条就够了"不成立；删除路径的借键/合并必须循环到真正补足为止。
- **相邻槽未必是兄弟**：内部页 0/1 号槽是一对冗余引用（指向同一个子页），
  按槽号找兄弟时必须先按**页号**排除自己。
- **删除后立即压实**：`EraseEntry` 只留墓碑槽、`num_slots` 不减，
  会让下溢判定、条目计数、叶链键序检查全部失真。
- **`free_space` 是"跨度"口径，不是"存活字节之和"**：`EntryAreaUsed` 量的是
  「最高地址 − 最低存活偏移」，因此**任何留下空洞的操作都必须把腾出的字节记入
  `frag_bytes` 并压实**，否则空洞会被算成已用、`free_space` 塌成谎报（缺陷 #32）。
  分裂截断槽目录、删除中间槽都属于这类操作。
- **分裂点由页算子定，通用层必须前置检查它是否合法**：中点分裂在
  `存活数 < 2 × 下溢阈值` 时必然产出低于阈值的一半。宁可报错也不能拆 ——
  静默拆出来的树当场就是 `Validate` 眼里的损坏状态（缺陷 #32 的第二半）。
- **"父页槽位相邻"≠"叶链相邻"**：多维树的内部页借键会搬运子页在父页里的槽位，
  却不改变它在线性链上的位置。修链必须按**页自己记录的 prev/next**，不能按父页槽号（缺陷 #31）。
- **`Insert` 在通用层必须是"同主键覆盖"语义**：页算子只能做**页内**去重，
  而多维树的插入去向由 MBR（不是 key）决定，同键旧记录可能落在别的页里 ——
  只做页内去重会留下跨页幽灵副本，且**所有不变量都不报警**（缺陷 #29）。
- **查找半边不可共享**：单路径下钻对"选唯一去向"的插入成立，对查找不成立
  （一个探测范围可能同时落在多个孩子里）。`Search`/`Erase` 的定位与
  `RangeScan`/`SpatialQuery`/`Knn` 必须各树自己实现，否则 MBR 一重叠就漏记录。
- **"死胡同"不是错误**：多维树的路由天然产生假阳性候选，子树的递归查找/删除
  返回 `kNotFound` 只说明"不在这里"，调用方必须继续试兄弟候选；
  用 `LTM_RETURN_IF_ERROR` 一把梭会把它变成静默的提前退出（缺陷 #30）。
- **恢复直接读写页文件，不经过缓冲池**：所以 `Recover()` 必须在**任何索引打开之前**调用，
  且此时池里不应有驻留页；池非空时 `Recover()` 会告警。`RecoveryManager` 的构造顺序是
  `DiskManager → RecoveryManager（持有 WalWriter）→ BufferPool（借用它）`，
  销毁顺序相反（池先走 —— 它的析构要在日志还活着的时候写 `clean_shutdown`）。
- **`clean_shutdown` 的判据要取"打开时捕获的值"**：构造函数已经把它在盘上清 0，
  在 `Recover()` 里重新读只会读到 0，于是"干净关闭 ⇒ 跳过恢复"这条短路永远不生效。
- **不要在 `FlushFrame` 里为"建库引导"开豁免口**：唯一安全的豁免是"日志里一条记录都没有"。
  "数据文件是本次新建的"与"这一页确实没有 REDO 覆盖"并不等价 —— 数据文件被删掉、
  日志却留着时，那个口子会让 I6 在最需要它的时候失明。
- **两条记录之间也会崩**：日志里"只有一半记录存在"是常态而不是异常。任何"按记录类型定位
  待处理页"的恢复逻辑，都必须先问一句"另一半没落盘时会怎样"（缺陷 #38）。
- **写测试时先问"这个状态真实崩溃到不到得了"**：数据文件比日志更超前是不可达状态
  （写前规则保证不会），用它做基准只会得到假失败（缺陷 #39）。
- **批量改 C++ 代码不要用 PowerShell `-replace` / Python 字符串拼接，也不要用
  `Get-Content`/`Set-Content` 往返读写源文件**：本阶段因此反复弄坏换行、转义与括号平衡，
  更严重的一次是 Windows PowerShell 5.1 用 ANSI(936) 解码 UTF-8 源文件再写回，
  把全部中文注释与字符串变成乱码（14% 的汉字因 GBK 非法字节对丢失）。
  **必须用 `edit` 工具做精确替换；批量文本修复要么用 `edit`，要么在 Python 里显式
  指定 `encoding='utf-8'` 并且只写回自己构造的字节。**
- **"存页号"这类定位提示一定会过期**：任何会搬运条目的结构变化（分裂、借键、合并）
  都会让"当时记下的页号"变成错的值，而错的值比没有值更危险（可能指向已释放、
  已被别的树复用的页）。要么把它当**提示**并在取用时校验 + 兜底，
  要么把需要的信息**内联进记录本身**（M5 的最终选择）。缺陷 #41。
- **回调返回 `false` 会吞掉错误**：`ScanAll`/`RangeScan` 的 emit 返回 false 只是"停止扫描"，
  调用方拿到的仍然是 `Ok`。想在里面报错必须把 `Status` 捕获到外面再返回
  （`HybridIndex::Query` 的 kRange1D 分支就是这么写的）。缺陷 #42 是同一个坑的另一种形态。
- **重构出抽象层时，最容易漏掉的是"必要步骤"而不是"逻辑"**：崩溃矩阵重构出 `Engine`
  抽象时漏了 `BPlusEngine::Open` 里的 `Recover()`（缺陷 #43）。
  能发现它，是因为**同一条判据同时覆盖两个引擎**；两个引擎各写一套判据时，
  漏掉的那一边不会被另一边发现。
- **源文件里的字符串字面量一律 ASCII，注释保持中文**（本阶段已全量执行）。这不是洁癖，
  是上面那条陷阱的根治：本机控制台代码页是 GBK(936)，而 Python 的 stdout 用的是本地
  代码页 —— 同一个 UTF-8 中文字面量在 C++ 的 `printf` 下正常、在 Python 的 `print`
  下变乱码，**遇到 GBK 无法表示的字符（如 `∈`、`—`）时 Python 直接抛
  `UnicodeEncodeError` 崩掉**，而不是打印乱码。也就是说中文输出在任何一端都不是
  可靠契约。判据是「这段文本会进控制台吗」：
  - 会 → 必须 ASCII（`src/`、`tests/`、`demo/`、`scripts/` 的全部字面量，
    含**全角标点** `（）`、`：`、`，` 与 `——`，这些曾经残留在"看起来已经英文"的消息里）；
  - 不会 → 保持中文（注释、`docs/**`、`README.md`）。
