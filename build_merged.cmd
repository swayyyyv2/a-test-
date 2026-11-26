@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul
where mspdbsrv.exe
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "%~dp0Sunrise.sln" /t:Build /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /v:minimal /nologo
exit /b %errorlevel%
