# Modding

Mods live in `mods\` next to where the game runs (the folder is created on first start).
Each mod is its own folder. To manage mods, press **F1** and choose **Mods**:

- switch each mod on or off,
- turn on **Dump textures**,
- open the mods folder.

```
mods\
  my-mod\
    mod.ini                       name, author, ... (optional)
    disc\...                      files that replace files on the game disc
    textures\<16 hex>.png         images that replace game textures
```

## mod.ini

```ini
[mod]
name=My Mod
author=you
version=1.0
description=One line shown on the Mods page.
```

Every key is optional. If there is no `name`, the folder name is used. `on=0` makes the mod start switched off until it is turned on in F1 → Mods.

## Replacing disc files

A file at `mods\my-mod\disc\<path>` is loaded instead of `disc0:/<path>`. For example, `disc\PSP_GAME\USRDIR\data\foo.lvl` replaces that file.

The game opens most files only while loading, so changes show up on the next load.

## Replacing textures

1. Open F1 → Mods, turn **Dump textures** on, then play through the part you want to change.
   - Every texture the game decodes is saved once to `dump\textures\<hash>.png`.
2. Copy the PNGs you want into `mods\my-mod\textures\`, keeping each file name, and edit them.
   - A replacement can be any size: a 2× or 4× image just looks sharper.
   - Keep the alpha channel.
3. Turn the mod on. Textures are swapped live, with no restart needed.

The hash covers the texture's pixels and palette. A texture that the game draws with two different palettes gets two dumps.

## Load order

Mods load in folder-name order, and **a later mod wins** when two mods replace the same file or texture. Prefix the folder names to set the order, for example `10-base`, `20-hud`.

On/off state is saved in `settings.ini` under `[mods]`.
