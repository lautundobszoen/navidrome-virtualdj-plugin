$ErrorActionPreference = 'Stop'

cmake -S $PSScriptRoot -B "$PSScriptRoot/build" -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake-Konfiguration fehlgeschlagen." }

cmake --build "$PSScriptRoot/build" --config Release
if ($LASTEXITCODE -ne 0) { throw "Plugin-Build fehlgeschlagen." }

Write-Host "Plugin erstellt: $PSScriptRoot/build/Release/Navidrome.dll"
