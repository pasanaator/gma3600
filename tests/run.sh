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
