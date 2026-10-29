#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本机构建驱动（Ninja + 显式 MSVC/SDK 环境），零硬编码路径。

它当初是为另一台机器写的，那台机器上 `scripts/build.bat` 完全走不通：
  1. 项目路径含非 ASCII 字符，而 cmd.exe 与 bash 在命令行上传非 ASCII 路径时
     都会走一次本地代码页往返，于是 `pushd "%~dp0.."` 直接失败；
     Python 用 CreateProcessW 传参数，不做任何编码转换。
  2. 那台机器的 Visual Studio 装在 `D:\\VS产品`，旧构建缓存指向已不存在的路径。
  3. 那台机器安全策略禁止 `reg.exe` 启动，而 VS 生成器与 `vcvars64.bat` 都靠它
     定位 VS 实例与 Windows SDK，结果是一个"能跑但找不到 crtdbg.h/stddef.h/stdio.h"
     的编译器。这里因此改成：Ninja 生成器 + 自己拼 INCLUDE/LIB/PATH（全靠目录扫描）。

**当前机器（项目已迁回）这三条都不成立**：路径是纯 ASCII、VS 在
`D:\\visual studio\\IDE`、`reg.exe` 可用，`scripts\\build.bat` 同样能跑。
本脚本仍作为推荐入口保留 —— 它不依赖注册表，且一条命令能在
RelWithDebInfo / 故障注入 / ASAN / Debug 四种配置间切换。

为了让它在当前机器上可用，`find_vs_root()` 已改为向下多探一层子目录：
本机的 VS 在 `D:\\visual studio\\IDE\\` 下多一层，只扫盘顶层目录会扫不到。

用法：
    python scripts/build_local.py <configure|build|test|all|fi|asan|debug|clean> [build-dir] [config]

