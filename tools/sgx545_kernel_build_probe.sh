#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Research-only external-module build. Never installs or loads the candidate.
set -euo pipefail
sgx_source=
sgx_kdir=
sgx_output=
while (($#)); do
    case "$1" in
        --source|--kdir|--output)
            if (($# < 2)); then echo "Missing value for $1" >&2; exit 2; fi
            case "$1" in
                --source) sgx_source=$2 ;;
                --kdir) sgx_kdir=$2 ;;
                --output) sgx_output=$2 ;;
            esac
            shift 2 ;;
        -h|--help)
            echo "Usage: $0 --source OPENPVRSGX_CHECKOUT --kdir KERNEL_HEADERS --output NEW_DIRECTORY"
            echo "Builds pinned SGX545/Intel-1.7-ABI research sources; never installs or loads a module."
            exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
if [[ -z "$sgx_source" || -z "$sgx_kdir" || -z "$sgx_output" ]]; then
    echo "--source, --kdir and --output are required" >&2; exit 2
fi
sgx_pin=4000c3fb6577ecad4800e4fa2695777643bfaf86
sgx_subpath=drivers/gpu/drm/pvrsgx/1.17.4948957/eurasia_km
sgx_source=$(realpath "$sgx_source")
sgx_kdir=$(realpath "$sgx_kdir")
sgx_output=$(realpath -m "$sgx_output")
if [[ ! -f "$sgx_kdir/Makefile" ]]; then
    echo "Kernel header Makefile missing" >&2; exit 2
fi
if [[ $(git -C "$sgx_source" rev-parse HEAD) != "$sgx_pin" ]]; then
    echo "Expected openpvrsgx commit $sgx_pin" >&2; exit 2
fi
if [[ -e "$sgx_output" || -L "$sgx_output" ]]; then
    echo "Output must be a new directory; refusing to overwrite $sgx_output" >&2; exit 2
fi
mkdir -p "$sgx_output"
# Require clean tracked sources, then copy only those tracked files. This avoids
# git archive's lazy attribute-object fetches in partial clones and excludes
# ignored build products and untracked local files.
if ! git -C "$sgx_source" diff --no-ext-diff --no-textconv --quiet HEAD -- "$sgx_subpath"; then
    echo "Candidate sources have local changes; refusing an ambiguous build" >&2; exit 2
fi
git -C "$sgx_source" ls-files -z -- "$sgx_subpath" | \
    tar -C "$sgx_source" --null -T - -cf - | tar -xf - -C "$sgx_output"
set +e
make -C "$sgx_kdir" M="$sgx_output/$sgx_subpath" \
    CONFIG_SGX_DRM=y CONFIG_SGX_CEDARVIEW=m CONFIG_SGX_HW_PERF=y \
    CONFIG_SGX_HW_RECOVERY=y CONFIG_SGX_PB_PER_CONTEXT=y \
    CONFIG_PVRSGX_1_7_862890_ABI=y CONFIG_SGX_IOCTL_ALIAS=18 \
    SOC_VENDOR=intel SOC_FAMILY=cedarview_linux SOC=gma3600 SGX=sgx545 \
    SGX_FLAG=SGX545 SGX_REV=10141 TARGET_TYPE=m AM_VERSION=0 \
    -j2 modules 2>&1 | tee "$sgx_output/build.log"
sgx_status=("${PIPESTATUS[@]}")
set -e
printf 'Candidate make exit status: %s\n' "${sgx_status[0]}"
printf 'Research log: %s/build.log\n' "$sgx_output"
if ((sgx_status[0] != 0)); then exit "${sgx_status[0]}"; fi
exit "${sgx_status[1]}"
