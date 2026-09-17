@echo off
REM Double-click launcher for the Koi NNUE Studio GUI.
setlocal
set "KOI_REPO=%~dp0"
set "KOI_STUDIO=%KOI_REPO%tools\nnue\koi_nnue_studio.py"

where pythonw.exe >nul 2>nul
if %errorlevel%==0 (
    start "" pythonw.exe "%KOI_STUDIO%" %*
) else (
    python.exe "%KOI_STUDIO%" %*
)
endlocal
