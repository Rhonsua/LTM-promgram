# Linux TSAN 验收（2026-10-07）

当前：修复后 TSAN 全量测试 19/19、结构/链路验收通过；Windows 普通/ASAN、
Debug 专项、故障注入矩阵及连续 30 分钟 Debug 压力均通过；核心全量证据独立复核通过。
随后修正三个测试夹具的文件清理顺序，四种构建的受影响目标复测及组合独立复核通过。
完成状态以 docs/status.md 和最终 manifest/audit 为准。

## 环境与入口

WSL2，Ubuntu 26.04.1 LTS，内核 6.18.40.1-microsoft-standard-WSL2；
Clang 21.1.8，CMake 4.2.3，Ninja 1.13.2，glibc 2.43。
构建位于 /mnt/d/db_project/LTM/ltm-index；普通测试的 log 目录链接到 /var/tmp/ 下
的持久化 ext4 磁盘，具体位置与挂载信息在 storage_location.json。/tmp 为 tmpfs，
未将其用于文件测试。旧 Windows 挂载盘数据保留为 build-tsan-linux/log_drvfs_attempt02。
保留真实文件同步与完整操作量；链路保留数据库位于证据目录。Linux 的检测器耗时
不用于性能对照，独立池扩展性用例仍保留原操作量和原判据。

从 Windows 项目根目录运行：

```powershell
wsl -d Ubuntu --exec bash -lc 'cd /mnt/d/db_project/LTM/ltm-index && python3 scripts/accept_tsan.py'
```

默认生成新的时间戳证据目录，不覆盖历史目录；默认构建 build-tsan-linux，执行全部
19 项普通测试。--scope concurrency 只执行 5 项并发测试，不计作完整验收。
普通测试超时 1800 秒，保留读扩展性用例；不缩减操作量、不关闭检测、不加竞争抑制。
TSAN_OPTIONS 固定为 halt_on_error=1:exitcode=66，数据竞争立即使阶段失败。

## 已有结果

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| 故意竞争对照 | 报 data race，预期退出 66 | attempt_03/control_race.stderr.txt、manifest.json |
| 加锁对照 | 退出 0，无检测报告，结果为 3000 | attempt_03/control_clean.*、manifest.json |
| 严格 Clang 构建 | 全部 49 个编译单元带 -fsanitize=thread 与 -Werror；运行符号含 __tsan_init | attempt_03/compile_commands.json、runtime_symbols.stdout.txt |
| Linux TSAN 普通全量测试 | 19/19，679.39 秒 | attempt_03/ctest.stdout.txt |
| Linux TSAN 守门/链路 | 双树守门与 512 条记录跨进程重开通过，指纹与 Windows 两种构建一致 | attempt_03/ |
| Windows 普通全量 | 19/19，155.82 秒 | windows_verified/normal_ctest.stdout.txt |
| Windows ASAN 全量 | 19/19，220.39 秒；确认真实运行库导入 | windows_verified/asan_ctest.stdout.txt、asan_imports.stdout.txt |
| Windows 结构与链路 | 两种构建的双树守门、512 条记录跨进程重开全过 | windows_verified/ 的 validate、pipeline 记录 |
| Debug 正确性专项 | 5/5，47.59 秒；双树守门与预期锁序 abort 通过 | windows_debug/debug/ |
| 分层、ASCII、Python 协议 | 通过，协议 13/13 | windows_verified/ |
| 强杀/损坏恢复矩阵 | 78/78，其中 72 项实际强杀 | crash_verified/ |
| Debug 连续 C1 压力 | 1800 秒持续负载，2441884 次操作、443124 次淘汰；错误/I6/锁序违反均为 0 | long_soak_debug/ |
| 核心全量独立复核 | 90 个 Linux 源码/入口文件、27 个 Linux 二进制、54 个 Windows 二进制、6 个链路进程及原始证据哈希通过 | attempt_03/audit.json |
| 夹具清理补修复测 | 普通/ASAN/Debug/TSAN 各 HybridDiff 9/9 与两项守门通过，无同步/检测器错误且成功文件清理 | fixture_cleanup_windows/、fixture_cleanup_linux/ |
| 补修组合独立复核 | 仅三份测试变化，核心/静态库/压力程序未变，冻结全量证据与新补验哈希通过 | fixture_cleanup_audit.json |
| 同时启用 ASAN/TSAN | 配置明确拒绝 | sanitizer_conflict/ |

