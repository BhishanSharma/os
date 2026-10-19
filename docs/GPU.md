# The GPU driver (Intel Xe-LP graphics)

Terminal OS drives Intel's integrated graphics (Gen12: Tiger Lake, Alder
Lake, Raptor Lake) itself, in `src/impl/x86_64/drivers/igfx.c`. No firmware
blobs and no GuC: the engines take work through execlists, like Linux's i915
with GuC submission off.

| Part | What it does | State |
| --- | --- | --- |
| Display engine | hardware cursor plane, page flips (no tearing) | works |
| Blitter (BCS) | copies window contents and frames (`XY_FAST_COPY_BLT`) | works |
| Render engine (RCS), stage 1 | a context and ring; boot test stores `0x600DF00D` | new: check the log |
| Shaders, stages 2-3 | GPGPU dispatch of compiled shaders; each checked against the CPU at boot | new: check the log |
| Desktop, stage 4 | frosted glass (blur, saturate, tint, upscale) and fades on the GPU | works |
| Drawing, stage 5 | shadows, round fills and copies, window pictures (blend, scale) on the GPU | new: check the log |

QEMU and VirtualBox do not emulate Intel GPUs: everything here is tested on
real hardware only, through the boot log (`/USB/LOGS`, `scripts/usb-logs.ps1`).
In a virtual machine the driver is not used and the desktop draws on the CPU.

## Reading the boot log

```
igfx: blitter test: ok (wrote 600df00d) ...
igfx: render test: ok (wrote 600df00d) ...          the render engine runs commands
igfx: gpu test fill: ok                             a shader ran and wrote the right pixels
igfx: gpu test box3 across: ok                      ... each shader against the CPU's arithmetic
igfx: gpu test shadow: ok                           ... and the drawing shaders (stage 5)
igfx: gpu: shaders on the render engine (10 of 10 checked)   the desktop uses those that passed
```

`no answer` / `did not finish` lines carry the engine's registers (ring head,
ACTHD: where it stopped; IPEHR: the command it was on). A shader that gives a
`WRONG` pixel prints where and what the CPU expected. Any failure turns the
GPU path off for that shader (a hang: for all of them): the desktop draws
exactly as before.

## Shaders

`gpu/shaders.cl` is OpenCL C, compiled by Intel's own compiler (ocloc, the
Intel Graphics Compiler) into Gen12 machine code, written into
`src/intf/drivers/gpu_shaders.h` (in the repository: building the OS needs no
GPU compiler). To change them:

```powershell
docker build -t myos-gpu toolchain/gpu          # once: ocloc, iga64 (Ubuntu 24.04)
docker run --rm -v "${PWD}:/w" -w /w myos-gpu sh toolchain/gpu/build.sh
```

`build/gpu/shaders/*.asm` has each shader's disassembly. The rules the driver
relies on (`toolchain/gpu/genheader.py` checks them):

- SIMD16, a work group of 16: one hardware thread per group, 16 pixels of a row
  (`x = get_group_id(0) * 16 + get_sub_group_local_id()`, `y = get_group_id(1)`).
- **No arguments**, no local ids: nothing is loaded into registers but r0. A
  shader's parameters are a 64-byte block at `PARAMS + 64 * get_group_id(2)`
  (the driver's `gpu_params_t`); memory is reached by 64-bit addresses through
  the GPU's page tables (stateless A64 messages, no binding tables).
- No scratch space (no spilling).

## How a batch looks

On the render engine's ring, every batch (the context is restore-inhibited,
so nothing is kept between batches):

1. `PIPE_CONTROL` (TLB invalidate: programs' buffers move), then a flush
2. `PIPELINE_SELECT` GPGPU, `STATE_BASE_ADDRESS` (instructions at `VA_CODE`,
   interface descriptors at `VA_STATE`), `MEDIA_VFE_STATE`
3. per pass: `MEDIA_INTERFACE_DESCRIPTOR_LOAD`, `GPGPU_WALKER` (z = the pass's
   parameter block), `MEDIA_STATE_FLUSH`, `PIPE_CONTROL` (CS stall, data-cache
   flush: the next pass reads what this one wrote)
4. `PIPE_CONTROL` writing the batch's number, which the CPU waits for (50 ms at
   most; longer turns the GPU path off)

GPU addresses (one 32-bit PPGTT, shared with the blitter): `VA_BUF1/2` and
`VA_SHADOW` (the blitter's), 16 MiB slots for programs' buffers at
`0x34000000` (eight), the canvas being drawn on at `0x14000000`, code `0x3C000000`, parameters `0x3C100000`, descriptors
`0x3C200000`, self-test pictures `0x3C300000`.

## For programs

`gfx_gpu(struct os_gpu_op *)` (`os.h`, only the program that owns the screen):

- `GPU_MATERIAL`: frosted glass from a quarter-resolution sample picture.
- `GPU_OP_FADE`: blend a saved picture towards the new one.

`gfx_gpu_draw(struct os_gpu_draws *)`: up to 12 drawings onto a canvas (the
window server's frame, or a picture), in order, as one batch: `GD_SHADOW`
(`ui_shadow_ex`), `GD_FILL` (`ui_round_ex`), `GD_ROUND` (`ui_copy_round`),
`GD_OVER` (a picture with alpha, with a hole), `GD_SCALED` (a picture scaled,
with a shape and an opacity). All are the CPU's arithmetic, so the window
server draws the same list on the CPU when the area is small (under 8192
pixels a batch costs more) or the GPU says no. `GD_KEEP`: the pictures are
unchanged since the GPU last read them (no cache flush for them).

It returns 0 when the GPU did it; otherwise the program draws it itself (the
window server keeps the CPU code as the fallback and stops asking after eight
failures in a row).

## How the desktop uses it

- Glass (the Rail, the pill, the Launcher, menus): one batch each: two
  shadows, the rim, the frosted picture, the sheen. The frosted picture is kept
  (eight of them) until the backdrop's samples under it change: hovering, the
  clock, the Launcher's tiles redraw no blur.
- Windows: the shadow, rim and title bar are a picture (made when the focus,
  title or size changes) that the GPU blends under the content: moving a
  window redraws none of it.
- Window motion (open, close, minimise, restore): the window is drawn once
  into a picture; each frame the GPU draws its shadow and scales and fades the
  picture, in one batch.

## Next

- The whole frame as GPU layers (the content too, without the CPU's copy).
