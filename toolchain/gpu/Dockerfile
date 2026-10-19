# The GPU shader compiler: Intel's offline OpenCL compiler (ocloc, IGC) and
# its disassembler (iga64), for the shaders in gpu/ (docs/GPU.md).
#   docker build -t myos-gpu toolchain/gpu
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        intel-ocloc libigc1 libigdfcl1 libigc-tools python3 make \
    && rm -rf /var/lib/apt/lists/*
