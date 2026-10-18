# ADR-0001: DOOR OS design foundations (Phase 0)

Source of truth for design: `DOOR_OS_Design_and_Architecture_Spec.docx` v0.1 ("Soft Minimal"). Tokens: `design/tokens.json`.
Every statement is labelled **[confirmed]** (read from this repo), **[spec]** (from the DOOR spec), **[proposed]** (my proposal, needs sign-off) or **[unresolved]** (needs a human decision).

## What the repo already gives us

| # | Question (spec section 20) | Answer |
|---|---|---|
| D1 | Display path | **[confirmed]** The kernel hands the desktop a 0x00RRGGBB framebuffer (`GFX_INFO` -> `os_gfx_info`: width, height, `top` = first row below the kernel status bar, text cell size). `GFX_SURFACE` + `GFX_COPY` + `GFX_PRESENT` + `GFX_SYNC` give a GPU-assisted, page-flipped path where available; `GFX_BLIT` is the fallback. The kernel draws the pointer (`GFX_CURSOR`, max 64x64 ARGB). |
| D2 | Scaling | **[confirmed]** Integer scale only: window server sets `S = 2` when `cell_w >= 16`, else 1; apps get `w.scale`. **[proposed]** keep integer scaling (no fractional); all DOOR sizes are multiples of 4 so they stay crisp at 2x. |
| D3 | Float in userspace | **[confirmed]** User programs are built `-mgeneral-regs-only`: no float/SSE. **[proposed]** all token maths is integer (door.h does this). |
| D4 | Compositor model | **[confirmed]** Userspace window server; each window is a shared-memory surface (`ws.h`, 4096-byte header + pixels), damage rectangles, 64-slot event ring per window. This is the compositor/WM layer of the spec; **[proposed]** keep it, no new process. |
| D5 | Input | **[confirmed]** `os_mouse` gives pixel x/y, buttons, wheel. Window server forwards `WS_EV_DOWN/UP/WHEEL/KEY`; `WS_EV_MOVE` is only sent to the capturing window **while a button is held** (windowserver.c ~2066). **[unresolved/needs work]** the spec requires hover states and a 40 px hit model: the server must send hover `WS_EV_MOVE` to the window under the pointer (Step 2). |
| D6 | Process model | **[confirmed]** Static ELF programs, `thread_create` / `pthread_create`, `malloc`, ports + shared memory for IPC. Apps are `apps/NAME/main.c` + `app.cfg`. |
| D7 | Appearance prefs | **[confirmed]** `WS_PREF_DARK/CONTRAST/MOTION/TRANSP` + accent are in every surface header; `WS_EV_PREFS` tells apps to redraw. This already covers the spec's dark/light, high contrast, Reduce Motion, Reduce Transparency. Text scaling is **not** present **[unresolved]**. |
| D8 | Font | **[confirmed]** Inter + JetBrains Mono, both SIL OFL 1.1, pre-rendered to `UIFONT.BIN` at 1x-4x from a 13 px base. Spec sizes (12-32 px) need extra pre-rendered sizes: today only 13 px x scale exists. **[proposed]** add 12/14/16/18/30 px faces in `gen-uifont.py` (Step 3). Spec's font choice is **[unresolved]**: confirm Inter is the DOOR UI font. |
| D9 | Icons | **[confirmed]** `third_party/icons/*.png` are Microsoft **Fluent UI Emoji, 3D style** (MIT). The spec requires an **original geometric 24x24 stroke icon family**. The current set is therefore not DOOR-compliant and is the biggest license/identity risk. **[proposed]** replace with original SVG-like vector icons drawn at runtime (integer rasteriser), Step 4. Until then they are placeholders. |
| D10 | Window chrome | **[confirmed]** `windowserver.c` still implements macOS-style traffic lights, Dock, menu bar, spring motion. Spec forbids these. This is Step 2, not Step 1. |
| D11 | Security boundary | **[confirmed]** Apps are ring-3 and only reach the screen through shm surfaces. **[unresolved]** no per-app permission model exists; spec leaves it open. |

## Decisions

1. **[proposed]** Tokens live in `design/tokens.json`; `user/include/door.h` (numbers) and `color_table` in `user/lib/ui.c` (colours) mirror them by hand until a generator exists. Any change goes through the JSON first.
2. **[proposed]** Keep the existing `UI_C_*` semantic-token API. Existing apps keep compiling; they pick up DOOR colours automatically. New tokens added: `SURFACE1-3`, `BORDER_STRONG`, `ON_ACCENT`, `DANGER`, `SUCCESS`.
3. **[proposed]** Text on the accent uses `UI_C_ON_ACCENT`, never `UI_WHITE` (sand accent is light; white on it fails contrast). `ui_button` and `ui_list` are fixed; any app drawing text over `UI_ACCENT` itself must switch (none found outside `ui.c`).
4. **[owner decision, 2026-10-18]** The default palette is monochrome navy-grey (hue ~215, low saturation, no pure black or white), replacing the spec's warm charcoal and sand accent. Default accent is `slate`. Light values, High Contrast rules and the other accent presets are mine (the spec gives dark values only). All text tokens in light were checked >= 4.5:1 on canvas/surface1/surface2.
5. **[proposed]** `WS_ACCENT_*` renamed to DOOR presets (SLATE default). Saved `DESKTOP.CFG` accent indexes keep their number but change colour; nothing else referenced the old names.
6. **[proposed]** Legacy `UI_R_*` radius names are kept as aliases onto DOOR radii so nothing breaks; new code uses `DOOR_R_*`.

## Contrast notes (measured)

- Dark (navy-grey): primary 10.4-15.0:1, secondary 5.8-8.4:1, accent 6.1-8.9:1 on all surfaces. **textMuted** is 5.1:1 on surface1 but **4.5 on surface2 and 3.8 on surface3**: use only on canvas/surface1, 14 px or larger. **[unresolved]** darken muted slightly, or accept?
- Danger on surface3 is 4.25:1: pair it with an icon/label (spec: no colour-only state) and avoid it as small text on surface3.

## Open decisions for you

1. App name: **Path Explorer vs Path Finder** (spec unresolved; "Finder" also still appears in repo folder names and docs).
2. Confirm Inter is the DOOR UI font (OFL, fine to ship) or choose another.
3. Replace Fluent emoji icons (see D9). Their MIT licence permits use with attribution, so this is an identity decision, not only a legal one. I can't give legal advice or guarantee no IP issues.
4. Approve the light palette and accent presets in `tokens.json`.
5. Text-scaling support (spec accessibility) needs a per-surface pref and larger faces.

## Next steps (not done here)

- Step 2: window server: DOOR frame and controls, launcher replacing the Dock, status area, hover events, task switcher.
- Step 3: type faces for spec sizes.
- Step 4: original icon family.
- Step 5+: toolkit components, then apps one by one.