@echo off
setlocal

:: ===== Автопошук Visual Studio =====
set "VC_PATH="
for %%E in (Community Professional Enterprise Preview) do (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
        set "VC_PATH=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
        goto :found
    )
)
:found

if not defined VC_PATH (
    echo [ERROR] Visual Studio 2022 vcvars64.bat not found.
    echo Edit this file and set VC_PATH manually.
    pause
    exit /b 1
)

set "SRC_PATH=%~dp0GpuModuleBridgeM.cpp"
set "DLL_PATH=%~dp0GpuModuleBridgeM.dll"

echo [*] Building GpuModuleBridgeM.dll ...
call "%VC_PATH%" >nul
cl /nologo /O2 /Oi /Ot /GL /LD /MT /EHsc /D_USRDLL /D_WINDLL "%SRC_PATH%" /Fe:"%DLL_PATH%" /link /OPT:REF /OPT:ICF /LTCG

if %ERRORLEVEL% EQU 0 (
    echo [SUCCESS] GpuModuleBridgeM.dll has been built!
) else (
    echo [ERROR] Compilation failed.
)

pause
