# Local Windows development environment. Dot-source before CMake.
$ErrorActionPreference = 'Stop'
$betterRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$betterWorkspace = [IO.Path]::GetFullPath((Join-Path $betterRepo '../..'))
if (-not (Test-Path (Join-Path $betterWorkspace 'baseline.json'))) {
    throw 'This local build helper requires the configured workspace.'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
if (-not $vsInstall) { throw 'Visual Studio with the C++ toolchain is required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$sdkVersion = '10.0.22621.0'
$sdkBin = Join-Path $sdkRoot "bin/$sdkVersion/x64"
$sdkIncludes = @('ucrt', 'shared', 'um', 'winrt', 'cppwinrt') | ForEach-Object { Join-Path $sdkRoot "Include/$sdkVersion/$_" }
$sdkLibraries = @('ucrt', 'um') | ForEach-Object { Join-Path $sdkRoot "Lib/$sdkVersion/$_/x64" }
foreach ($required in (@($sdkBin) + $sdkIncludes + $sdkLibraries)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing SDK component: $required" }
}
$env:INCLUDE = ($sdkIncludes -join ';') + ';' + $env:INCLUDE
$env:LIB = ($sdkLibraries -join ';') + ';' + $env:LIB
$env:PATH = $sdkBin + ';' + $env:PATH
$env:BETTER_MIACODE_QT_ROOT = Join-Path $betterWorkspace 'toolchains/qt/6.11.1/msvc2022_64'
if (-not (Test-Path (Join-Path $env:BETTER_MIACODE_QT_ROOT 'lib/cmake/Qt6/Qt6Config.cmake'))) { throw 'Qt SDK is missing.' }
$env:BETTER_MIACODE_RUNTIME_ROOT = Join-Path $betterWorkspace 'runtime/better_miacode'
$betterTemp = Join-Path $env:BETTER_MIACODE_RUNTIME_ROOT 'temp'
New-Item -ItemType Directory -Path $betterTemp -Force | Out-Null
Set-Content -LiteralPath (Join-Path $env:BETTER_MIACODE_RUNTIME_ROOT '.better-miacode-runtime') -Value 'BetterMiacode.runtime.v1' -Encoding utf8NoBOM
$env:TEMP = $betterTemp
$env:TMP = $betterTemp
$env:TMPDIR = $betterTemp
$env:PATH = (Join-Path $env:BETTER_MIACODE_QT_ROOT 'bin') + ';' + $env:PATH
