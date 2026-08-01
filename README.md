# UMVC3 Mod Loader

A mod loader for **Ultimate Marvel vs. Capcom 3** (Steam AppID 357190) that serves mods
directly from their own folders — **without ever copying mod files into the game folder**.

- **`src/Loader`** — the `dinput8.dll` proxy injected into the game at startup. A virtual
  file system (VFS) redirects the game's file reads to the first enabled mod that provides
  the same relative file, and it also loads the mod's `.asi` plugins and any runtime DLLs
  they need (e.g. debug CRT). It understands `nativePCx64\...` content, root-level configs
  like `Characters.ini`, and INI-profile reads done by mod ASIs.
- **`src/UMVC3.ModManager`** — a small Avalonia desktop app to detect the Steam install,
  pick/enable/reorder mods, deploy the loader, and launch the game.

The only files that ever go into the game folder are our small `dinput8.dll` proxy and a
`mods.ini` manifest. Mod content stays where it is.

## Requirements

- Windows 10/11, 64-bit
- Ultimate Marvel vs. Capcom 3 (Steam), `umvc3.exe`
- To **build the loader**: a MinGW-w64 cross/toolchain (see below)
- To **build the manager**: .NET SDK 9.0+

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

Output: `src\Loader\out\dinput8.dll`.

### 2. The manager (`src\UMVC3.ModManager`)

Build the loader first — the manager bundles the resulting `dinput8.dll` as a resource.

```powershell
dotnet build src\UMVC3.ModManager
```

or publish a self-contained exe:

```powershell
dotnet publish src\UMVC3.ModManager -c Release
```

## Usage

### Via the manager (recommended)

1. Launch `UMVC3.ModManager`.
2. **Game folder** is auto-detected from Steam (registry + `libraryfolders.vdf`); if wrong,
   pick the folder containing `umvc3.exe` manually.
3. Set a **mod library** (a folder that contains mod folders). Default:
   `Documents\UMVC3 Mod Loader\Mods`. The folder is created if missing.
4. Click **Add mod folder** for any mod outside the library (e.g. Fluffy manager mods).
5. Enable the mods you want and reorder them with **Up/Down** (top = highest priority).
6. Click **Deploy loader** to install `dinput8.dll` + `mods.ini` into the game folder.
7. Click **Launch game** (goes through `steam://rungameid/357190` so Steam DRM/achievements
   keep working).

### Manually

1. Copy `src\Loader\out\dinput8.dll` into the game folder
   (`...\steamapps\common\ULTIMATE MARVEL VS. CAPCOM 3\`).
2. Create `mods.ini` next to it (see below).
3. Launch the game from Steam.

### Removing

Undeploy via the manager, or delete `dinput8.dll` and `mods.ini` from the game folder.
The manager refuses to overwrite a `dinput8.dll` that isn't ours (e.g. from another mod
manager).

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

Written by the manager; lower numbers win.

```ini
[Loader]
Version=1
Enabled=1

[Mods]
1=C:\path\to\Mods\MyMod
2=C:\path\to\Mods\OtherMod
```

Set `Enabled=0` to disable all mods. Index `1` has the highest priority.

## Troubleshooting

- **Game still boots without mods:** make sure `dinput8.dll` in the game folder is ours
  (contains the marker `UMVC3-MOD-LOADER-v1`), `mods.ini` lists absolute paths that exist,
  and `Enabled=1`.
- **A different mod manager's `dinput8.dll` is in the game folder:** remove it first; only
  one proxy can be active.
- **Diagnostics:** the loader writes `mods_loader.log` next to the game exe on each run.
