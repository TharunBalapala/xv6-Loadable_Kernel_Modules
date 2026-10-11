#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

riscv_cc="${RISCV_CC:-riscv64-linux-gnu-gcc}"
host_cc="${HOST_CC:-gcc}"
host_extra_flags=()
if [[ "${HOST_SANITIZE:-0}" == 1 ]]; then
  host_extra_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

# Match the xv6 Makefile's freestanding rv64gc / medany options. The
# installed compiler reports LP64D as its default; pass that ABI explicitly.
module_flags=(
  -Wall -Werror -Wno-unknown-attributes -O -fno-omit-frame-pointer
  -march=rv64gc -mabi=lp64d -std=gnu99 -mcmodel=medany
  -ffreestanding -fno-common -nostdlib
  -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen
  -fno-builtin-memset -fno-builtin-memmove -fno-builtin-memcmp
  -fno-builtin-log -fno-builtin-bzero -fno-builtin-strchr
  -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc
  -fno-builtin-free -fno-builtin-memcpy -fno-builtin-printf
  -fno-builtin-fprintf -fno-builtin-vprintf
  -fno-stack-protector -fno-pie -no-pie -msmall-data-limit=0 -I.
  "-ffile-prefix-map=$repo_root=."
)

"$riscv_cc" "${module_flags[@]}" -g0 \
  -fno-asynchronous-unwind-tables -fno-unwind-tables \
  -c -o "$tmp_dir/hello-min.o" experiments/hello_module.c

"$riscv_cc" "${module_flags[@]}" \
  -c -o "$tmp_dir/store-reloc.o" experiments/elfmod_store.S

# Compile the exact parser source for the kernel target without linking it into
# the baseline kernel image. This is a freestanding portability check.
"$riscv_cc" "${module_flags[@]}" -c \
  -o "$tmp_dir/elfmod-riscv.o" kernel/elfmod.c

old_object="$repo_root/experiments/hello_module.o"
if [[ ! -f "$old_object" ]]; then
  "$riscv_cc" "${module_flags[@]}" -ggdb -gdwarf-2 \
    -c -o "$tmp_dir/hello-debug.o" experiments/hello_module.c
  old_object="$tmp_dir/hello-debug.o"
fi

"$host_cc" -Wall -Wextra -Werror -std=gnu99 "${host_extra_flags[@]}" -Ikernel \
  -o "$tmp_dir/test-elfmod" experiments/test_elfmod.c kernel/elfmod.c

"$tmp_dir/test-elfmod" "$tmp_dir/hello-min.o" \
  "$tmp_dir/store-reloc.o" "$old_object"
