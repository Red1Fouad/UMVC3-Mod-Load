using CommunityToolkit.Mvvm.ComponentModel;
using UMVC3.ModManager.Models;

namespace UMVC3.ModManager.ViewModels;

public partial class ModEntryViewModel : ObservableObject
{
    private readonly ModEntry _mod;

    public ModEntryViewModel(ModEntry mod, bool isEnabled)
    {
        _mod = mod;
        _isEnabled = isEnabled;
    }

    public string FullPath => _mod.FullPath;
    public string Name => _mod.Name;
    public string SizeLabel => _mod.SizeLabel;
    public int FileCount => _mod.FileCount;
    public bool HasNativePC => _mod.HasNativePC;
    public bool HasOwnDInput8 => _mod.HasOwnDInput8;
    public bool HasAsi => _mod.HasAsi;
    public bool HasDebugRuntime => _mod.HasDebugRuntime;
    public bool UsesHooks => _mod.UsesHooks;

    public string HookBadge =>
        HasAsi && HasOwnDInput8 ? "ASI + dinput8" :
        HasAsi ? "ASI" :
        HasOwnDInput8 ? "dinput8" :
        HasNativePC ? "files" : "files";

    public string KindDescription =>
        HasNativePC && UsesHooks ? "content + hooks" :
        UsesHooks ? "hook plugin" :
        HasNativePC ? "content replacement" : "unknown";

    /// <summary>1-based priority slot; 0 = disabled.</summary>
    [ObservableProperty]
    private int _priority;

    [ObservableProperty]
    private bool _isEnabled;

    public string PriorityLabel => Priority <= 0 ? "" : "#" + Priority;
}
