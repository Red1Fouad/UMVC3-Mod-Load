$ErrorActionPreference = 'Stop'

$Root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$Ld   = Join-Path $Root 'src\Loader'
$Out  = Join-Path $Ld 'out'
$Mh   = Join-Path $Ld 'third_party\minhook'

$Cc = 'C:\devkitPro\msys2\opt\bin\x86_64-w64-mingw32-gcc.exe'
$Ar = 'C:\devkitPro\msys2\opt\bin\x86_64-w64-mingw32-ar.exe'

New-Item -ItemType Directory -Force -Path $Out | Out-Null

function Invoke-Cc {
    param([string[]]$CcArgs)
    & $Cc @CcArgs 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "gcc failed with exit code $LASTEXITCODE" }
}

Write-Host '=== Building MinHook static lib ==='
$mhFlags = @('-I', (Join-Path $Mh 'src'), '-I', (Join-Path $Mh 'src\hde'), '-I', (Join-Path $Mh 'include'), '-masm=intel', '-O2', '-Wall')
Invoke-Cc @($mhFlags + @('-c', (Join-Path $Mh 'src\buffer.c'),     '-o', (Join-Path $Out 'buffer.o')))
Invoke-Cc @($mhFlags + @('-c', (Join-Path $Mh 'src\hook.c'),       '-o', (Join-Path $Out 'hook.o')))
Invoke-Cc @($mhFlags + @('-c', (Join-Path $Mh 'src\trampoline.c'), '-o', (Join-Path $Out 'trampoline.o')))
Invoke-Cc @($mhFlags + @('-c', (Join-Path $Mh 'src\hde\hde32.c'),  '-o', (Join-Path $Out 'hde32.o')))
Invoke-Cc @($mhFlags + @('-c', (Join-Path $Mh 'src\hde\hde64.c'),  '-o', (Join-Path $Out 'hde64.o')))

& $Ar rcs (Join-Path $Out 'libMinHook.a') (Join-Path $Out 'buffer.o') (Join-Path $Out 'hook.o') (Join-Path $Out 'trampoline.o') (Join-Path $Out 'hde32.o') (Join-Path $Out 'hde64.o')
if ($LASTEXITCODE -ne 0) { throw 'ar failed' }

Write-Host '=== Building dinput8.dll ==='
Invoke-Cc @('-O2', '-Wall', '-I', (Join-Path $Mh 'include'), '-shared', '-static-libgcc', '-s',
    '-o', (Join-Path $Out 'dinput8.dll'),
    (Join-Path $Ld 'loader.c'), (Join-Path $Out 'libMinHook.a'), (Join-Path $Ld 'dinput8.def'))

$size = (Get-Item (Join-Path $Out 'dinput8.dll')).Length
Write-Host "DONE. Output: $Out\dinput8.dll ($size bytes)"
