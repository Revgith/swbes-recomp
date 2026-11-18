# Bring-up notes

## Boot 3 (2026-11-13)
- 570 constructors run; module_start sets up a 19.9 MB heap and starts threads.
- Unimplemented NIDs: 0xC07BB470, 0x623AE665, 0x6AD345D7 (0xF919F628 too).
- `break` at 0x00126844 (game assert), bad read32 at 0xE4800048 from 0x0012843C.
- Final stop: indirect call to 0x8C820008.
- sp-unbalanced warnings in 0x126xxx-0x128xxx: suspect function boundaries.

## Boot 9-11 (2026-11-13): reaches the title screen
- Emitter bug: `jalr` counted as a body terminator, so a split body ending in an
  indirect call never continued to its return address (0x45380 -> 0x45388 skipped
  the shared epilogue: "sp NOT RESTORED ... -32", then a call into stack memory).
  Fixed in emit.c (`is_indirect && !is_call`).
- libc reent: *(k0+4) is a thread record; its +0x37C key is looked up in a per-thread
  reent table. Key 0 matched an empty slot (garbage atexit pointer at 0x3404xxxx).
  module.c now writes key 0x7FFFFFFF so lookups fall back to _impure_ptr.
- "sceIoGetstat() returned error code (0)" is just the game's debug print; harmless.
- New HLE: Output2 audio, GetSystemParamInt/String, RotateThreadReadyQueue,
  __sceSasGetOutputmode, RtcGetCurrentTick, ImposeSetLanguageMode, UMD callback, IoDevctl.
- New VFPU: vcmov, vsrt1, lvl/lvr.q, svl/svr.q.
- PSPRECOMP_TRACE_HLE=1 logs each firmware call at its 1st/10th/100th... use.
- Status: runs in a steady frame loop (~1000 GE lists + flips per 30 s), loads all
  front-end GUIs, title screen shows but blinks and text is garbled.
- Next: framebuffer flicker (SetFrameBuf / double buffering in display.c) and GE
  texture fidelity for fonts (CLUT4/8, swizzle, texture-buffer width).

## Boot 12+ (2026-11-13): rendering bring-up
- Display: the window now shows a frame latched at each vblank (display.c), not live VRAM.
- BIG FIX: emitter passed `vs` as vcst's constant index; the index is bits 20..16.
  All 248 vcst sites loaded 0.0. The camera fov was tan(fov*0)=0, the projection was inf, and
  every 3D vertex was NaN. After the fix the title scene's 3D geometry is submitted and transforms.
- Frame anatomy (title): clear -> skybox (96 quads, infinite proj, translation-free view)
  -> lit textured strips (vtype 1D2, CLUT4 swizzled) -> downsample to 0x04154000 (fbw 120)
  -> blur to 0x04162100 -> composite back onto 0x04000000. Double buffer 0x04000000/0x04088000.
- Current image: very dark, with a blurred figure bottom-left. Suspects, in order:
  1. No near-plane clipping in the software rasterizer: any triangle with a vertex behind the
     camera is dropped (only 13% of verts land on screen), so most of the scene vanishes.
  2. The bloom composite (sprites sampling the 120-wide buffers with blending) is likely
     overwriting the scene with the blurred copy.
- Debug aids: SWBES_SHOT=<vblank,...> dumps shot_N.ppm + both VRAM buffers + camera/vertex
  info into work/run; PSPRECOMP_TRACE_GE=<vblank>; PSPRECOMP_WATCH=<hex addr> (int+float writes);
  PSPRECOMP_TRACE_HLE=1.
- Near-plane clipping added (ge.c draw_clipped): triangles crossing w=0 are cut and drawn
  instead of dropped. The filled area at frame 700 went from ~18k to ~28k pixels.
- PSPRECOMP_SKIP_FBTEX=1 drops passes that sample the screen buffers (bloom). The frame is
  just as dark without them, so post-processing is not what darkens it.
- Frame 700, brightened, shows only faint rotated particle sprites (smoke/green glow), max channel 11.
  The scene's palettes are genuine 8888 grayscale with low values (0x0D..0x4A for common indices);
  texture modulate x vertex colour (~0x73) gives near-black. Still open: whether lighting /
  material colour / doubling / multipass makes this brighter on hardware.
  PPSSPP side-by-side of the same frame is the fastest way to settle it.
