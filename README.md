# LTM — 通用磁盘页式树形索引

C++17 实现的 larger-than-memory 索引工程。B+ 树与 R 树共用定长页、有限缓冲池、
WAL 与恢复内核，HybridIndex 在同一写事务中维护有序及空间索引。

支持按键查询、范围查询、空间相交/包含和 KNN 查询；实现检查点、
Analysis/Redo/Undo 与 CLR、单写者/多读者访问及跨进程持久化核验。

## 目录

当前磁盘页式实现位于 `ltm-index/`。仓库原有的根目录 `src/` 保留为早期中期
内存原型历史，不参与当前构建；当前使用与开发请进入 `ltm-index/`。


| 路径 | 内容 |
| --- | --- |
| [ltm-index/src](ltm-index/src) | 存储内核、索引、事务与恢复源码 |
| [ltm-index/tests](ltm-index/tests) | C++ 回归、差分及不变量测试 |
| [ltm-index/demo](ltm-index/demo) | 演示和基准程序 |
| [ltm-index/scripts](ltm-index/scripts) | 构建、完整链路、基准与验收脚本 |
| [ltm-index/tools](ltm-index/tools) | KNN、恢复及压力测试工具 |
| [ltm-index/docs](ltm-index/docs) | 架构、接口、不变量和验收说明 |
| [ltm-index/report](ltm-index/report) | 历史验证说明与选取的汇总记录 |
| [终版方案](终版方案.md) | 设计规格与实施方案 |

## 构建与运行

需要 CMake 3.20+ 和支持 C++17 的编译器。自动化脚本需要 Python 3。
Windows 可使用 Visual Studio C++ 工具链；Linux 可使用 GCC/Clang。
当前运行脚本应放在 ASCII 路径中，允许英文空格。

```sh
cd ltm-index
cmake -S . -B build
cmake --build build --config RelWithDebInfo -j 2
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

完整持久化链路（脚本负责构建、写入、查询、检查点以及新进程重开验证）：

```sh
python scripts/run_pipeline.py
```

Windows 推荐的构建入口也可使用：

```powershell
python scripts/build_local.py all build-verify-current
```

基准和验收参数见 [工程说明](ltm-index/README.md)、
[工程验收](ltm-index/docs/acceptance.md) 和 [KNN 工具](ltm-index/tools/knn_bench/README.md)。

## 当前范围与实验依据

核心存储、B+ 树、R 树、混合索引和故障恢复已有验证记录。
固定总工作量 KNN 普通矩阵 54 次，连同检测器小矩阵共 78 次测量及 78 次新进程重开；
两类混合轨迹共 36 次运行与 36 次新进程重开。各轮数据属于其记录的源码与实验版本，
不能混合计时口径计算提速，也不能把有限缓存实验等同于超过物理内存的规模测试。

频繁淘汰下的并发成本、更大的工作集和更多负载分布仍待进一步验证。
详细进度以 [status.md](ltm-index/docs/status.md) 为准。

本仓库上传源码、构建测试脚本、技术文档及必要的汇总验证记录。
不包含结题报告、PPT、学校材料、本机维护资料、缓存、历史副本、预编译程序、
运行数据库和逐场景大型输出。`report/` 中历史路径用于描述原实验；未上传的逐场景
数据库、日志和完整输出可通过对应脚本重新生成，汇总结果本身不替代重新验收。
