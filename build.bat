@echo off
setlocal

set "PROJECT_DIR=%~dp0"
REM Strip the trailing backslash: "C:\dir\" breaks quoted arguments
if "%PROJECT_DIR:~-1%"=="\" set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"
set "BUILD_DIR=%PROJECT_DIR%\build"

set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Release"

echo Building project
echo Configuration: %BUILD_TYPE%

where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: CMake not found. Install CMake first.
    exit /b 1
)

where git >nul 2>&1
if errorlevel 1 (
    echo ERROR: Git not found. It is needed to download wxWidgets.
    exit /b 1
)

cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE% --parallel

if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo Build completed!

endlocal
