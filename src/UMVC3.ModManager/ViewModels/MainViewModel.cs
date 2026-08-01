using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using UMVC3.ModManager.Models;
using UMVC3.ModManager.Services;

namespace UMVC3.ModManager.ViewModels;

public partial class MainViewModel : ViewModelBase
{
    private readonly SettingsService _settingsService = new();
    private readonly SteamGameLocator _locator = new();
    private readonly ModLibraryService _library = new();
    private readonly LoaderService _loader = new();
    private readonly GameLauncher _launcher = new();

    private AppSettings _settings;

    public MainViewModel()
    {
        _settings = _settingsService.Load();
        _gameDir = _settings.GameDir ?? _locator.Locate() ?? "";
        _modLibraryDir = _settings.ModLibraryDir ?? SettingsService.DefaultModLibraryDir;

        RefreshMods();
        RefreshDeployState();
    }

    private IStorageProvider? _storageProvider;
    private Window? _window;

    public void Attach(Window window)
    {
        _window = window;
        _storageProvider = window.StorageProvider;
    }

    public ObservableCollection<ModEntryViewModel> Mods { get; } = new();

    public IEnumerable<string> EnabledModPaths => Mods.Where(m => m.IsEnabled).Select(m => m.FullPath);

    [ObservableProperty]
    private string _gameDir = "";

    [ObservableProperty]
    private string _modLibraryDir = "";

    [ObservableProperty]
    private bool _loaderDeployed;

    [ObservableProperty]
    private bool _foreignLoaderPresent;

    [ObservableProperty]
    private string _status = "";

    [ObservableProperty]
    private string _loaderHint = "";

    [ObservableProperty]
    private int _enabledCount;

    [ObservableProperty]
    private string _gameFound = "";

    partial void OnGameDirChanged(string value)
    {
        GameFound = string.IsNullOrWhiteSpace(value)
            ? "Game not found — pick the folder containing umvc3.exe"
            : $"Game: {value}";
        RefreshDeployState();
    }

    [RelayCommand]
    private void Refresh()
    {
        RefreshMods();
        RefreshDeployState();
        Status = "Refreshed.";
    }

    private void RefreshMods()
    {
        _settings.GameDir = GameDir;
        _settings.ModLibraryDir = ModLibraryDir;
        _library.EnsureLibrary(ModLibraryDir);

        var enabledPaths = new HashSet<string>(
            _settings.EnabledMods.Where(p => Directory.Exists(p)),
            StringComparer.OrdinalIgnoreCase);

        var scanned = _library.ScanLibrary(ModLibraryDir, _settings.ExtraModFolders);

        // Preserve current toggle state from the UI where possible.
        var currentState = Mods.ToDictionary(m => m.FullPath, m => m.IsEnabled, StringComparer.OrdinalIgnoreCase);

        Mods.Clear();
        foreach (var mod in scanned)
        {
            var isEnabled = currentState.TryGetValue(mod.FullPath, out var cur)
                ? cur
                : enabledPaths.Contains(mod.FullPath);
            Mods.Add(new ModEntryViewModel(mod, isEnabled));
        }

        ReindexPriorities();
        _settingsService.Save(_settings);
    }

    /// <summary>Recomputes 1-based priority slots from the persisted enabled order.</summary>
    private void ReindexPriorities()
    {
        var order = new List<string>(
            _settings.EnabledMods.Where(p => Mods.Any(m => string.Equals(m.FullPath, p, StringComparison.OrdinalIgnoreCase))));

        foreach (var mod in Mods)
        {
            var enabled = mod.IsEnabled;
            if (enabled && !order.Contains(mod.FullPath, StringComparer.OrdinalIgnoreCase))
            {
                order.Add(mod.FullPath);
                mod.Priority = order.Count;
            }
            else if (enabled)
            {
                mod.Priority = order.FindIndex(p => string.Equals(p, mod.FullPath, StringComparison.OrdinalIgnoreCase)) + 1;
            }
            else
            {
                mod.Priority = 0;
            }
        }

        _settings.EnabledMods = order;
        EnabledCount = Mods.Count(m => m.IsEnabled);
    }

    // Re-index when an individual mod's enabled state changes.
    public void NotifyModToggled()
    {
        ReindexPriorities();
        _settingsService.Save(_settings);
    }

    [RelayCommand]
    private void MoveUp(ModEntryViewModel? mod)
    {
        if (mod is null || !mod.IsEnabled)
            return;
        Move(mod, -1);
    }

    [RelayCommand]
    private void MoveDown(ModEntryViewModel? mod)
    {
        if (mod is null || !mod.IsEnabled)
            return;
        Move(mod, +1);
    }

    private void Move(ModEntryViewModel mod, int direction)
    {
        var list = _settings.EnabledMods;
        var idx = list.FindIndex(p => string.Equals(p, mod.FullPath, StringComparison.OrdinalIgnoreCase));
        var target = idx + direction;
        if (idx < 0 || target < 0 || target >= list.Count)
            return;

        (list[idx], list[target]) = (list[target], list[idx]);
        ReindexPriorities();
        _settingsService.Save(_settings);
    }

