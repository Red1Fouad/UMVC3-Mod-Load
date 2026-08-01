using System.Collections.Generic;

namespace UMVC3.ModManager.Models;

/// <summary>
/// Persistent manager settings (JSON in %AppData%\UMVC3ModLoader\settings.json).
/// </summary>
public sealed class AppSettings
{
    /// <summary>Absolute path to the game install folder (umvc3.exe dir).</summary>
    public string? GameDir { get; set; }

    /// <summary>Folder where mod folders live. Subfolders are scanned as mods.</summary>
    public string? ModLibraryDir { get; set; }

    /// <summary>Extra mod folders added by the user (absolute paths, referenced in place).</summary>
    public List<string> ExtraModFolders { get; set; } = new();

    /// <summary>Enabled mod full paths, in priority order (index 0 = highest priority).</summary>
    public List<string> EnabledMods { get; set; } = new();
}
