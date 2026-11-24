# Changelog

## 0.1.5

- F1 menu: clicks and keys no longer lag or get lost in large windows (#3); the menu draws faster.
- Scoreboard: the Change Team prompt shows the arrow keys, or the controller's d-pad.
- Select A Hero and the campaign: the left / right arrows can be clicked.

## 0.1.4

This is the last update for a while.

- Galactic Conquest: mouse hover and clicks go only to the menu that takes input; Esc on the map opens the conquest menu, as Start does.
- Galactic Conquest: the Navigate and Purchase prompts show the arrow keys, or the controller's d-pad, instead of the PSP d-pad.
- Cheats tab: everyone / half / two become heroes, offer hero to both teams, turbo, launch everyone up, kill soldiers / tanks / flyers / turrets / vehicle riders, heal turrets, +10000 credits.
- Fixed: Endor could freeze with a hero (Cheat.MakeHero).

## 0.1.3

- Vehicles: getting in or out no longer needs a second press.
- Options: Vehicle sensitivity, for ship, tank, turret and walker aim.

## 0.1.2

- Galactic Conquest: the map moves on from Finance to Purchase (a recompiler bug skipped an instruction).
- Galactic Conquest: Enter / A close the popups after a battle instead of opening pause.
- Campaign mission select: hover a mission to select it, click the arrows to step; hovering an arrow no longer shows an empty mission.
- Controller: D-pad Left reloads on foot. An existing settings.ini keeps its old `weapon_action` pad bind; set it to `Left` under `[pad]`.

## 0.1.1

- Campaign tutorial: Enter continues popups, E takes weapons, and weapon swaps are noticed.
- Tutorial and hint prompts show the right PC keys (throttle, boost, grenades, block, autopilot, targeting and more).
- R reloads on foot (new default for `weapon_action`).
- Small walker: up/down aim is no longer inverted, and is faster.

## 0.1.0

First release.

- Native mouse and keyboard for menus, infantry, starfighters, tanks, turrets and walkers.
- Controllers (Xbox, DualSense, DualShock 4) with matching button prompts.
- Rebindable keys and pad buttons (F1 > Controls).
- Higher resolution, uncapped or capped frame rate, anisotropic filtering.
- Mods, texture dumping and a cheats tab.
- Only your .iso is needed: the disc is unpacked and its EBOOT decrypted on first start.
