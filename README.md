# Sunderandforged (Skyrim SE 1.5.97) — CommonLibSSE-NG project

This is a minimal CMake + vcpkg manifest project targeting **Skyrim SE 1.5.x** via **commonlibsse-ng-se**.

## Build (PowerShell)

```powershell
cd D:\SunderAndForged

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake"

cmake --build build --config Release
```

Result DLL:
`build\Release\Sunderandforged.dll`