    [RelayCommand]
    private async Task PickGameFolder()
    {
        var picked = await PickFolderAsync("Select the folder containing umvc3.exe");
        if (picked is null)
            return;

        if (!File.Exists(Path.Combine(picked, SteamGameLocator.GameExe)))
        {
            Status = "That folder does not contain umvc3.exe.";
            return;
        }

        GameDir = picked;
        _settings.GameDir = GameDir;
        _settingsService.Save(_settings);
        Status = "Game folder set.";
    }

    [RelayCommand]
    private async Task PickLibraryFolder()
    {
        var picked = await PickFolderAsync("Select a folder that contains mod folders");
        if (picked is null)
            return;

        ModLibraryDir = picked;
        _settings.ModLibraryDir = ModLibraryDir;
        _settingsService.Save(_settings);
        RefreshMods();
        Status = "Mod library set.";
    }

    [RelayCommand]
    private async Task AddModFolder()
    {
        var picked = await PickFolderAsync("Select a mod folder (e.g. ...\\Mods\\MyMod)");
        if (picked is null)
            return;

        if (!ModLibraryService.LooksLikeMod(picked))
        {
            Status = "That folder does not look like a UMvC3 mod (no nativePCx64, *.asi or dinput8.dll).";
            return;
        }

        _settings.ExtraModFolders.Add(picked);
        _settingsService.Save(_settings);
        RefreshMods();
        Status = "Added mod folder: " + Path.GetFileName(picked);
    }

    [RelayCommand]
    private void RemoveMod(ModEntryViewModel? mod)
    {
        if (mod is null)
            return;

        _settings.ExtraModFolders.RemoveAll(p => string.Equals(p, mod.FullPath, StringComparison.OrdinalIgnoreCase));
        _settings.EnabledMods.RemoveAll(p => string.Equals(p, mod.FullPath, StringComparison.OrdinalIgnoreCase));
        _settingsService.Save(_settings);
        RefreshMods();
        Status = "Removed from list: " + mod.Name;
    }

    [RelayCommand]
    private void DeployLoader()
    {
        if (string.IsNullOrWhiteSpace(GameDir))
        {
            Status = "Set the game folder first.";
            return;
        }

        if (_loader.HasForeignLoader(GameDir))
        {
            Status = "A different dinput8.dll is already in the game folder (e.g. from another mod manager).";
            RefreshDeployState();
            return;
        }

        try
        {
            var bundled = Path.Combine(AppContext.BaseDirectory, "Resources", LoaderService.LoaderFileName);
            _loader.Deploy(GameDir, bundled);
            _loader.WriteConfig(GameDir, EnabledModPaths, enabled: true);
            RefreshDeployState();
            Status = "Loader deployed. Launch the game to apply enabled mods.";
        }
        catch (Exception ex)
        {
            Status = "Deploy failed: " + ex.Message;
        }
    }

    [RelayCommand]
    private void UndeployLoader()
    {
        if (string.IsNullOrWhiteSpace(GameDir))
            return;

        if (_loader.Undeploy(GameDir))
        {
            RefreshDeployState();
            Status = "Loader removed from the game folder.";
        }
        else
        {
            Status = "Could not remove the loader (foreign dinput8.dll present or file locked).";
        }
    }

    [RelayCommand]
    private void LaunchGame()
    {
        if (string.IsNullOrWhiteSpace(GameDir))
        {
            Status = "Set the game folder first.";
            return;
        }

        // Always write the current mod set right before launch so the loader sees the latest state.
        if (_loader.IsDeployed(GameDir))
        {
            try
            {
                _loader.WriteConfig(GameDir, EnabledModPaths, enabled: true);
            }
            catch (Exception ex)
            {
                Status = "Could not write mods.ini: " + ex.Message;
                return;
            }
        }

        var ok = _launcher.LaunchViaSteam() || _launcher.LaunchDirect(GameDir);
        Status = ok ? "Launching game…" : "Launch failed.";
    }

    [RelayCommand]
    private void ApplyConfig()
    {
        if (string.IsNullOrWhiteSpace(GameDir))
        {
            Status = "Set the game folder first.";
            return;
        }

        if (!_loader.IsDeployed(GameDir))
        {
            Status = "Deploy the loader first.";
            return;
        }

        _loader.WriteConfig(GameDir, EnabledModPaths, enabled: true);
        ReindexPriorities();
        _settingsService.Save(_settings);
        Status = "Configuration applied: " + _loader.DescribeEnabled(EnabledModPaths);
    }

    private void RefreshDeployState()
    {
        if (string.IsNullOrWhiteSpace(GameDir))
        {
            LoaderDeployed = false;
            ForeignLoaderPresent = false;
            LoaderHint = "";
            return;
        }

        LoaderDeployed = _loader.IsDeployed(GameDir);
        ForeignLoaderPresent = _loader.HasForeignLoader(GameDir);

        LoaderHint = ForeignLoaderPresent
            ? "Note: a foreign dinput8.dll exists in the game folder (from another mod manager). Remove it to use this loader."
            : LoaderDeployed
                ? "Loader is active in the game folder."
                : "Loader is not deployed yet.";
    }

    private async Task<string?> PickFolderAsync(string title)
    {
        if (_storageProvider is null)
            return null;

        var folders = await _storageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = title,
            AllowMultiple = false,
        });

        var folder = folders.FirstOrDefault();
        return folder?.TryGetLocalPath();
    }
}
