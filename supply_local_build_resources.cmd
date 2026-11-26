@echo off
setlocal EnableExtensions

echo Enter the root of the checkout you want to build.
echo It must contain Sunrise.sln and Sunrise\Sunrise.vcxproj.
set /p "TARGET=Target checkout: "
if not defined TARGET goto bad_target
for %%I in ("%TARGET%\.") do set "TARGET=%%~fI"
if not exist "%TARGET%\Sunrise.sln" goto bad_target
set "DEST=%TARGET%\Sunrise\resources"
if not exist "%DEST%\sunrise.rc" goto bad_target

echo.
echo Enter the path to an existing Sunrise\resources folder containing the local game resources.
echo Example: D:\Sunrise\Sunrise\resources
echo Paste paths without surrounding quotes; a trailing backslash is okay.
set /p "SOURCE=Resources folder: "
if not defined SOURCE goto bad_source
for %%I in ("%SOURCE%\.") do set "SOURCE=%%~fI"
if not exist "%SOURCE%\sunrise.rc" goto bad_source

for %%F in (texture_80A145FF.dds texture_80A14601.dds texture_80A14607.dds texture_80A1460E.dds texture_80A1461D.dds texture_80A1461F.dds texture_80A14621.dds texture_80A14624.dds texture_80A14625.dds texture_80A14628.dds texture_80A14629.dds texture_80A1462B.dds texture_80A1462E.dds texture_80A1462F.dds texture_80A14631.dds texture_80A14633.dds texture_80A14636.dds texture_80A14610.dds texture_80A14613.dds texture_80A14617.dds texture_80A14619.dds texture_80A14604.dds texture_80A146D3.dds) do (
  if not exist "%SOURCE%\bootflow\%%F" goto missing_source
)
for %%F in (item_names.bin item_icons.pack item_rarity.bin) do (
  if not exist "%SOURCE%\%%F" goto missing_source
)
if not exist "%SOURCE%\database\investment_defaults.sql" goto missing_source

if not exist "%DEST%\bootflow" mkdir "%DEST%\bootflow"
if not exist "%DEST%\database" mkdir "%DEST%\database"
for %%F in ("%SOURCE%\bootflow\*.dds") do if exist "%%~fF" if not exist "%DEST%\bootflow\%%~nxF" copy "%%~fF" "%DEST%\bootflow\%%~nxF" >nul
for %%F in (item_names.bin item_icons.pack item_rarity.bin) do (
  if not exist "%DEST%\%%F" copy "%SOURCE%\%%F" "%DEST%\%%F" >nul
)
if not exist "%DEST%\database\investment_defaults.sql" copy "%SOURCE%\database\investment_defaults.sql" "%DEST%\database\investment_defaults.sql" >nul

for %%F in (texture_80A145FF.dds texture_80A14601.dds texture_80A14607.dds texture_80A1460E.dds texture_80A1461D.dds texture_80A1461F.dds texture_80A14621.dds texture_80A14624.dds texture_80A14625.dds texture_80A14628.dds texture_80A14629.dds texture_80A1462B.dds texture_80A1462E.dds texture_80A1462F.dds texture_80A14631.dds texture_80A14633.dds texture_80A14636.dds texture_80A14610.dds texture_80A14613.dds texture_80A14617.dds texture_80A14619.dds texture_80A14604.dds texture_80A146D3.dds) do (
  if not exist "%DEST%\bootflow\%%F" goto copy_failed
)
for %%F in (item_names.bin item_icons.pack item_rarity.bin) do (
  if not exist "%DEST%\%%F" goto copy_failed
)
if not exist "%DEST%\database\investment_defaults.sql" goto copy_failed

echo Resources copied to "%DEST%".
echo They are ignored by Git and will not be included in a commit.
pause
exit /b 0

:bad_target
echo Target checkout is invalid. Choose the folder containing Sunrise.sln.
pause
exit /b 1

:bad_source
echo Source folder is invalid. Choose an existing Sunrise\resources folder.
pause
exit /b 1

:missing_source
echo The source folder is missing one or more required files or belongs to another Sunrise build.
pause
exit /b 1

:copy_failed
echo Some resources could not be copied. Check the target folder and run this helper again.
pause
exit /b 1
