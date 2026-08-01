using System;
using System.Diagnostics;
using System.IO;
using UMVC3.ModManager.Services;

namespace UMVC3.ModManager.Services;

/// <summary>
/// Launches the game through Steam (keeps DRM/achievements working) using the
/// steam://rungameid protocol, or directly when Steam is unavailable.
/// </summary>
public sealed class GameLauncher
{
    public const int GameAppId = SteamGameLocator.GameAppId;

    public bool LaunchViaSteam()
    {
        try
        {
            // ShellExecute on the steam:// URI hands off to the Steam client, which
            // launches the game with the correct working directory and DRM context.
            Process.Start(new ProcessStartInfo
            {
                FileName = $"steam://rungameid/{GameAppId}",
                UseShellExecute = true,
            });
            return true;
        }
        catch
        {
            return false;
        }
    }

    public bool LaunchDirect(string gameDir)
    {
        var exe = Path.Combine(gameDir, SteamGameLocator.GameExe);
        if (!File.Exists(exe))
            return false;

        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = exe,
                WorkingDirectory = gameDir,
                UseShellExecute = true,
            });
            return true;
        }
        catch
        {
            return false;
        }
    }
}
