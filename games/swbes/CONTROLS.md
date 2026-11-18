# SWBES input map

The game reads every action from an input *mapper*: one float per channel,
read with GetValue (0xB73D0, a0 mapper, a1 channel, value at mapper+16[ch]).
Each channel has a *source* object (mapper+8[ch]) saying which PSP button
feeds it and how:

| Source vtable | Meaning |
|---|---|
| 0x41F120 | held button (+16 button id) |
| 0x41F180 | tap (+20: 1 = single, 2 = double tap; fires on release) |
| 0x41F0C0 | analog axis |

Button ids: 0x1CB L, 0x1CC R, 0x1CF up, 0x1D0 down, 0x1D1 left, 0x1D2 right,
0x1C3-0x1C6 face buttons. Each controller holds two mappers, Standard at -0x18
and Alternate at +0 (soldier), or +0 / +0x18 (starfighter). Channels mean the
same in both; only the sources differ. host.c writes the channels directly
from PC binds, so no PC key goes through a PSP button.

Dump a live mapper: `status.txt` has `hum_map` / `flyer_map`, then
`ram.cmd` and `python work/mapdump.py <mapper> [<mapper>...]`.

## Soldier

| Ch | Action | PSP (Alternate) | PC (settings.ini [keys]) |
|---|---|---|---|
| 0-3 | aim up / down / turn | face buttons | mouse |
| 4, 5 | move fwd / right (30, 31 when zoomed) | stick | WASD |
| 7 | crouch / jetpack down (Standard only) | - | `crouch` Ctrl |
| 8 | primary fire | R hold | LMB |
| 9 | secondary fire | down hold | `secondary_fire` G |
| 10 | jump | L tap | `jump` Space |
| 11-14 | roll left / right / fwd / back (Standard) | - | `crouch` + direction |
| 17 | zoom; while zoomed, one scope level in (wraps to unzoomed after the last) | up double tap | RMB (held); RMB + wheel up = level in |
| 18 | camera recenter, not a lock: 0x2251C0 rebuilds the camera from the soldier's flattened forward vector (vtable +312 matrix, +316 position, 0x1262F8, 0x27C704) each frame it is non-zero. No target involved (Standard "(Hold) Target") | - | unbound (useless with mouse look: it fights the mouse) |
| 19 | target search (calls the 0x275C98 targeting routine; Standard down tap) | - | unbound, no visible effect |
| 20 | sprint | L hold | `sprint` Shift |
| 21 | cycle primary weapon | right tap | wheel, `weapon1`/`weapon2` |
| 22 | cycle secondary weapon | left tap | `cycle_secondary` X, `weapon3`-`5` |
| 23 | use / enter vehicle / take weapon | up tap | `use` E (tap) |
| 24 | change equipment (loadout at a command post) | up tap | `equipment` C |
| 27 | weapon action on the primary weapon (0x2CE118, likely reload / melee); blasters here overheat instead | - | unbound |
| 29 | scope one level out (unzooms from level 0); only while zoomed | - | RMB + wheel down (never past level 0) |
| 28 | melee block | up hold | `use` E held |

## Starfighter

| Ch | Action | PSP (Alternate) | PC |
|---|---|---|---|
| 0, 1 | yaw / pitch | face buttons | mouse + WASD |
| 3 | boost | L hold | `jump` Space, or `aim` (RMB / LT, unused in ships) |
| 4, 5 | throttle | stick | Shift / Ctrl |
| 6 | land / get out | up tap | `use` E |
| 7 | climb towards the region's exit (sets ship +724; throttle cancels it). Not the transition | - | unbound |
| 8 | change seat | left tap | `change_seat` V |
| 10 | primary fire | R hold | LMB |
| 11 | secondary fire | down hold | `secondary_fire` G |
| 14 | flip towards the target | L + direction | `stunt` Q |
| 15, 16 | barrel roll | L + left / right | `stunt` Q + A / D |
| 17 | roll correct | right hold | `cycle_target` T held 20+ frames |
| 18 | autopilot to target (tap again for manual); in a transition region ("Press up to transition") the transition cutscene. The PSP's up tap feeds 6 and 18 together | up tap | `autopilot` R, and `use` E |
| 19 | cycle target (one read) | right tap | `cycle_target` T tap (under 20 frames) |