## 本轮修复与范围

保持严格警告设置，修复 LatchManager 前置声明、缺失 algorithm 头文件、有符号索引
的显式转换及字符指纹的无符号字节转换。CRC 硬件探测函数只在该路径可编译时定义；
移除未使用的 R 树辅助函数及测试辅助函数。Config 的有符号参数直接以有符号形式输出。
CMake 显式链接 Threads::Threads，并拒绝同时启用 ASAN 与 TSAN。

TSAN 进一步发现 IndexTree 的 undo 回调重写 root/height 未持结构锁；现与根升降
共用 structure_mutex，即使恢复的是相同值也同步。新增 128 次不存在键删除与 6144
次并发读取回归，rollback_before/ 报真实 data race，rollback_after/ 无检测报告并通过。
三项受影响 TSAN 专项也通过，见 repair_checks/。

旧 Hybrid 恢复夹具在持底层写锁时调用 Hybrid API，违反接口门 → 写锁的顺序，
并析构仍持锁的缓冲池。已改为正常 Put 的页镜像及缺少最后 Commit 的 WAL 前缀，
不将合成现场称为实际强杀；真实强杀另由 78 项矩阵覆盖。
未改变页格式、WAL 协议、树算法、热度同步或接口门；回滚新增结构锁，因此本轮
另补 Debug 锁序与连续压力。attempt_01/ 保留构建失败，attempt_02/ 保留锁序警报
及中止轮次，api_preflight/ 保留真实竞争；windows_final/ 是内核修复前的 Windows
证据，windows_repaired/ 是测试断言编译失败，均不冒称当前源码最终成功。

通过后的检测结论也只覆盖实际执行的接口、配置及负载；原始 pool/disk 和底层树
调用仍需按各自契约同步，对象销毁前须停止调用者。不把有限测试写成任意调度均安全，
不据此整体勾选 M6/M7。

Windows 回归复跑（输出目录必须不存在）：

```powershell
python report/c1/tsan/accept_windows.py report/c1/tsan/windows_repeat
```

核心全量证据完成后已用 verify_acceptance.py 独立核对原始输出、源码及二进制哈希、全部
插桩、五项并发用例、运行库对照、三种构建六个链路子进程的相同指纹和保留数据库。

## 全量验收后的测试夹具清理

原 Linux 日志有测试结束时的 fsync open failed：两个 validator 和 Hybrid 原子恢复用例
在池/磁盘析构前删了文件，POSIX unlink 成功后析构无法重新打开文件同步；Windows
打开文件的删除原本失败。现在成功时先释放所有持有者，再删文件；validator 失败时保留现场。
该补修只改三份 tests/index 文件，核心、页闩、WAL 和压力程序不变。

attempt_03/audit.json 冻结在这项补修之前，不改写旧 manifest 的源码或程序哈希。
accept_fixture_cleanup.py 对普通、ASAN、Debug、Linux TSAN 分别重建三项受影响目标，
跑完整 HybridDiff 9 个用例及两项原操作量 validator，检查无同步/检测器错误、文件已清理。
补验保留新旧程序哈希，任何其他程序或静态库改变都使阶段失败；源码只允许三份测试变更。
当前组合复核使用 verify_fixture_cleanup.py，它保留全量版本的证据哈希并核对当前补验。
旧 verify_acceptance.py 适用于补修前冻结的全量版本；当前源码使用上述组合复核。
上述组合证据不冒称清理补修之后重新执行了一轮全量或 30 分钟压力。

四配置各 9 个 HybridDiff 用例与两项守门均通过，清理后同步错误消失。
fixture_cleanup_audit.json 已独立确认上述源码差异、程序/库未变范围和全部原始证据。
M6-2 约定 C1 门槛已补齐，后续固定操作流混合读写设计见 report/bench/mixed_design.md。
