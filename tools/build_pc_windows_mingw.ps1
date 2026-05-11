param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $RepoRoot

try {
    foreach ($cmd in @("cmake", "g++", "mingw32-make")) {
        if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
            throw "Missing required command: $cmd"
        }
    }
}
catch {
    Write-Host "Faltan herramientas de compilacion para MinGW/MSYS2." -ForegroundColor Yellow
    Write-Host "Instala MSYS2 y despues ejecuta en consola MSYS2 MinGW64:" -ForegroundColor Yellow
    Write-Host "  pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make git" -ForegroundColor Yellow
    Write-Host "Asegurate de tener en PATH: C:\msys64\mingw64\bin" -ForegroundColor Yellow
    throw
}

Write-Host "Configurando proyecto en $BuildDir ($Config)..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=$Config

Write-Host "Compilando..." -ForegroundColor Cyan
cmake --build $BuildDir --config $Config -j 4

Write-Host "Build completada. Binario esperado: $BuildDir/src/lan-play.exe" -ForegroundColor Green