退出码：0 = 成功；非 0 = 失败（与 cmake/ctest 的退出码一致）。
"""
import os
import shutil
import subprocess
import sys

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CL_REL = os.path.join("VC", "Tools", "MSVC")
CMAKE_REL = os.path.join("Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin")
NINJA_REL = os.path.join("Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "Ninja")


def drives():
    for letter in "CDEFGHIJ":
        root = letter + ":\\"
        if os.path.isdir(root):
            yield root


def find_vs_root():
    """扫各盘顶层目录（及其一层子目录），找同时具备 cl.exe 与 cmake.exe 的那一个。

    下探这一层是必需的：Visual Studio 的安装目录不一定是盘的顶层目录。
    本机就是 `D:\\visual studio\\IDE\\` —— 顶层 `D:\\visual studio` 只是个壳，
    `VC/` 与 `Common7/` 都在多一层的 `IDE\\` 下面，只看顶层会扫不到它。
    """
    for root in drives():
        try:
            entries = [e.path for e in os.scandir(root) if e.is_dir()]
        except OSError:
            continue
        candidates = list(entries)
        for path in entries:
            try:
                candidates.extend(e.path for e in os.scandir(path) if e.is_dir())
            except OSError:
                continue
        for path in candidates:
            msvc = os.path.join(path, CL_REL)
            cmake = os.path.join(path, CMAKE_REL, "cmake.exe")
            if os.path.isdir(msvc) and os.path.isfile(cmake):
                return path
    return None


def newest_dir(path):
    try:
        names = [e.name for e in os.scandir(path) if e.is_dir()]
    except OSError:
        return None
    return sorted(names)[-1] if names else None


def find_sdk_root():
    """Windows SDK 根：<盘>\\Windows Kits\\10 或 Program Files (x86) 下的默认位置。"""
    candidates = []
    for root in drives():
        candidates.append(os.path.join(root, "Windows Kits", "10"))
        candidates.append(os.path.join(root, "Program Files (x86)", "Windows Kits", "10"))
        candidates.append(os.path.join(root, "Program Files", "Windows Kits", "10"))
    for path in candidates:
        if os.path.isdir(os.path.join(path, "Include")):
            return path
    return None


def build_env():
    vs = find_vs_root()
    sdk = find_sdk_root()
    if vs is None:
        raise SystemExit("cannot find a Visual Studio root with cl.exe and cmake.exe")
    if sdk is None:
        raise SystemExit("cannot find a Windows 10 SDK root with an Include directory")
    msvc_ver = newest_dir(os.path.join(vs, CL_REL))
    sdk_ver = newest_dir(os.path.join(sdk, "Include"))
    msvc = os.path.join(vs, CL_REL, msvc_ver)
    cmake = os.path.join(vs, CMAKE_REL, "cmake.exe")
    ninja = os.path.join(vs, NINJA_REL, "ninja.exe")
    env = dict(os.environ)
    env["VSLANG"] = "1033"  # Stable ASCII /showIncludes diagnostics for Ninja.
    env["PATH"] = ";".join([
        os.path.join(msvc, "bin", "Hostx64", "x64"),
        os.path.join(sdk, "bin", sdk_ver, "x64"),
        env.get("PATH", ""),
    ])
    env["INCLUDE"] = ";".join([
        os.path.join(msvc, "include"),
        os.path.join(sdk, "Include", sdk_ver, "ucrt"),
        os.path.join(sdk, "Include", sdk_ver, "shared"),
        os.path.join(sdk, "Include", sdk_ver, "um"),
    ])
    env["LIB"] = ";".join([
        os.path.join(msvc, "lib", "x64"),
        os.path.join(sdk, "Lib", sdk_ver, "ucrt", "x64"),
        os.path.join(sdk, "Lib", sdk_ver, "um", "x64"),
    ])
    print("toolchain: vs=%s msvc=%s sdk=%s(%s)" % (vs, msvc_ver, sdk, sdk_ver))
    return env, cmake, ninja


def run(argv, env):
    print(">> " + " ".join(argv))
    r = subprocess.run(argv, shell=False, env=env, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.stdout:
        print(r.stdout, end="" if r.stdout.endswith("\n") else "\n")
    if r.stderr:
        print(r.stderr, end="" if r.stderr.endswith("\n") else "\n")
    print("   exit=%d" % r.returncode)
    return r.returncode


def main():
    action = sys.argv[1] if len(sys.argv) > 1 else "all"
    build_name = sys.argv[2] if len(sys.argv) > 2 else "build-ninja"
    config = sys.argv[3] if len(sys.argv) > 3 else "RelWithDebInfo"
    extra = []
    if action == "fi":
        build_name, action, extra = "build-fi", "all", ["-DLTM_ENABLE_FAULT_INJECT=ON"]
    elif action == "debug":
        build_name, config, action = "build-dbg", "Debug", "all"
    elif action == "asan":
        build_name, action, extra = "build-asan", "all", ["-DLTM_ENABLE_ASAN=ON"]
    build_dir = os.path.join(PROJECT_ROOT, build_name)

    if action == "clean":
        for d in ("build-ninja", "build-fi", "build-dbg", "build-asan"):
            shutil.rmtree(os.path.join(PROJECT_ROOT, d), ignore_errors=True)
        print("cleaned build-ninja/ build-fi/ build-dbg/ build-asan/")
        return 0

    env, cmake, ninja = build_env()
    if action in ("configure", "all"):
        rc = run([cmake, "-S", PROJECT_ROOT, "-B", build_dir, "-G", "Ninja",
                  "-DCMAKE_MAKE_PROGRAM=" + ninja, "-DCMAKE_BUILD_TYPE=" + config,
                  "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"] + extra, env)
        if rc != 0:
            return rc
    if action in ("build", "all"):
        rc = run([cmake, "--build", build_dir], env)
        if rc != 0:
            return rc
    if action in ("test", "all"):
        ctest = os.path.join(os.path.dirname(cmake), "ctest.exe")
        return run([cmake, "-E", "chdir", build_dir, ctest, "-E", "validate",
                    "--output-on-failure"], env)
    return 0


if __name__ == "__main__":
    sys.exit(main())
