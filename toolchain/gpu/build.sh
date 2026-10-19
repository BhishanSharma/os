#!/bin/sh
# build.sh - gpu/shaders.cl into src/intf/drivers/gpu_shaders.h (run in myos-gpu):
#   docker run --rm -v "$PWD:/w" -w /w myos-gpu sh toolchain/gpu/build.sh
# The header is in the repository, so building the OS needs no GPU compiler.
set -e
out=build/gpu/shaders
mkdir -p "$out"
ocloc compile -q -file gpu/shaders.cl -device adl-p \
    -options "-cl-std=CL2.0" -internal_options "-cl-intel-greater-than-4GB-buffer-required" \
    -out_dir "$out" -output shaders > "$out/ocloc.log" 2>&1 || { cat "$out/ocloc.log"; exit 1; }
bin=$(ls "$out"/shaders*.bin | head -1)
python3 toolchain/gpu/genheader.py "$bin" src/intf/drivers/gpu_shaders.h
for k in fill box3 satint upscale fade shadow over rcopy scaled rfill; do
    python3 toolchain/gpu/zebin.py "$bin" .text.$k "$out/$k.isa"
    iga64 -d -p=12p1 "$out/$k.isa" > "$out/$k.asm" 2>/dev/null || true
done
echo "src/intf/drivers/gpu_shaders.h from $bin"
