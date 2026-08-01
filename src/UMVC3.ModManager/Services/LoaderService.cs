using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using UMVC3.ModManager.Models;

namespace UMVC3.ModManager.Services;

/// <summary>
/// Deploys the loader proxy into the game folder and writes the active-mod manifest.
/// The only thing ever placed in the game folder is our small dinput8.dll proxy + mods.ini —
/// mod content itself is never copied.
/// </summary>
public sealed class LoaderService
{
    public const string LoaderFileName = "dinput8.dll";
    public const string ConfigFileName = "mods.ini";

    private const string Marker = "UMVC3-MOD-LOADER-v1";

    public bool IsDeployed(string gameDir)
    {
        var loader = Path.Combine(gameDir, LoaderFileName);
        return File.Exists(loader) && IsOurs(loader);
    }

    public bool HasForeignLoader(string gameDir)
    {
        var loader = Path.Combine(gameDir, LoaderFileName);
        return File.Exists(loader) && !IsOurs(loader);
    }

    public static bool IsOurs(string loaderPath)
    {
        try
        {
            using var fs = File.OpenRead(loaderPath);
            using var reader = new BinaryReader(fs);
            var bytes = reader.ReadBytes((int)Math.Min(fs.Length, 1024 * 1024));
            var text = Encoding.ASCII.GetString(bytes);
            return text.Contains(Marker, StringComparison.Ordinal);
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Copies the bundled proxy into the game folder (overwrite).</summary>
    public void Deploy(string gameDir, string bundledLoaderPath)
    {
        Directory.CreateDirectory(gameDir);
        if (!File.Exists(bundledLoaderPath))
            throw new FileNotFoundException("Bundled loader dinput8.dll not found.", bundledLoaderPath);

        File.Copy(bundledLoaderPath, Path.Combine(gameDir, LoaderFileName), overwrite: true);
    }

    /// <summary>Removes the loader proxy if it is ours. Returns false if a foreign one blocks removal.</summary>
    public bool Undeploy(string gameDir)
    {
        var loader = Path.Combine(gameDir, LoaderFileName);
        if (!File.Exists(loader))
        {
            TryDeleteConfig(gameDir);
            return true;
        }

        if (!IsOurs(loader))
            return false;

        try
        {
            File.Delete(loader);
            TryDeleteConfig(gameDir);
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static void TryDeleteConfig(string gameDir)
    {
        try
        {
            var cfg = Path.Combine(gameDir, ConfigFileName);
            if (File.Exists(cfg))
                File.Delete(cfg);
        }
        catch { }
    }

    /// <summary>
    /// Writes mods.ini listing enabled mods in priority order (index 0 = highest priority).
    /// </summary>
    public void WriteConfig(string gameDir, IEnumerable<string> enabledModPaths, bool enabled)
    {
        var sb = new StringBuilder();
        sb.AppendLine("[Loader]");
        sb.AppendLine("Version=1");
        if (enabled)
            sb.AppendLine($"Enabled=1");
        else
            sb.AppendLine("Enabled=0");
        sb.AppendLine();
        sb.AppendLine("[Mods]");

        var order = enabled ? enabledModPaths.ToList() : new List<string>();
        var i = 1;
        foreach (var path in order)
        {
            sb.AppendLine($"{i}={path}");
            i++;
        }

        File.WriteAllText(Path.Combine(gameDir, ConfigFileName), sb.ToString());
    }

    public string DescribeEnabled(IEnumerable<string> enabledModPaths)
    {
        var list = enabledModPaths.ToList();
        if (list.Count == 0)
            return "No mods enabled.";

        return string.Join(", ", list.Select(p => Path.GetFileName(p.TrimEnd(Path.DirectorySeparatorChar))));
    }
}
