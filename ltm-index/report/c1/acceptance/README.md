# C1 验收补齐（2026-10-06）

统一入口（从 ltm-index 目录运行）：

```powershell
python scripts/accept_c1.py --mode asan
python scripts/accept_c1.py --mode debug
```

每次先配置并构建，再检查 Ninja 记录的真实头文件依赖，运行普通 ctest 和两个
validate 守门程序。ASAN 跑全部普通 ctest，Debug 跑页闩、并发重放、混合索引和回滚
这 4 项正确性测试，不将长耗时读扩展性测量列入 Debug 门槛。
ASAN 额外确认可执行文件包含运行库导入；Debug 额外运行故意逆序的
独立子进程，要求打印指定的持锁链并以 abort 的预期退出码结束。运行库使用构建驱动
生成的 PATH，无需手工复制 DLL。完整结果见各目录 manifest.json；passed=false 或
缺少结束时间均不可算通过。运行时间以 manifest 中 UTC 为准。

## 新增并发对照

`test_c1_replay` 使用种子 42、2026、8675309 预先生成操作流。
每个种子含 3 个互不相交的写键空间、720 次插入/覆盖/删除操作；3 个读线程共查询
12000 次不可变记录。缓冲池只有 16 帧，断言实际工作集超过池容量，并检查测量期发生缺页。
单线程重放和并发执行分别与独立 map 模型逐条比较键和完整载荷；B+ 的 MBR 维度为 0。
同时检查重叠读发生、调用错误为 0、树不变量、锁序计数和 I6。

该用例覆盖 B+ 页闩并发，不覆盖 HybridIndex 的全部并发 API，也不证明同键冲突的
线性一致性。热度启用配置、共享统计和数据竞争检测仍需后续处理。

## 本次发现并修复的问题

1. **头文件变更未触发重建**：原 Ninja 的依赖前缀缓存为乱码，依赖数为 0。
   MSVC 安装仅有中文语言资源，VSLANG=1033 不足以切换输出。新增编译启动器统一
   include 前缀和 UTF-8 输出；验收入口检查 test_harness.h 是否真的被记录。
2. **I8 反向校验逆序加锁**：R 树 ScanAll 在持叶页闩时回调查询 B+ 根页。
   Debug 报 held pid=2/L0 → requested pid=6/L1，原始失败输出保存于
   debug_before_fix.txt。改为先收集 R 树键、释放扫描页锁，再查询 B+；全程持
   write_mutex 排除双树写入，使校验所见记录集稳定。额外临时内存为 O(R 树条目数)
   个键，校验期间写者需等待。
3. **无人值守 Debug 断言弹窗**：测试框架将断言写到 stderr，并禁用终止确认弹窗；
   断言仍会导致测试失败。stdout 及时刷新，超时日志保留最后运行的用例。

`debug_before_fix.txt` 含人工中止停滞进程的记录，只用于缺陷定位，不能作为通过证据。
仅各模式最终 manifest 和对应输出用于验收。历史的 30 分钟 crabbing 压测位于上级目录，
本轮没有重新运行它，且不能将其推广为 HybridIndex 启用热度后的并发证明。

## 最终结果

2026-10-06：ASAN 普通 ctest 16/16（232.33 秒）、两个结构守门通过；
Debug 正确性专项 4/4（51.28 秒）、两个结构守门、默认逆序 abort 探针通过。
两份最终 manifest 均 passed=true。分层检查 46 文件零违规；ASCII 字面量检查
79 文件零违规。`debug_full_interrupted.txt` 是人工中止扩展性测量的历史运行，
不可记为全量 Debug 通过。新增用例的完整输出已摘存为 `asan/replay.txt` 与
`debug/replay.txt`，包含每个种子的错误数、重叠读次数和实际缺页次数。
