# 工程验收入口

本文件说明磁盘页式 LTM 工程的验收方法，完成状态以 `status.md` 和对应原始清单为准。
结题材料与工程验收分别维护。

## 快速检查主链路

在工程根目录运行，使用新的构建目录保留已有实验版本：

```powershell
python -B scripts/run_pipeline.py --build-dir build-verify-current
```

该入口执行真实编译、写入 512 条记录、检查点、查询核对、关闭，再由新进程打开
同一数据库核对全部记录。必须看到 `PIPELINE_VERDICT: passed`，不能将仅 Open
成功或缺少指标的输出当作完成。数据与运行记录保存在新的 `report/pipeline/` 子目录。
同一入口支持 Windows 的 MSVC 构建和 Linux 的 CMake/Ninja 构建；Linux 的
`--skip-build` 直接使用无 `.exe` 后缀的程序，不再检查 Visual Studio。

## 扩展验收

```powershell
python -B scripts/accept_mixed_measure.py --phase all
python -B report/acceptance/mixed_measure_20261016/verify_evidence.py
```

`all` 顺序执行 Windows 普通、ASAN 和已安装的 WSL Ubuntu / Clang TSAN 配置；
也可用 `--phase ordinary`、`--phase asan` 或在 Linux 内用 `--phase tsan` 分项执行。
没有对应工具链时该项应明确失败，不能自动降级成普通构建。

| 检查 | 通过条件 |
| --- | --- |
| 普通与 ASAN | 先构建再完整 ctest 20 项；ASAN 运行库实际导入 |
| TSAN 相关专项 | mixed 参考、R 树差分、Hybrid 并发三个测试目标；全编译单元插桩、运行库符号、真实竞争正反对照 |
| 结构与静态守门 | 每种配置分别运行 B+ / R 结构检查、分层、控制台 ASCII 检查 |
| 结果协议 | 10 项 mixed、8 项只读协议回归；缺字段、错计数、坏时间、跨 schema、隐藏探测、非法参数、覆盖已有库和超时均拒绝 |
| 主链路 | 每种配置写入、保存、关闭与新进程重开均正确 |
| 双轨迹 | 同输入分别用独立数据库运行 checked 与 measure；12/128 帧各三次；末尾完整记录、双树 I8、各自的新进程重开全部通过 |
| 独立核对 | 重新解析原始输出、复算样本/CSV/摘要、核对数据库及源码/二进制哈希，并检查历史证据未改变 |

任何阶段失败均返回非零退出码，保存当次源码、输出及失败清单；失败记录不能覆盖
或混入最终通过结果。每次验收自动分配含配置和时间的独立构建目录，同时创建
新的证据子目录，保留已被历史清单引用的程序和数据；实际构建位置写在清单中。

## 混合轨迹的含义

- `--trajectory=checked` 使用 schema 1，在每次 Put / Erase 后额外 Get 验证。
- `--trajectory=measure` 使用 schema 2，删除这些额外索引请求；读返回仍与独立
  完整记录模型核对，统计仍按 API 前后采样。不是完全无观测的性能测量。
- 两类轨迹的输入、操作、查询和最终记录指纹必须一致，测量区间与各自的 API /
  探测 I/O 分开计算。schema、scope、调用数和 CSV 轨迹标签一起核对，拒绝混用。
- 原有 `IndexStats.queries` 统计成功 Get 与 Query 调用，未命中 Get 不增加该值。
  验收同时记录原计数增量、已验证的未命中 Get 和二者合成的完整读请求数，并从
  独立操作历史复算。每 300 次操作，checked 为 300 次读请求，其中 90 次为写后
  探测；measure 为 210 次读请求，探测次数、耗时与全部探测 I/O 均为零。
- API 延迟不含参考计算及统计采样；墙钟吞吐包含它们。应用逻辑页请求不等于
  设备物理 I/O。驻留/淘汰必须由实际页数与真实缺页、淘汰证明。

## KNN 固定总工作量与多读者

```text
python -B scripts/knn_bench.py
python -B scripts/knn_bench.py --profile asan --quick
# Linux / WSL 内：
python3 -B scripts/knn_bench.py --profile tsan --quick
```

普通默认使用新构建和新数据库，1,024 条记录、4,096 次固定查询，k=1/16/64、
1/2/4 读者、16/256 帧各三次，共 54 次测量与 54 次独立进程重开。不同读者数
分摊同一批总工作量，完整参考与校验在计时外。全驻留须实际零读页/零淘汰，
淘汰场景须 R 树页数超过帧数且真实读页/淘汰非零；测量期无写入及 WAL 追加。
协议检查实际执行程序，拒绝错误输入、覆盖已有库及错误 schema/计数/时间。

检测配置强制使用 `--quick` 小矩阵，结果仅证明正确性。完整测量边界与参数见
[工具说明](../tools/knn_bench/README.md)。2026-10-16 三配置共 78 次测量与 78 次
新进程重开均通过；冻结历史入口的一行 Windows 工具定位差异由专门核对程序
验证，运行 `python -B report/optimization/knn_matrix_20261016/verify_evidence.py`。
原始数据和结论见 [本轮结果](../report/optimization/knn_matrix_20261016/README.md)。
该工具不替代核心全量回归；先前同一核心版本的验证继续单独引用。

## 清理可生成文件

```powershell
./scripts/clean_intermediates.ps1
./scripts/clean_intermediates.ps1 -Apply
```

第一条只列计划；第二条删除工程内可重新编译的对象文件、编译器中间符号、增量
链接缓存及 Python 字节码。脚本检查绝对路径、跳过链接和重解析点、排除报告中以
哈希标识的证据；可执行文件、库、根目录调试符号、数据/WAL、源码和报告保留。
清单保存在 `report/maintenance/`。清理后首次重新编译需要重建中间产物。

## 验收边界

本入口的 paired mixed 为 4KB 页、二维 quadratic R 树、实际热度策略、单线程固定
有界输入；普通配置的重复观察与 ASAN / TSAN 正确性结果分别表述。它不替代完整
Linux 套件、长时间压力或历史强杀矩阵，也不代表系统内存超限、冷设备启动和断电
持久性实验已经完成。单写者、多读者仍是核心并发契约。
