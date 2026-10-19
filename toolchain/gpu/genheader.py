#!/usr/bin/env python3
# genheader.py SHADERS.bin OUT.h - the kernels of an ocloc output (zebin) as
# C arrays for the driver, with what it must check: SIMD16, no arguments
# read from the payload, no scratch space, no per-thread data.
import re, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
from zebin import sections

secs = sections(open(sys.argv[1], 'rb').read())
info = secs['.ze_info'].decode()
kernels = []
for block in re.split(r'\n  - name:\s+', info)[1:]:
    name = block.split('\n', 1)[0].strip()
    if name.startswith('kernels_misc_info') or '.text.' + name not in secs:
        continue
    simd = int(re.search(r'simd_size:\s+(\d+)', block).group(1))
    grf = int(re.search(r'grf_count:\s+(\d+)', block).group(1))
    if simd != 16:
        sys.exit(f'{name}: SIMD{simd} (the driver dispatches SIMD16)')
    if 'per_thread_payload_arguments' in block.split('kernels_misc_info')[0]:
        sys.exit(f'{name}: needs per-thread data (local ids): use get_group_id/get_sub_group_local_id')
    if re.search(r'arg_type:\s+arg_by', block):
        sys.exit(f'{name}: has arguments: read them from the PARAMS block instead')
    if re.search(r'(scratch|private_memory|spill)\w*:\s+[1-9]', block):
        sys.exit(f'{name}: needs scratch space (spills)')
    kernels.append((name, secs['.text.' + name], grf))

with open(sys.argv[2], 'w', newline='\n') as f:
    f.write('/* shaders.h - made by toolchain/gpu/build.sh from gpu/shaders.cl: do not edit.\n'
            ' * Gen12 (Xe-LP) machine code from Intel\'s compiler (ocloc), SIMD16. */\n')
    f.write('#ifndef GPU_SHADERS_H\n#define GPU_SHADERS_H\n\n#include <stdint.h>\n\n')
    f.write('enum { ' + ', '.join(f'GPU_{n.upper()}' for n, _, _ in kernels) + ', GPU_KERNELS };\n\n')
    for n, code, grf in kernels:
        f.write(f'static const uint8_t gpu_{n}_isa[{len(code)}] __attribute__((aligned(64))) = {{')
        for i, b in enumerate(code):
            f.write(('\n    ' if i % 16 == 0 else ' ') + f'0x{b:02x},')
        f.write('\n};\n\n')
    f.write('static const struct { const char *name; const uint8_t *isa; uint32_t size; int grf; } gpu_kernels[GPU_KERNELS] = {\n')
    for n, code, grf in kernels:
        f.write(f'    {{ "{n}", gpu_{n}_isa, {len(code)}, {grf} }},\n')
    f.write('};\n\n#endif\n')
