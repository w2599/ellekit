#!/usr/bin/env python3
"""在 macOS 上验证 Hook 内存后端初始化和暂停区间约束。"""

from pathlib import Path
import subprocess
import tempfile


# 同时检查 Swift 调用顺序，并编译实际 C 实现测试后端行为。
def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "ellekit/Languages/C/Hook.swift").read_text()
    raw_hook = source[source.index("func rawHook("):]
    prepare = raw_hook.index("EKPrepareHookMemory()")
    suspend = raw_hook.index("let threads = stopAllThreads()")
    write = raw_hook.index("EKHookMemoryRaw(")
    assert prepare < suspend < write, "必须在暂停线程前完成内存后端初始化"
    paused_region = raw_hook[suspend:]
    assert "resumeAllThreads(threads)" in paused_region
    assert "defer {" in paused_region
    for operation in ("print(", "NSLog(", "debugBytes(", "dlsym(", "String("):
        assert operation not in paused_region, f"暂停区间包含可能获取锁的操作: {operation}"

    worker = root / "worker"
    worker.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="hook-memory-test-", dir=worker) as temporary:
        temporary = Path(temporary)
        obj = temporary / "ellekitc.o"
        binary = temporary / "hook-memory-test"
        subprocess.run([
            "xcrun", "clang", "-fblocks", "-include", "dispatch/dispatch.h",
            "-Ddlsym=EKTestDlsym", "-I", str(root / "ellekitc/include"),
            "-c", str(root / "ellekitc/ellekitc.m"), "-o", str(obj),
        ], check=True, timeout=60)
        subprocess.run([
            "xcrun", "clang", "-std=c11", "-fblocks",
            str(root / "test/hook_memory_preparation.c"), str(obj), "-o", str(binary),
        ], check=True, timeout=60)
        for mode in ("hook", "protect", "protect-error", "direct", "override"):
            subprocess.run([str(binary), mode], check=True, timeout=10)
    print("PASS 暂停区间约束与后端兼容性")


if __name__ == "__main__":
    main()
