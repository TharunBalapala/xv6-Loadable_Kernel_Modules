#!/usr/bin/env python3
"""Run the standard xv6 usertests binary in single-hart QEMU."""
import os
import select
import subprocess
import time

qemu = [
    "qemu-system-riscv64", "-machine", "virt", "-bios", "none",
    "-kernel", "kernel/kernel", "-m", "128M", "-smp", "1",
    "-nographic", "-global", "virtio-mmio.force-legacy=false",
    "-drive", "file=fs.img,if=none,format=raw,id=x0",
    "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
]
process = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, bufsize=0)
output = bytearray()


def wait_for(marker, timeout):
    deadline = time.monotonic() + timeout
    while marker not in output:
        left = deadline - time.monotonic()
        if left <= 0:
            raise RuntimeError("timeout waiting for " + repr(marker) +
                               "; serial tail:\n" +
                               output[-6000:].decode("utf-8", "replace"))
        ready, _, _ = select.select([process.stdout], [], [], left)
        if not ready:
            continue
        chunk = os.read(process.stdout.fileno(), 4096)
        if not chunk:
            raise RuntimeError("QEMU exited before " + repr(marker) +
                               "; serial tail:\n" +
                               output[-6000:].decode("utf-8", "replace"))
        output.extend(chunk)


try:
    wait_for(b"$ ", 30)
    process.stdin.write(b"usertests -q\n")
    process.stdin.flush()
    wait_for(b"ALL TESTS PASSED", 240)
    print(output[-3000:].decode("utf-8", "replace"))
    print("standard xv6 usertests passed on one QEMU hart")
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
