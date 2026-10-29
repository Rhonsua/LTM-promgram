cmake_minimum_required(VERSION 3.20)
project(ltm_index LANGUAGES CXX C)

# ---------------------------------------------------------------------------
# 通用 Larger-than-Memory 树形索引结构
# 构建约定见 docs/architecture.md；分层硬约束由 scripts/check_layering.py 强制
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# 所有目标统一输出到 build/ 根目录，脚本无需猜测路径
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR})
endforeach()

option(LTM_BUILD_TESTS          "构建测试"                 ON)
option(LTM_ENABLE_ASAN          "启用 AddressSanitizer"    OFF)
option(LTM_ENABLE_TSAN          "启用 ThreadSanitizer"     OFF)
option(LTM_ENABLE_FAULT_INJECT  "启用故障注入(崩溃测试)"   OFF)
option(LTM_WERROR               "把警告当作错误"           ON)

# ---------------------------------------------------------------------------
# 编译选项
#
# 关键：add_compile_options() 只作用于"其后创建"的目标，而 add_compile_definitions()
#       作用于整个目录。本段最初被写在所有 add_library/add_executable 之后，
#       结果 /utf-8 没有生效，MSVC 按 GBK(936) 解析 UTF-8 源码，把一个中文注释
#       拆成半个字符而吞掉换行，报出成百上千条伪语法错误。
#       教训：编译选项段落必须位于第一个目标之前。
# ---------------------------------------------------------------------------
if(MSVC)
  # Normalize /showIncludes before Ninja sees it. VSLANG alone cannot switch
  # a compiler installation that only contains Chinese language resources.
  if(CMAKE_GENERATOR MATCHES "Ninja")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${Python3_EXECUTABLE};${CMAKE_CURRENT_SOURCE_DIR}/scripts/msvc_compile.py")
    set(CMAKE_C_COMPILER_LAUNCHER "${Python3_EXECUTABLE};${CMAKE_CURRENT_SOURCE_DIR}/scripts/msvc_compile.py")
  endif()
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
  if(LTM_WERROR)
    add_compile_options(/WX)
  endif()
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  # 骨架阶段的确定性抑制项：
  #   C4127 常量条件表达式
  #   C4702 不可达代码（NotImplemented 占位实现里 "终止 + 返回" 的固定形态，
  #         M1..M6 逐个实到位后这些警告会自然消失，届时应删除本行）
  add_compile_options(/wd4127 /wd4702)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter)
  if(LTM_WERROR)
    add_compile_options(-Werror)
  endif()
endif()

# Sanitizer 开关。
#
# 教训（本项的写法曾直接导致一条验收门槛从未被真正执行）：
#   原写法是 `if(LTM_ENABLE_ASAN AND NOT MSVC)`，且**没有 else 分支**。
#   本工程唯一的构建器就是 MSVC，于是 `-DLTM_ENABLE_ASAN=ON` 在这里**静默地
#   什么都不做** —— 命令成功、配置成功、测试全绿，但那不是 ASAN 下的绿。
#   方案把「ASAN 全绿」写成 M3 门槛的一部分，M3 却从未在 ASAN 下跑过。
# 规则：开关与平台不匹配时必须显式报错，绝不能静默降级成"没开"。
#
# MSVC 只支持 ASAN（VS2019 16.9+），不支持 TSAN；TSAN 走 GCC/Clang。
if(LTM_ENABLE_ASAN AND LTM_ENABLE_TSAN)
  message(FATAL_ERROR "ASAN and TSAN require separate builds")
endif()
if(LTM_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/fsanitize=address)
    message(STATUS "AddressSanitizer 已启用（MSVC /fsanitize=address）")
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
    message(STATUS "AddressSanitizer 已启用（-fsanitize=address）")
  endif()
endif()
if(LTM_ENABLE_TSAN)
  if(MSVC)
    message(FATAL_ERROR
      "LTM_ENABLE_TSAN=ON 但当前构建器是 MSVC，而 MSVC 不支持 ThreadSanitizer。"
      "请在 Linux 下用 GCC/Clang 构建；若只想查内存错误，请改用 -DLTM_ENABLE_ASAN=ON。")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
  message(STATUS "ThreadSanitizer 已启用（-fsanitize=thread）")
