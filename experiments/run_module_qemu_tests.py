#!/usr/bin/env python3
"""One-hart runtime checks for the xv6 module loader test build."""
import os
import select
import subprocess
import sys
import time

QEMU = [
    "qemu-system-riscv64", "-machine", "virt", "-bios", "none",
    "-kernel", "kernel/kernel", "-m", "128M", "-smp", "1",
    "-nographic", "-global", "virtio-mmio.force-legacy=false",
    "-drive", "file=fs.img,if=none,format=raw,id=x0",
    "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
]
TEST_FIXTURES = "--production" not in sys.argv
output = bytearray()
cursor = 0


def read_until(marker, timeout):
    global cursor
    deadline = time.monotonic() + timeout
    while marker not in output[cursor:]:
        left = deadline - time.monotonic()
        if left <= 0:
            raise RuntimeError("timeout waiting for " + repr(marker) +
                               "; serial tail: " +
                               output[-3000:].decode("utf-8", "replace"))
        ready, _, _ = select.select([process.stdout], [], [], left)
        if not ready:
            continue
        chunk = os.read(process.stdout.fileno(), 4096)
        if not chunk:
            raise RuntimeError("QEMU exited; serial tail: " +
                               output[-3000:].decode("utf-8", "replace"))
        output.extend(chunk)
    end = output.index(marker, cursor) + len(marker)
    segment = bytes(output[cursor:end])
    cursor = end
    return segment.decode("utf-8", "replace")


def command(line, must=(), must_not=()):
    global cursor
    process.stdin.write(line.encode() + b"\n")
    process.stdin.flush()
    text = read_until(b"$ ", 15)
    for item in must:
        if item not in text:
            raise RuntimeError(f"{line!r}: expected {item!r}; got:\n{text}")
    for item in must_not:
        if item in text:
            raise RuntimeError(f"{line!r}: unexpected {item!r}; got:\n{text}")
    print("PASS", line)
    return text


process = subprocess.Popen(QEMU, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, bufsize=0)
try:
    boot = read_until(b"$ ", 30)
    if "init: starting sh" not in boot:
        raise RuntimeError("xv6 did not reach its shell: " + boot[-2000:])
    print("PASS one-hart QEMU boot reached xv6 shell")

    command("modctl load missing.o",
            ("cannot read object file", "modctl: load failed"))
    command("modctl load README",
            ("ELF rejected:", "modctl: load failed"))
    command("modctl run absent",
            ("not live", "module unavailable or command failed"))

    if TEST_FIXTURES:
        command("modctl load fail.o",
                ("failtest: init returns failure", "initialization failed",
                 "modctl: load failed"),
                ("FAILTEST ERROR",))
        command("modctl list", ("no modules loaded",))

    command("modctl load hello.o",
            ("hello module initialized through API v2", "module hello loaded"))
    command("modctl load hello.o", ("already loaded", "modctl: load failed"))
    command("modctl run hello alpha beta",
            ("hello module command invoked", "alpha", "beta",
             "command returned 2"))
    command("modctl unload hello",
            ("hello module exiting", "module hello unloaded"))
    command("modctl unload hello", ("not loaded", "modctl: unload failed"))

    command("modctl load sysmon.o", ("sysmon: initialized", "module sysmon loaded"))
    sysmon = command("modctl run sysmon",
                     ("sysmon uptime_ticks=", "sysmon total_pages=",
                      "sysmon free_pages=", "sysmon active_processes="))
    if "sysmon free_pages=0" in sysmon or "sysmon total_pages=0" in sysmon:
        raise RuntimeError("sysmon returned invalid allocator statistics")
    command("modctl unload sysmon", ("sysmon: exit", "module sysmon unloaded"))

    command("modctl load procmon.o",
            ("procmon: initialized", "module procmon loaded"))
    procmon = command("modctl run procmon",
                      ("procmon active_processes=", " state=running ",
                       " name=sh"))
    command("modctl unload procmon", ("procmon: exit", "module procmon unloaded"))

    command("modctl load klogger.o",
            ("klogger: initialized", "module klogger loaded"))
    command("modctl run klogger add WARN first-warning",
            ("klogger: entry added",))
    command("modctl run klogger add ERROR second-error",
            ("klogger: entry added",))
    command("modctl run klogger show",
            ("level=WARN message=first-warning",
             "level=ERROR message=second-error"))
    command("modctl run klogger add BAD invalid",
            ("klogger usage:", "module unavailable or command failed"))

    command("modctl run klogger clear", ("klogger: log cleared",))
    for i in range(35):
        command(f"modctl run klogger add INFO entry-{i:03d}",
                ("klogger: entry added",))
    shown = command("modctl run klogger show",
                    ("klogger entries=32 dropped=3",
                     "message=entry-003", "message=entry-034"),
                    ("message=entry-002",))
    if "message=entry-000" in shown:
        raise RuntimeError("ring did not discard oldest record on overflow")
    command("modctl run klogger clear", ("klogger: log cleared",))
    command("modctl run klogger show", ("klogger entries=0 dropped=0"))
    command("modctl unload klogger", ("klogger: exit", "module klogger unloaded"))

    if TEST_FIXTURES:
        command("modctl load busytest.o",
                ("busytest: initialized", "module busytest loaded"))
        command("modctl run busytest",
                ("busy with 1 active call", "busytest: active unload rejected"))
        command("modctl list", ("busytest state=live active=0",))
        command("modctl unload busytest",
                ("busytest: new call rejected during unload",
                 "busytest: exit", "module busytest unloaded"))
        command("modctl run busytest",
                ("not live", "module unavailable or command failed"))

    command("modctl load sysmon.o", ("module sysmon loaded",))
    command("modctl load procmon.o", ("module procmon loaded",))
    command("modctl load klogger.o", ("module klogger loaded",))
    command("modctl list", ("sysmon state=live", "procmon state=live",
                            "klogger state=live"))
    command("modctl unload sysmon", ("module sysmon unloaded",))
    command("modctl unload procmon", ("module procmon unloaded",))
    command("modctl unload klogger", ("module klogger unloaded",))
    command("modctl list", ("no modules loaded",))
    print("all runtime module checks passed on one QEMU hart")
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
