#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bsp_source="$repo_root/.local/read_pico_firmware"
bsp_revision=5f3406deec33304de8801046db583dec7c49e55b

if [ ! -d "$bsp_source" ]; then
    mkdir -p "$repo_root/.local"
    git init -q "$bsp_source"
    git -C "$bsp_source" remote add origin https://github.com/MindReset/read_pico_firmware.git
fi

# A new checkout, or one left at an earlier pin, is moved to the pinned revision.
if [ "$(git -C "$bsp_source" rev-parse --verify HEAD 2>/dev/null || true)" != "$bsp_revision" ]; then
    if [ "$(git -C "$bsp_source" remote get-url origin)" != "https://github.com/MindReset/read_pico_firmware.git" ]; then
        printf '%s\n' "Cannot update a BSP checkout with an unexpected origin." >&2
        exit 1
    fi
    git -C "$bsp_source" fetch --depth 1 origin "$bsp_revision"
    git -C "$bsp_source" checkout --detach "$bsp_revision"
fi

if [ "$(git -C "$bsp_source" rev-parse HEAD)" != "$bsp_revision" ]; then
    printf '%s\n' "BSP revision mismatch: $bsp_source" >&2
    exit 1
fi
git -C "$bsp_source" diff --exit-code HEAD -- components
if [ -n "$(git -C "$bsp_source" ls-files --others --exclude-standard -- components)" ]; then
    printf '%s\n' "BSP components contain untracked files: $bsp_source" >&2
    exit 1
fi
printf '%s\n' "Read Pico BSP: $bsp_revision"