Readers (ra after GetValue): 6@23654C, 9@236604, 2@23664C, 1@236670, 0@236688,
17@236738, 7@236784, 4@2367A8, 5@2367F4, 3@236840, 10@2368D0, 11@236930,
14@236974, 15@236998, 16@2369BC, 18@2369E4, 8@236A20.

## Ground vehicle (tank, speeder)

Update at 0x240A00-0x240B20 (host.c `gnd`).

| Ch | Action | PSP (Alternate) | PSP (Standard) | PC |
|---|---|---|---|---|
| 0, 1 | turret aim | face buttons | R + axis / axis | mouse |
| 2, 3 | drive | stick | stick | WASD (stick) |
| 4 | sets +1164 bit 8 (L hold; effect unconfirmed) | L hold | L hold | `jump` Space, as the ship's boost |
| 6 | read at 240A6C | - | R hold | - |
| 7 | get out | up tap | up tap | `use` E (press) |
| 8 | change seat (event 0x806C) | left tap | circle tap | `change_seat` V (press) |
| 9 | read only while +980 set (passenger path) | - | down tap | - |
| 10 | read, result unused | - | left tap | - |
| 11 | primary fire (+1168 bit 0) | R hold | triangle hold | LMB |
| 12 | secondary fire (+1168 bit 1) | down hold | cross hold | `secondary_fire` G |
| 15 | L tap, no reader in the update | L tap | L tap | - |

## Turret (ion cannon, manned guns)

Update 0x2472B0 (class TURRET, object vtable 0x42C998), controller at entity
+68. PC binds: aim mouse / WASD, `fire` 11, `secondary_fire` 12, `use` 7
(press), `change_seat` 8, `cycle_target` or `target_search` 9, `aim` or
`target_hold` 6 (held), `prev_weapon` 15, `next_weapon` 16. Console `board
<object>` or `SWBES_QM_BOARD=turret` seats the player; some turrets don't
take it (`vehicles` lists them). Verified 2026-11-18: G 12, T 9, RMB 6, V 8,
E 7 (gets out).

| Ch | Reader | Effect |
|---|---|---|
| 0, 1 | 2473C8, 2473B0 | aim (+740, +736) |
| 6 | 247358 | held: target hold (+384 bit 14, as soldier 15) |
| 7 | 2476C0 | get out (event 0x8065) |
| 8 | 24778C | change seat (event 0x806C) |
| 9 | 247838 | target search (0x275C98; with hold, cycles) |
| 11 | 2474E8 | primary fire (+1168 bit 0) |
| 12 | 247658 | secondary fire (+1168 bit 1) |
| 15, 16 | 2477D0, 2477F4 | weapon prev / next (0x266184 -1 / +1) |

## Walker

Update 0x2507A0 (class WALKER), same layout as the turret, shifted:

| Ch | Reader | Effect |
|---|---|---|
| 0 | 250878 | turn (+740) |
| 1 | 25089C | drive (+748) |
| 11 | 2508C0 | pitch (+736; + tilts down) |
| 5 | 250844 | target hold (+384 bit 14) |
| 6 | 2508E4 | primary fire (+1168 bit 0) |
| 7 | 250914 | secondary fire (+1168 bit 1) |
| 9 | 250950 | sets +1288 bit 0 (unconfirmed) |
| 3 | 250980 | get out (0x8065) |
| 4 | 2509C8 | change seat (0x806C) |
| 10 | 250A48 | target search (0x275C98) |

PC binds as the turret's (get out 3, seat 4, target 10, fire 6, secondary 7,
hold 5); W / S (left stick y) drive on 1, A / D and mouse x turn on 0, mouse
y (right stick y) pitches on 11. The Mustafar quick match (`SWBES_QM_MAP=3`)
has one: object vtable 0x42E930, `vehicles` then `board <object>`. Verified
2026-11-18: W drives, D turns, mouse pitches, G 7, T 10, RMB 5, V 4 (to its
gunner seat, a turret), E 3.

## Other readers

- Events: 0x8065 get out (ship 6, tank 7, turret 7, walker 3), 0x806C change
  seat, 0x80B0 upgrade accept.
- Soldier, beyond the table: 15 (214844, 21CCF8) target hold (+384 bit 14,
  Standard R hold); 25 / 26 (22377C, 223764) digital turn axis 26 - 25, read
  instead of 4 / 5 in one movement state when mapper +20 is 0; 8 also at
  22AD78 (charge weapons: +292 bit 4 while held); 10 at 2213B8, 2221E8,
  2239CC, 226294, 226888 (jump / jetpack in other movement states); 2, 3 at
  213D48, 213D24; 24 at 2130E4; 4, 5 at 223738, 223720, 226374, 22633C.
