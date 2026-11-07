@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

REM =========================================================================
REM  DS2 -> .ipa : one-click launcher for plain cmd.exe (no bash required).
REM  Run from ANY directory. Paths are absolute by default.
REM
REM  One-time manual step (WITHOUT any trailing comment):
REM      gh auth login --web --git-protocol https
REM  Then just run this file:
REM      C:\docs\darksouls2\ios_port\ci\launch.bat
REM
REM  Optional signing with your own certificate (set before running):
REM      set DS2_P12=C:\path\cert.p12
REM      set DS2_P12_PASSWORD=yourpassword
REM      set DS2_MOBILEPROVISION=C:\path\profile.mobileprovision
REM =========================================================================

set "PROJECT_ROOT=C:\docs\darksouls2"
if not "%DS2_PROJECT_ROOT%"=="" set "PROJECT_ROOT=%DS2_PROJECT_ROOT%"
set "REPO=%PROJECT_ROOT%\ds2-ios-build"
set "RELEASE_TAG=ds2-private-assets"
set "REPO_NAME=ds2-ios-build"
if not "%DS2_REPO_NAME%"=="" set "REPO_NAME=%DS2_REPO_NAME%"

set "XEX=%PROJECT_ROOT%\darksouls_nintendo\DARK SOULS II Scholar of the First Sin (homebrew port) for Nintendo Switch [NSP]\switch\ds2\game_root\Default.xex"
set "SPVZ=%PROJECT_ROOT%\ios_port\out\ds2_shaders.spvz"
set "IDX=%PROJECT_ROOT%\ios_port\out\ds2_shaders.spvz.idx"

echo === DS2 iOS pipeline launcher (cmd.exe) ===
echo.

where gh >nul 2>&1
if errorlevel 1 (
  echo [X] GitHub CLI not found. Install: https://cli.github.com
  exit /b 1
)

gh auth status >nul 2>&1
if errorlevel 1 (
  echo [X] GitHub CLI is not logged in. Run this ONE command first:
  echo.
  echo        gh auth login --web --git-protocol https
  echo.
  echo     ^(do not add any text after the command^)
  echo     Then re-run launch.bat
  exit /b 1
)

for %%F in ("%XEX%" "%SPVZ%" "%IDX%" "%REPO%\.git\config" "%REPO%\.github\workflows\ds2-full.yml") do (
  if not exist %%~F (
    echo [X] missing: %%~F
    exit /b 1
  )
)

for /f "delims=" %%U in ('gh api user --jq .login') do set "GH_USER=%%U"
set "SLUG=%GH_USER%/%REPO_NAME%"
echo     account: %GH_USER%
echo     repo   : %SLUG%
echo.

cd /d "%REPO%"

echo [1/6] Creating PRIVATE repository (or reusing it)...
gh repo view "%REPO_NAME%" >nul 2>&1
if errorlevel 1 (
  gh repo create "%REPO_NAME%" --private --source . --remote origin --push
) else (
  git remote remove origin >nul 2>&1
  git remote add origin "https://github.com/%SLUG%.git"
  git push -u origin HEAD
)
if errorlevel 1 goto :fail

echo [2/6] Uploading private build inputs (21 MB XEX + 501 MB shaders)...
gh release view "%RELEASE_TAG%" --repo "%SLUG%" >nul 2>&1
if errorlevel 1 (
  gh release create "%RELEASE_TAG%" --repo "%SLUG%" --title "private build inputs" --notes "Personal use only. No game data archives." "%XEX%" "%SPVZ%" "%IDX%"
) else (
  gh release upload "%RELEASE_TAG%" "%XEX%" "%SPVZ%" "%IDX%" --repo "%SLUG%" --clobber
)
if errorlevel 1 goto :fail

echo [3/6] Signing secrets...
set "DO_SIGN=false"
if defined DS2_P12 if defined DS2_MOBILEPROVISION (
  for /f "delims=" %%B in ('powershell -NoProfile -Command "[Convert]::ToBase64String([IO.File]::ReadAllBytes('%DS2_P12%'))"') do set "P12B64=%%B"
  for /f "delims=" %%B in ('powershell -NoProfile -Command "[Convert]::ToBase64String([IO.File]::ReadAllBytes('%DS2_MOBILEPROVISION%'))"') do set "MPB64=%%B"
  gh secret set P12_BASE64 --repo "%SLUG%" --body "!P12B64!"
  gh secret set MOBILEPROVISION_BASE64 --repo "%SLUG%" --body "!MPB64!"
  if defined DS2_P12_PASSWORD (
    gh secret set P12_PASSWORD --repo "%SLUG%" --body "%DS2_P12_PASSWORD%"
  ) else (
    gh secret set P12_PASSWORD --repo "%SLUG%" --body ""
  )
  set "DO_SIGN=true"
  echo     signing enabled
) else (
  echo     DS2_P12 / DS2_MOBILEPROVISION not set - unsigned .ipa
)

echo [4/6] Starting workflow...
gh workflow run ds2-full.yml --repo "%SLUG%" -f asset_release=%RELEASE_TAG% -f do_sign=!DO_SIGN!
if errorlevel 1 goto :fail
ping -n 7 127.0.0.1 >nul
set "RUN_ID="
for /f "delims=" %%R in ('gh run list --workflow=ds2-full.yml --repo "%SLUG%" --limit 1 --json databaseId --jq .[0].databaseId') do set "RUN_ID=%%R"
if "!RUN_ID!"=="" (
  echo [X] could not determine run id
  goto :fail
)
echo     run id: !RUN_ID!
gh run watch !RUN_ID! --repo "%SLUG%" --exit-status

echo [5/6] Downloading artifacts...
if not exist "%PROJECT_ROOT%\ios_port\out\ci" mkdir "%PROJECT_ROOT%\ios_port\out\ci"
gh run download !RUN_ID! --repo "%SLUG%" --dir "%PROJECT_ROOT%\ios_port\out\ci"
dir /s /b "%PROJECT_ROOT%\ios_port\out\ci\*.ipa" 2>nul

echo [6/6] Done.
echo     If a job failed, show its log with:
echo         gh run view !RUN_ID! --repo %SLUG% --log-failed
exit /b 0

:fail
echo.
echo [X] Pipeline stopped. Show logs with:
echo     gh run list --workflow=ds2-full.yml --repo %SLUG%
exit /b 1