endif()

if(LTM_ENABLE_FAULT_INJECT)
  add_compile_definitions(LTM_ENABLE_FAULT_INJECTION=1)
  message(STATUS "故障注入已启用：仅用于 tests/crash_matrix")
else()
  add_compile_definitions(LTM_ENABLE_FAULT_INJECTION=0)
endif()

# ---------------------------------------------------------------------------
# 核心静态库：src/util + src/storage
# ---------------------------------------------------------------------------
add_library(ltm_core STATIC
  src/util/status.cc
  src/util/log.cc
  src/util/crc32c.cc
  src/util/config.cc
  src/storage/page.cc
  src/storage/eviction_policy.cc
  src/storage/disk_manager.cc
  src/storage/buffer_pool.cc
  src/storage/wal.cc
  src/storage/recovery.cc
  src/storage/fault_inject.cc
  # C1：页闩表由 BufferPool 持有（"闩保护的是池里的帧"），所以这个编译单元
  # 必须落在**低层**库里 —— 若留在 ltm_index，buffer_pool.cc（在 ltm_core）
  # 引用 LatchManager 就会形成 ltm_core → ltm_index → ltm_core 的链接环。
  # 文件仍留在 src/concurrency/（方案 §4.10 把并发模型放在那里），只是编进 ltm_core。
  src/concurrency/latch_manager.cc
)

find_package(Threads REQUIRED)
target_link_libraries(ltm_core PUBLIC Threads::Threads)

