#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_binary=$(mktemp "${TMPDIR:-/tmp}/gma3600-clock-test.XXXXXX")
trap 'rm -f "$test_binary"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
    -Ikernel/gma3600 tests/clock_test.c kernel/gma3600/gma3600_clock.c -o "$test_binary"
"$test_binary"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=undefined \
    -Itests/stubs -Itests -Ikernel/gma3600 -include tests/dpll_stubs.h \
    tests/dpll_test.c kernel/gma3600/gma3600_dpll.c -o "$test_binary"
"$test_binary"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=undefined \
    -Itests/stubs -Itests -Ikernel/gma3600 -include tests/gtt_stubs.h \
    tests/gtt_test.c kernel/gma3600/gma3600_gtt.c -o "$test_binary"
"$test_binary"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=undefined \
    -Itests/stubs -Itests -Ikernel/gma3600 -include tests/memory_stubs.h \
    tests/memory_test.c kernel/gma3600/gma3600_memory.c -o "$test_binary"
"$test_binary"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
    -Iexperimental/sgx545/abi tests/sgx545_abi_test.c \
    tests/sgx545_abi_layout.c experimental/sgx545/abi/sgx545_abi.c -o "$test_binary"
"$test_binary"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
    -Iexperimental/sgx545/abi tests/sgx545_protocol_test.c \
    experimental/sgx545/abi/sgx545_protocol.c -o "$test_binary"
"$test_binary"

"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -fsanitize=undefined -pthread -DGMA3600_SGX_HOST \
    -Itests/stubs -Itests -Ikernel/gma3600 -include tests/irq_stubs.h \
    tests/irq_test.c kernel/gma3600/gma3600_irq.c \
    kernel/gma3600/gma3600_sgx_irq.c -o "$test_binary"
"$test_binary"

"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=undefined \
    -Itests/stubs -Itests -Iexperimental/sgx545/core \
    -include tests/sgx_memory_stubs.h tests/sgx_memory_test.c \
    experimental/sgx545/core/gma3600_sgx_memory.c -o "$test_binary"
"$test_binary"

PYTHONDONTWRITEBYTECODE=1 python3 tests/sgx545_bridge_lifetime_test.py
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
    -Iexperimental/sgx545/abi -Itools tests/sgx545_heap_test.c -o "$test_binary"
"$test_binary"
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_sgx545_*.py' -v