- Ship: 19 at 236A94 (cycle target, honours target hold); 0 / 1 at 236D88 /
  236D68 into +1228 / +1224 (another flight state).
- Debug camera 0x20E888: 3, 4, 5 move (+464 height), 6 toggles controller
  +109, 7 cycles mode +480 (0-3), 8 toggles byte 0x5C9A09, 9 moves to the
  player. Never reached in play.
- Menu mappers: 0xA48A4 / 0xA4ED0 ch 33, 34 (GUI controller array at
  +13876), 0x5388C ch 0, 2.
- Scan: every `lw X, 16(vtable); jalr X` with a constant a1, plus direct
  0xB73D0 / 0x321DC8 calls (152 sites). Those at 0x32xxx, 0x6Exxx, 0x70xxx,
  0x115C54, 0x1DAxxx-0x1DCxxx, 0x26AA38, 0x27B730, 0x28BB70, 0x2AAFB8,
  0x330124, 0x3945xx are other classes' slot 16 (stream reads of size 4,
  etc.), not mappers.

## Upgrade offer

"You can now upgrade your flyer. E803 Accept or Decline E804" has its own
controller (object +36, GetValue 0x321DC8, which calls 0xB73D0 for it, so
the real caller is at sp+0, not sp+8). While it shows (+76 > 0, state 3) the
game reads ch 3 at 0x1E8934 (accept: event 0x80B0) and ch 4 at 0x1E89C4
(decline); 0x1E8A00 / 0x1E8A24 read the same pair in another state.
`hero_accept` Y / `hero_decline` N write them, one read per press (the hero
offer itself is player +272). Verified by forcing it: poke object +76 to 5.0
(`SWBES_VEHDBG` logs the object as "upg obj").

Stunts: `stunt` Q takes its direction from A / D or either stick's x (the
right stick steers a ship): left 15, right 16, none 14 (flip).

## Fire

`fire` (LMB, pad RT) is written straight to soldier 8, ship 10 and ground
vehicle 11, turret 11 and walker 6. Anywhere else (other seats) host.c's `fire_r` tells the
window to pass the PSP's R instead (`psp_fire_r`), from the same bind. Before,
LMB was hard-wired to R, so rebinding fire only moved it in ships.

## Optional binds (default None)

`recenter_camera` (soldier 18, held), `target_search` (19, one read per
press), `weapon_action` (27, one read per press), `sprint_toggle` (20: a press starts
sprinting until you stop moving, press it again, or hold `sprint`).
`prev_weapon` (turret 15), `target_hold` (turret 6, walker 5; `aim` too). `cycle_secondary` (22) is one read per press, as the PSP's left tap. Any [keys] / [pad] value
`None` leaves an action unbound.

Left out: soldier 15 (Standard R hold, no Alternate source); ship 2 (no
source), 7 (climb: only read right after 6, land / get out, fires), 9
(Standard R hold); tank 6, 9, 10 (Standard only), 15 (no reader).

## Rule

One PSP button may feed several channels (soldier up tap: 23 and 24; ship up
tap: 6 and 18). Before wiring a PC bind, dump the mapper (`SWBES_MAPDUMP=1`:
`mapdump <mapper>[*] ch N <source>` per channel, `*` = active scheme, and
`mapread m ch ra` per reader) and write every channel the PSP button feeds,
unless another bind already covers it (soldier 24 is `equipment` C).

Check a bind: `SWBES_CHLOG=1` logs `ch <mapper> <n> on/off <value> ra <reader>`
for every channel after the PC writes. Verified 2026-11-17: on foot Space 10,
Shift 20, Ctrl 7, C 24, G 9, E 23 (held 28), X 22 (1-5 call the weapon switch
directly); ship Space 3, Shift/Ctrl 4, G 11, T 19, V 8, Q 14, R 18, E 6 + 18
(18 once per press, as the PSP tap); tank G 12, V 8. A tap channel must get
one read per press, not one per frame held.

Sources also seen: 0x41F100 combo of two sources (+16, +20), 0x41F160 stick
direction, 0x41F140 menu mapper. `SWBES_AUTOUSE=1` presses use at the first
transition prompt (test hook). Turret (ion cannon) mapper: not dumped yet.

