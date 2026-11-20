param(
    [ValidatePattern('^[A-Za-z0-9_-]+\.exe$')]
    [string]$OutputName = '24HourCountdown.exe'
)

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) {
    throw 'The Visual Studio installer and MSVC C++ build tools are required.'
}
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) {
    throw 'Install the MSVC x64 build tools and a Windows SDK.'
}
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'

Push-Location -LiteralPath $PSScriptRoot
try {
    New-Item -ItemType Directory -Force -Path dist, .build | Out-Null
    $commands = @(
        ('call "{0}" >nul' -f $vcvars)
        'rc /nologo /fo .build\countdown.res countdown.rc'
        ('cl /nologo /std:c++17 /permissive- /utf-8 /W4 /WX /O2 /MT /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /Fo.build\ /Fe:dist\{0} app.cpp clock.cpp settings.cpp .build\countdown.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /INCREMENTAL:NO user32.lib gdi32.lib shell32.lib advapi32.lib' -f $OutputName)
    )
    & $env:ComSpec /d /c ($commands -join ' && ')
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }
    Get-Item -LiteralPath (Join-Path dist $OutputName)
} finally {
    Pop-Location
}
