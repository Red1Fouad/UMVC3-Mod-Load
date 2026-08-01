using System.IO;

namespace UMVC3.ModManager.Models;

/// <summary>
/// A mod: a folder whose contents mirror the game root (nativePCx64\, *.asi, dinput8.dll, *.ini, ...).
/// Mods are referenced in place and never copied into the game folder.
/// </summary>
public sealed class ModEntry
{
    public required string FullPath { get; init; }

    public string Name => Path.GetFileName(FullPath.TrimEnd(Path.DirectorySeparatorChar));

    public bool HasNativePC => Directory.Exists(Path.Combine(FullPath, "nativePCx64"));

    /// <summary>Mod ships its own proxy DLL (like Community Edition) — handled by our loader.</summary>
    public bool HasOwnDInput8 => File.Exists(Path.Combine(FullPath, "dinput8.dll"));

    /// <summary>Mod ships ASI plugins that must be injected into the process.</summary>
    public bool HasAsi => Directory.GetFiles(FullPath, "*.asi", SearchOption.TopDirectoryOnly).Length > 0;

    /// <summary>Mod ships debug CRT runtimes needed by its ASI plugins.</summary>
    public bool HasDebugRuntime =>
        File.Exists(Path.Combine(FullPath, "ucrtbased.dll")) ||
        File.Exists(Path.Combine(FullPath, "msvcp140d.dll")) ||
        File.Exists(Path.Combine(FullPath, "vcruntime140d.dll"));

    public bool UsesHooks => HasOwnDInput8 || HasAsi;

    public long ContentBytes
    {
        get
        {
            try
            {
                var total = 0L;
                foreach (var f in Directory.EnumerateFiles(FullPath, "*", SearchOption.AllDirectories))
                {
                    try { total += new FileInfo(f).Length; } catch { /* ignore locked files */ }
                }
                return total;
            }
            catch
            {
                return 0;
            }
        }
    }

    public int FileCount
    {
        get
        {
            try { return Directory.EnumerateFiles(FullPath, "*", SearchOption.AllDirectories).Count(); }
            catch { return 0; }
        }
    }

    /// <summary>Last-write time of the newest file, used as a light "modified" indicator.</summary>
    public DateTime LastModified
    {
        get
        {
            try
            {
                return Directory.GetLastWriteTime(FullPath);
            }
            catch { return DateTime.MinValue; }
        }
    }

    public string SizeLabel
    {
        get
        {
            var bytes = ContentBytes;
            if (bytes >= 1L << 30) return $"{bytes / (double)(1L << 30):0.0} GB";
            if (bytes >= 1L << 20) return $"{bytes / (double)(1L << 20):0.0} MB";
            if (bytes >= 1L << 10) return $"{bytes / (double)(1L << 10):0.0} KB";
            return $"{bytes} B";
        }
    }
}
