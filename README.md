# UMVC3 Mod Loader

A mod loader for **Ultimate Marvel vs. Capcom 3** (Steam AppID 357190) that serves mods
directly from their own folders — **without ever copying mod files into the game folder**.

It was built mainly for the [**UMVC3 Community Edition**](https://www.nexusmods.com/ultimatemarvelvscapcom3/mods/286) mod (extra characters, clone
engine, ColorExpansion), which relies on a mix of replaced `nativePCx64\...` files, `.asi`
plugins, and root-level configs like `Characters.ini` — all of which the loader serves
straight from the mod's own folder.

**`src/Loader`** — the `dinput8.dll` proxy injected into the game at startup. A virtual
file system (VFS) redirects the game's file reads to the first enabled mod that provides
the same relative file, and it also loads the mod's `.asi` plugins and any runtime DLLs
they need (e.g. debug CRT). It understands `nativePCx64\...` content, root-level configs
like `Characters.ini`, and INI-profile reads done by mod ASIs.

The only files that ever go into the game folder are our small `dinput8.dll` proxy and a
`mods.ini` manifest. Mod content stays where it is.

## Building Requirements

- Windows 10/11, 64-bit
- Ultimate Marvel vs. Capcom 3 (Steam), `umvc3.exe`
- A MinGW-w64 toolchain to build the loader (see below)

## Building

### 1. The loader (`src\Loader`)

MinHook is vendored under `src\Loader\third_party\minhook`; no download needed.

The build scripts expect the devkitPro MSYS2 MinGW toolchain at
`C:\devkitPro\msys2\opt\bin\x86_64-w64-mingw32-gcc.exe` (and `ar.exe`). If your toolchain
lives elsewhere, edit the `CC`/`AR` paths in the script.

PowerShell:

```powershell
.\src\Loader\build.ps1
```

or CMD:

```cmd
src\Loader\build.cmd
```

Output: `src\Loader\out\dinput8.dll` and `src\Loader\out\UMVC3ModManager.asi`.

## Usage

1. Copy `src\Loader\out\dinput8.dll` **and** `src\Loader\out\UMVC3ModManager.asi` into the
   game folder (`...\steamapps\common\ULTIMATE MARVEL VS. CAPCOM 3\`).
2. Create `mods.ini` next to them (see below), or skip it — the mod manager will create it
   on first launch.
3. Launch the game from Steam (`steam://rungameid/357190` so Steam DRM/achievements keep
   working). The **mod manager** opens *before* the game boots: enable/disable and reorder
   mods, then press **Launch Game** to continue or **Exit** to abort the launch.

To remove the loader, delete `dinput8.dll`, `UMVC3ModManager.asi` and `mods.ini` from the
game folder. Only one `dinput8.dll` proxy may be present at a time.

### Mod manager (`UMVC3ModManager.asi`)

The loader loads this plugin at startup and it shows a small window where you can manage
mods without touching `mods.ini` by hand:

- **Checkbox** next to each mod enables/disables it (written to a `[Disabled]` section).
- **Move Up / Move Down** change a mod's priority. Order *is* the priority: the first mod in
  the list that provides a file wins, so **top of the list is highest priority** (index 1).
- **Refresh** re-scans the game folder's `Mods\` directory for newly added mods.
- **Enable all mods (master switch)** is the same as `Enabled=` in the ini.
- **Show this window on launch** controls the `Manager=` setting. If you uncheck it, hold
  **Shift** while launching the game to force the window open again.
- Mods are auto-discovered from the game folder's `Mods\` directory. If a mod ships a
  `nativePCx64\...` tree, the folder that directly contains `nativePCx64` is used as the mod
  root automatically.
- **Loose-file mods** (a publisher ships the mod file alone, no `nativePCx64` folder) are
  cross-referenced against the game's `nativePCx64` once; the mapping is saved to a
  `[ModFiles]` section in `mods.ini` so the file is served from the mod folder.
- The manager writes `mods.ini` for you when the window closes.

## Mod folder layout

A mod is a folder that mirrors the game root. Anything the game reads from
`<game>\...` is looked up first in each enabled mod in priority order.

```
MyMod\
  nativePCx64\
    boot\
    chr\
    CloneEngine\
    sound\
    ui\
    ...
  Characters.ini        # root-level configs read by ASIs
  ColorExpansion.ini
  mag_patch.asi         # .asi plugins
  CloneEngine.asi
  dinput8.dll           # optional: the mod's own loader
```

Mods can live anywhere on disk — `mods.ini` points the loader at them. Matching is done on
the game-relative path, so `MyMod\nativePCx64\ui\mnchs_en.arc` overrides the game's
`ui\mnchs_en.arc`, and `MyMod\Characters.ini` satisfies an ASI's `.\Characters.ini` read.

## `mods.ini`

Lower numbers win.

```ini
[Loader]
Version=1
Enabled=1
Manager=1

[Mods]
1=C:\path\to\Mods\MyMod
2=C:\path\to\Mods\OtherMod

[Disabled]
1=C:\path\to\Mods\OtherMod
```

Set `Enabled=0` to disable all mods. Index `1` has the highest priority.

The `[Mods]` section lists every known mod; entries in `[Disabled]` are skipped at load
time (that's how the mod manager's checkboxes work). `Manager=0` skips the mod manager GUI
on launch — hold **Shift** at launch to force it open anyway. You normally never edit this
file by hand; the mod manager writes it for you.

The `[ModFiles]` section holds loose-file mappings from the mod manager's cross-referencing:

```ini
[ModFiles]
1=C:\path\to\Mods\LooseMod\file.arc|nativePCx64\ui\file.arc
```

The loader serves the absolute file (left of `|`) whenever the game asks for the
game-relative path (right of `|`).

## Troubleshooting

- **Game still boots without mods:** make sure `dinput8.dll` in the game folder is ours
  (contains the marker `UMVC3-MOD-LOADER-v1`), `mods.ini` lists absolute paths that exist,
  and `Enabled=1`.
- **A different mod manager's `dinput8.dll` is in the game folder:** remove it first; only
  one proxy can be active.
- **Diagnostics:** the loader writes `mods_loader.log` next to the game exe on each run.
