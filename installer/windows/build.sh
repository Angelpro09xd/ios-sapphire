#!/bin/bash
# build.sh: out/SapphireInstaller.exe with the C# compiler Windows ships (.NET Framework 4.x, C# 5). From Git Bash
# or WSL. No SDK, no NuGet: the exe needs only .NET Framework 4.8 (in Windows 10 1903+ and 11).
set -e
cd "$(dirname "$0")"
CSC=/c/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe
[ -x "$CSC" ] || CSC=/mnt/c/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe
mkdir -p out
cd src
"$CSC" -nologo -target:winexe -optimize+ -out:../out/SapphireInstaller.exe -win32icon:app.ico -win32manifest:app.manifest \
    -resource:logo.jpg,logo.jpg -r:System.IO.Compression.dll -r:System.IO.Compression.FileSystem.dll -r:System.Web.Extensions.dll \
    -r:System.Windows.Forms.dll -r:System.Drawing.dll *.cs
ls -la ../out/SapphireInstaller.exe
