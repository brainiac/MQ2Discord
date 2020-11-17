param([string]$VCPkgRoot);
Set-Location $VCPkgRoot
if (!(Test-Path ".\vcpkg.exe")) {
    & ".\bootstrap-vcpkg.bat"
}
& ".\vcpkg.exe" install yaml-cpp:x86-windows-static sleepy-discord[websocketpp]:x86-windows-static