target_include_directories(ltm_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---------------------------------------------------------------------------
# 索引库：src/index + src/migration + src/concurrency
# 依赖规则：本库只允许依赖 ltm_core 与 util。禁止出现文件 I/O（分层检查强制）
# ---------------------------------------------------------------------------
add_library(ltm_index STATIC
  src/index/index_entry.cc
  src/index/tree_ops.cc
  src/index/bplus_tree.cc
  src/index/rtree.cc
  src/index/mock_tree.cc
  src/index/hybrid_index.cc
  src/index/index_registry.cc
  src/migration/hot_cold_tracker.cc
  src/migration/migration_policy.cc
  # 注意：src/concurrency/latch_manager.cc 已移到 ltm_core（见上面那段注释）
)
target_link_libraries(ltm_index PUBLIC ltm_core)

# ---------------------------------------------------------------------------
# 可执行入口
# ---------------------------------------------------------------------------
add_executable(ltm_smoke  tests/harness/smoke_main.cc)
target_link_libraries(ltm_smoke PRIVATE ltm_index)

add_executable(ltm_demo   demo/demo_main.cc)
target_link_libraries(ltm_demo PRIVATE ltm_index)

add_executable(ltm_pipeline demo/pipeline_main.cc)
target_link_libraries(ltm_pipeline PRIVATE ltm_index)

add_executable(ltm_bench  src/bench/bench_main.cc src/bench/hybrid_bench.cc src/bench/mixed_bench.cc)
target_link_libraries(ltm_bench PRIVATE ltm_index)

# ---------------------------------------------------------------------------
# 里程碑验收入口（方案 §5.2 的"守门"程序）
#   退出码约定：0 = 通过；1 = 校验失败；2 = 尚未实现
#   scripts/validate_tree.py 负责调用并解析这几种退出码
# ---------------------------------------------------------------------------
add_executable(ltm_validate_bplus tests/index/test_bplus_validate.cc)
# C0 边界的诊断探针：故意在小池子上并发放大缺页，红是它的预期输出。
# 故意**不**注册进 LTM_TEST_SOURCES（见文件头）。
add_executable(c0_boundary_probe tools/c0_boundary_probe.cc)
# C1 验收的第三件：30 分钟混合读写压测驱动（方案 §6 的"无死锁无挂起"）。
# 同样**不**注册进 LTM_TEST_SOURCES：它是 30 分钟量级的工具，不是单元用例；
# 而且它今天绿只是 C0 的基线，不是 C1 的成绩（见文件头）。
add_executable(soak_mixed tools/soak_mixed.cc)
target_link_libraries(ltm_validate_bplus PRIVATE ltm_index)
target_link_libraries(c0_boundary_probe PRIVATE ltm_index)
target_link_libraries(soak_mixed PRIVATE ltm_index)

add_executable(ltm_validate_rtree tests/index/test_rtree_validate.cc)
target_link_libraries(ltm_validate_rtree PRIVATE ltm_index)

# ---------------------------------------------------------------------------
# 崩溃矩阵的执行程序（方案 §5.2 的守门程序 2，由 scripts/crash_matrix.py 驱动）
#
# 只在开启故障注入的构建里存在：它必须能真的把进程强杀掉，否则整张矩阵会变成
# "全绿但什么都没测到"（本项目缺陷 #37 就是这个形状）。构建方式：
#     python scripts/build_local.py fi      （或 scripts\build.bat faultinject）
# ---------------------------------------------------------------------------
if(LTM_ENABLE_FAULT_INJECT)
  add_executable(ltm_crash_test tests/crash/crash_test.cc)
  target_link_libraries(ltm_crash_test PRIVATE ltm_index)
  message(STATUS "崩溃矩阵执行程序：ltm_crash_test（仅故障注入构建）")
endif()


# ---------------------------------------------------------------------------
# 测试：ltm_test_<module>，由 ctest 驱动
# ---------------------------------------------------------------------------
if(LTM_BUILD_TESTS)
  enable_testing()
  set(LTM_TEST_TIMEOUT 300)
  if(LTM_ENABLE_TSAN)
    # Instrumented concurrency tests can take substantially longer than native
    # runs. Keep a bounded timeout without dropping the read-scaling tests.
    set(LTM_TEST_TIMEOUT 1800)
  endif()
  set(LTM_TEST_SOURCES
    util:test_status
    util:test_crc32c
    util:test_config
    storage:test_page
    storage:test_recovery
    storage:test_metadata
    index:test_registry
    index:test_tree_ops_contract
    index:test_wal_barrier
    index:test_bplus_diff
    index:test_rtree_diff
    index:test_hybrid_diff
    bench:test_spatial_reference
    bench:test_mixed_reference
    concurrency:test_c0_latch
    concurrency:test_page_latch
    concurrency:test_c1_replay
    concurrency:test_hybrid_concurrency
    concurrency:test_write_abort
    integration:test_smoke
  )
  foreach(entry IN LISTS LTM_TEST_SOURCES)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 grp)
    list(GET parts 1 name)
    add_executable(${name} tests/${grp}/${name}.cc)
    target_link_libraries(${name} PRIVATE ltm_index)
    # 测试源文件用 #include "harness/test_harness.h"，需要项目根目录在搜索路径上
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${LTM_TEST_TIMEOUT})
  endforeach()

  # 不变量自检注册为独立测试；未实现时以退出码 2 失败（这是期望行为，
  # 实现完成后自然转绿）。用 ctest -R validate 单独跑。
  add_test(NAME validate_bplus COMMAND ltm_validate_bplus)
  add_test(NAME validate_rtree COMMAND ltm_validate_rtree)
  # 退出码约定：0 = 通过，1 = 校验失败，2 = 尚未实现。
  # 实现完成前这两项会以退出码 2 失败 —— 这是期望状态：
  #   期望通过项：ctest -E validate
  #   单独看不变量自检：python scripts/validate_tree.py
  set_tests_properties(validate_bplus validate_rtree PROPERTIES TIMEOUT 900)
endif()

# ---------------------------------------------------------------------------
# 汇总信息
# ---------------------------------------------------------------------------
message(STATUS "==============================================")
message(STATUS " ltm_index 构建配置")
message(STATUS "   编译器      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "   构建类型    : ${CMAKE_BUILD_TYPE}")
message(STATUS "   C++ 标准    : ${CMAKE_CXX_STANDARD}")
message(STATUS "   测试        : ${LTM_BUILD_TESTS}")
message(STATUS "   Werror      : ${LTM_WERROR}")
message(STATUS "   故障注入    : ${LTM_ENABLE_FAULT_INJECT}")
message(STATUS "   输出目录    : ${CMAKE_BINARY_DIR}")
message(STATUS "==============================================")
