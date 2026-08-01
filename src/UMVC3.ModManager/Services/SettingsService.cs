using System;
using System.IO;
using System.Text.Json;
using UMVC3.ModManager.Models;

namespace UMVC3.ModManager.Services;

public sealed class SettingsService
{
    private static readonly string AppDataDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "UMVC3ModLoader");

    private static readonly string SettingsPath = Path.Combine(AppDataDir, "settings.json");

    public AppSettings Load()
    {
        try
        {
            if (File.Exists(SettingsPath))
            {
                var json = File.ReadAllText(SettingsPath);
                var s = JsonSerializer.Deserialize<AppSettings>(json);
                if (s is not null)
                    return s;
            }
        }
        catch
        {
            // Corrupt settings — start fresh rather than crash.
        }

        return new AppSettings();
    }

    public void Save(AppSettings settings)
    {
        Directory.CreateDirectory(AppDataDir);
        var json = JsonSerializer.Serialize(settings, new JsonSerializerOptions { WriteIndented = true });
        File.WriteAllText(SettingsPath, json);
    }

    public static string DefaultModLibraryDir =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "UMVC3 Mod Loader", "Mods");
}
