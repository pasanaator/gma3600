#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
output=${1:-/tmp/sgx545-compat-probe}
if [[ $# -gt 1 || -e "$output" || -L "$output" ]]; then
    echo "Usage: bash tools/sgx545_compat_probe_build.sh [new-output-file]" >&2
    exit 2
fi
# No CRT, ELF interpreter, libc, or i386 development packages are needed.
"${CC:-cc}" -m32 -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-builtin -fno-stack-protector -fno-pie -nostdlib -static -no-pie \
    -Wl,-e,_start -Wl,-z,noexecstack -Iexperimental/sgx545/abi \
    tools/sgx545_compat_probe.c -o "$output"
printf 'Built freestanding i386 diagnostic client: %s\n' "$output"
