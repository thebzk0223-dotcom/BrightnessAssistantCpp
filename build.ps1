$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root "src"
$dist = Join-Path $root "dist"
$productName = -join ([char[]](
  0x5c4f,0x5e55,0x4eae,0x5ea6,0x8c03,0x8282,0x52a9,0x624b,
  0x20,0x2d,0x20,0x7f51,0x7edc,0x4e2d,0x5fc3,0x5236,0x4f5c
))
$out = Join-Path $dist ($productName + ".exe")
$vcvars64 = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
$vcvars64Alt = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if (!(Test-Path $vcvars64)) { $vcvars64 = $vcvars64Alt }
if (!(Test-Path $vcvars64)) { throw "Visual Studio C++ build tools not found." }

New-Item -ItemType Directory -Force -Path $dist | Out-Null
Push-Location $src
try {
  cmd /c "`"$vcvars64`" && rc /nologo /c 65001 /fo app.res app.rc && cl /nologo /utf-8 /std:c++17 /MT /EHsc /W4 /DUNICODE /D_UNICODE /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 main.cpp app.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:`"$out`" user32.lib gdi32.lib gdiplus.lib comctl32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib wbemuuid.lib dxva2.lib"
  if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
}
finally {
  Remove-Item ".\main.obj", ".\app.res" -Force -ErrorAction SilentlyContinue
  Pop-Location
}

Write-Host "Built: $out"
