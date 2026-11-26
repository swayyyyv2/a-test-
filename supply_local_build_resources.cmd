@echo off
setlocal EnableExtensions
set "DEST=%~dp0Sunrise\resources"

if not exist "%DEST%\sunrise.rc" (
  echo Run this helper from the root of a Sunrise source checkout.
  pause
  exit /b 1
)

echo Paste the full path to an existing Sunrise\resources folder.
echo Example: C:\Users\ender\Downloads\Sunrise-merged\Sunrise-merged\Sunrise\resources
set /p "SOURCE=Resources folder: "
if not defined SOURCE (
  echo No source folder was entered.
  pause
  exit /b 1
)
for %%I in ("%SOURCE%") do set "SOURCE=%%~fI"

if not exist "%SOURCE%\sunrise.rc" (
  echo That folder does not contain sunrise.rc.
  pause
  exit /b 1
)

if not exist "%DEST%\bootflow" mkdir "%DEST%\bootflow"
for %%F in ("%SOURCE%\bootflow\*.dds") do if exist "%%~fF" if not exist "%DEST%\bootflow\%%~nxF" copy "%%~fF" "%DEST%\bootflow\%%~nxF" >nul

for %%F in (investment_defaults.sql item_names.bin item_icons.pack item_rarity.bin) do (
  if not exist "%DEST%\%%F" if exist "%SOURCE%\%%F" copy "%SOURCE%\%%F" "%DEST%\%%F" >nul
)

for %%F in (texture_80A145FF.dds texture_80A14601.dds texture_80A14607.dds texture_80A1460E.dds texture_80A1461D.dds texture_80A1461F.dds texture_80A14621.dds texture_80A14624.dds texture_80A14625.dds texture_80A14628.dds texture_80A14629.dds texture_80A1462B.dds texture_80A1462E.dds texture_80A1462F.dds texture_80A14631.dds texture_80A14633.dds texture_80A14636.dds texture_80A14610.dds texture_80A14613.dds texture_80A14617.dds texture_80A14619.dds texture_80A14604.dds texture_80A146D3.dds) do (
  if not exist "%DEST%\bootflow\%%F" goto missing_resources
)
for %%F in (investment_defaults.sql item_names.bin item_icons.pack item_rarity.bin) do (
  if not exist "%DEST%\%%F" goto missing_resources
)

echo Local resources are in place. They remain ignored by Git.
pause
exit /b 0

:missing_resources
echo Some required files are missing, or the source folder belongs to another Sunrise build.
echo Check the source path, then run this helper again.
pause
exit /b 1