## Rebinding

Every key is a bind in `settings.ini` `[keys]`; the file is re-read while the
game runs, and prompts, Controls pages and menu icons redraw with it.

In game: F1 (or pad Back + Start) > Controls lists every bind. Enter / click /
pad A on one, then press a key or mouse button (wheel for Next weapon); Esc or
pad B cancels. Reserved keys are refused there, duplicates show in amber, and
Default keys resets them all. Esc itself can't be captured (it cancels), so
`menu_back` returns to Esc through Default keys or the file. host.c
`binds_menu` registers the list (labels, flags) with window.c `psp_binds_set`.

| Group | Binds (default) |
|---|---|
| Moving | `forward` W, `back` S, `left` A, `right` D, `jump` Space, `sprint` Shift, `crouch` Ctrl |
| Weapons | `fire` LMB, `aim` RMB, `next_weapon` Wheel, `secondary_fire` G, `cycle_secondary` X, `weapon1`-`5` 1-5 |
| Actions | `use` E, `equipment` C, `change_seat` V, `cycle_target` T, `stunt` Q, `autopilot` R, `hero_accept` Y, `hero_decline` N |
| Menus | `pause` P, `select` Tab, `menu_confirm` Enter, `menu_back` Esc, `menu_tab_left` Q, `menu_tab_right` E, `menu_option_1` R (Square), `menu_option_2` F (Triangle), `menu_up` / `down` / `left` / `right` arrows |

Key names: a letter or digit, Space, Shift, Ctrl, Alt, Tab, Enter, Esc, Bksp,
Caps, Up, Down, Left, Right, F1-F12, LMB, RMB, Mouse3-5, or a hex VK (`0x..`).
`Wheel` works for `next_weapon` only. F1 (PC menu) is refused for every bind,
and Space / Backspace for menu binds other than `menu_confirm` / `menu_back`
(the default is used). A key bound to two actions of the same mode (play, or
menus) is a warning but kept. These, and unknown key names, are logged; F1 > Controls shows them. The mouse itself always aims, and
Backspace and Space keep their fixed menu meanings (back / select). The
window layer's PSP-button keys are remapped through `psp_key_remap`.

## Prompt glyphs

Hint text carries action codes (U+E8xx), drawn as PSP icons. host.c rewrites
the ones with a PC bind to `[Key]`: E848 shoot, E84A jump, E854 sprint,
E855 switch weapon, E85A crouch, E857 use (or equipment when the text says
"equipment"), E858/E887/E8C6/E8D2 use, E8C4 take off (jump), E888 change seat, and in the hero offer ("Accept or
Decline") E803 / E804 (d-pad left / right) become `hero_accept` / `hero_decline`.

## Hero offer

"You can now play as X" is player +272 == 2 (player = 0xD19EC id looked up
in the 0x5CA1CC table). PSP d-pad left writes 3 (accept), right 4 (decline);
on PC `hero_accept` Y / `hero_decline` N write the same, only while it is 2.
Console `hero [n]` shows / sets the state (a forced `hero 1` offer shows the
prompt but the server never resolves 3 / 4 for it, d-pad or key alike).

## Controls pages

Options > Controls > Infantry / Vehicle / Starfighter draw lone button
glyphs (U+EA00-EA1F) beside each label. While one of those lists is shown
(7D6 widget ids AD91E01C, DCEA980C, EFE2E42E, plus their hidden twins), host.c
blanks the glyphs and prefixes each known label with its PC key.

## Menu notes

- "In a menu" (Esc = back, play binds off) = a shown focusable button, a
  slider / toggle (0x7D3 / 0x7D2) or a Back / Select prompt bar (0x7D1 nav 0).
  Advanced Controls has only a slider and a toggle; it used to read as play.
- Toggles (Control Scheme, Auto-Level, Invert Y) change with Enter, not arrows.
- Debug quick match straight into a space ship (no spawn screen) leaves Start
  dead; a normal spawn pauses fine.
- Ground vehicles (0x2409D4, `chans 3` dumps the mapper): 0 / 1 turn (mouse),
  2 / 3 stick, 6 target lock, 7 get out (up tap, `use` E), 8 left tap,
  11 R hold, 12 down hold (secondary fire / missiles, `secondary_fire` G), 4 L hold. V not wired yet.
- Scopes: weapon +312 = level count, +332 = current level, +316[] = magnification
  per level (0x212F04). RMB + wheel shows 2.0 -> 4.0 on the default rifle.
- Controls pages are labelled under both schemes (Alternate and Default).
  The scheme toggle does not persist across a restart.

## Controller (XInput, DualSense, DualShock 4)

In play the pad drives the same direct actions as keyboard and mouse
(host.c `g_pad_of`); in the game's menus it is a PSP pad (A/Cross selects,
B/Circle backs out, d-pad / left stick move).

