using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UMVC3.ModManager.Models;

namespace UMVC3.ModManager.Services;

/// <summary>
/// Scans mod folders in place. A folder is treated as a mod when its contents mirror the
/// game root (nativePCx64\ subtree, or root-level *.asi / dinput8.dll / data archives).
/// </summary>
public sealed class ModLibraryService
{
    public List<ModEntry> ScanLibrary(string libraryDir, IEnumerable<string> extraFolders)
    {
        var result = new List<ModEntry>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        void TryAdd(string dir)
        {
            if (!Directory.Exists(dir))
                return;
            var full = Path.GetFullPath(dir);
            if (!seen.Add(full))
                return;
            if (LooksLikeMod(full))
                result.Add(new ModEntry { FullPath = full });
        }

        if (Directory.Exists(libraryDir))
        {
            foreach (var sub in Directory.EnumerateDirectories(libraryDir))
                TryAdd(sub);
        }

        foreach (var extra in extraFolders)
            TryAdd(extra);

        return result
            .OrderBy(m => m.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    public static bool LooksLikeMod(string dir)
    {
        try
        {
            if (Directory.Exists(Path.Combine(dir, "nativePCx64")))
                return true;

            if (Directory.EnumerateFiles(dir, "*.asi", SearchOption.TopDirectoryOnly).Any())
                return true;

            if (File.Exists(Path.Combine(dir, "dinput8.dll")))
                return true;

            if (Directory.EnumerateFiles(dir, "*.arc", SearchOption.TopDirectoryOnly).Any())
                return true;

            return false;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Creates the default library directory if missing.</summary>
    public void EnsureLibrary(string libraryDir)
    {
        if (!string.IsNullOrWhiteSpace(libraryDir))
            Directory.CreateDirectory(libraryDir);
    }
}
