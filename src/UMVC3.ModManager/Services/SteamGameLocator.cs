using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Microsoft.Win32;

namespace UMVC3.ModManager.Services;

/// <summary>
/// Locates the game install folder using the Steam registry uninstall key for AppID 357190,
/// with a libraryfolders.vdf fallback.
/// </summary>
public sealed class SteamGameLocator
{
    public const int GameAppId = 357190;
    public const string GameExe = "umvc3.exe";

    public static string GameDisplayName => "Ultimate Marvel vs. Capcom 3";

    /// <summary>Resolves the game folder, returning null when Steam says it isn't installed.</summary>
    public string? Locate()
    {
        var fromRegistry = FromRegistry();
        if (fromRegistry is not null)
            return fromRegistry;

        var fromVdf = FromLibraryFolders();
        if (fromVdf is not null)
            return fromVdf;

        return null;
    }

    private static string? FromRegistry()
    {
        try
        {
            var keyPath = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App " + GameAppId;
            using var key = Registry.LocalMachine.OpenSubKey(keyPath)
                             ?? Registry.CurrentUser.OpenSubKey(keyPath);
            var install = key?.GetValue("InstallLocation") as string;
            if (!string.IsNullOrWhiteSpace(install) && Directory.Exists(Path.Combine(install, GameExe)))
            {
                return Path.GetFullPath(install);
            }

            // Some machines also expose it through the 32-bit view of the registry.
            using var key32 = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32)
                .OpenSubKey(keyPath);
            var install32 = key32?.GetValue("InstallLocation") as string;
            if (!string.IsNullOrWhiteSpace(install32) && Directory.Exists(Path.Combine(install32, GameExe)))
            {
                return Path.GetFullPath(install32);
            }
        }
        catch
        {
            // Registry reads can fail in locked-down environments; fall through.
        }

        return null;
    }

    private static string? FromLibraryFolders()
    {
        var steamRoot = FindSteamRoot();
        if (steamRoot is null)
            return null;

        var libraries = new List<string> { Path.Combine(steamRoot, "steamapps") };
        var vdf = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
        if (File.Exists(vdf))
        {
            foreach (var line in File.ReadAllLines(vdf))
            {
                var m = System.Text.RegularExpressions.Regex.Match(line, "\"\\d+\"\\s+\"(.+?)\"");
                if (m.Success)
                {
                    var lib = m.Groups[1].Value.Replace("\\\\", "\\");
                    libraries.Add(Path.Combine(lib, "steamapps"));
                }
            }
        }

        foreach (var lib in libraries)
        {
            var candidate = Path.Combine(lib, "common", "ULTIMATE MARVEL VS. CAPCOM 3");
            if (Directory.Exists(Path.Combine(candidate, GameExe)))
                return Path.GetFullPath(candidate);
        }

        return null;
    }

    private static string? FindSteamRoot()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
            var path = key?.GetValue("SteamPath") as string;
            if (!string.IsNullOrWhiteSpace(path) && Directory.Exists(path))
                return path;
        }
        catch { }

        foreach (var candidate in new[]
                 {
                     @"C:\Program Files (x86)\Steam",
                     @"C:\Program Files\Steam",
                 })
        {
            if (Directory.Exists(candidate))
                return candidate;
        }

        return null;
    }
}