Every action is rebindable in settings.ini `[pad]` and on the F1 Controls
page's second column: select a cell, press a button (Esc cancels, right
click clears). Names: A B X Y LB RB LT RT LS RS Up Down Left Right PaddleL
PaddleR FnL FnR Start Back None. Movement binds (default None) add to the
left stick; the menu binds drive the game's own menus. Back + Start always
opens the PC menu, which keeps fixed pad navigation. The table below is the
default.

| Pad (Xbox / PlayStation) | On foot | Ground vehicle | Starfighter |
|---|---|---|---|
| Left stick | move (analog) | drive (PSP stick) | throttle (PSP stick) |
| Right stick | aim (rate, curved; Stick sensitivity) | aim | steer (turn-rate channels 0/1) |
| RT / R2 | fire | fire | fire |
| LT / L2 | zoom (hold) | zoom | - |
| A / Cross | jump | boost | boost |
| B / Circle | crouch (+ stick: roll) | | |
| X / Square | use (hold: melee block) | leave | land / leave |
| Y / Triangle | next primary weapon | | |
| LB / L1 | secondary fire | secondary fire | secondary fire |
| RB / R1 | cycle secondary | | |
| LS / L3 | sprint | | |
| RS / R3 | | | stunt (+ stick x: barrel roll) |
| D-pad up | change equipment | | |
| D-pad down | change seat | change seat | change seat |
| D-pad left | accept hero | | autopilot |
| D-pad right | decline hero | | cycle target |
| Start / Options | pause | pause | pause |
| Back + Start | PC settings (d-pad / stick, A select, B close) | | |

Turret / walker: right stick aims, RT fire, LB secondary, LT target hold, X
get out, d-pad down change seat, d-pad right target search, Y next weapon,
RB previous weapon (also `prev_weapon` if bound). Verified 2026-11-18 in a
turret (fake pad): LB, right, RB, Y, down, X; in the walker: left stick
drives, right stick turns / pitches, LB, right, LT, RT, down, X.

On a PlayStation pad the touchpad click is Select too (as is Create), and
Select prompts show the touchpad. A click also reports its half
(`PSP_PAD_TOUCH_L` / `_R`, from the first finger's x in the HID touch data,
Linux hid-playstation layout), so the halves can be bound apart; for now
both are Select. The PS button is left to the system.

Prompts show pad button icons (Xbox or PlayStation style, by the pad) while
the pad was used last: hints, menus and the Controls pages. The icons are
drawn at load in the game's own icon style (navy fill, dark rim, cyan ring and
labels; smooth, supersampled, shapes as SDFs; bumpers, triggers, Menu / View
and d-pads shaped like each pad's own) into an atlas of our own (96 px
cells, 7 to a row), and get their own slots in the sub-image registry
(host.c, `pad_glyph` / `glyph_hash_hook`); new text codes 0xEA20+ name them.
Every read button has one, including PS / Guide, touchpad halves, mute,
Edge Fn and back paddles (nothing binds those yet).

### Own textures

The game binds textures by handle: 500 object pointers at 0x54324C, bound
by 0x13BF90 / 0x13C1A0 (which copies the texels into its VRAM cache).
Object: +0 u16 w, +2 u16 h, +6 GE format, +7 mip levels - 1, +8 texels,
+12 CLUT, +16 VRAM cache slot (-1 none), +20 cached size. Registry entry
+12 is the handle a sub-image draws with. host.c `pgt_install` puts an
object of ours at handle 499: 256 x 256, 4-bit, every texel one magic CLUT
colour. The renderer passes each decoded texture to `psp_tex_post`, which
swaps that one for the atlas at any size (UVs are normalised). Keep the
stand-in at 256: sprite UVs reach the GE as whole texels, and a 16 px
stand-in cropped the cells. Test hook:
`pad.cmd` = `<hex buttons> lx ly rx ry [lt rt [ps]]` (ps 1 = PlayStation pad), `off` releases.
