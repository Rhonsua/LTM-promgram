@echo off
REM ============================================================================
REM ltm-index build helper  (ASCII only: cmd.exe parses UTF-8 batch files badly)
REM
REM   scripts\build.bat [build|configure|test|faultinject|ninja|clean]
REM
REM Why the Visual Studio generator is the default:
REM   On this machine the Ninja generator stalls for minutes inside CMake's MSVC
REM   ABI-detection step (cl /showIncludes try-compiles). It is not a deadlock,
REM   but it makes the build look hung and it produced no progress output at all.
REM   The VS generator configures in ~20s and builds reliably. Ninja stays
REM   available via `scripts\build.bat ninja` for anyone who wants it.
REM ============================================================================
setlocal

set "VCVARS=D:\visual studio\IDE\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=D:\visual studio\IDE\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=D:\visual studio\IDE\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "VSGEN=Visual Studio 18 2026"
set "BUILD=build-vs"
set "CONFIG=RelWithDebInfo"

pushd "%~dp0.."
set "ROOT=%CD%"

set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=build"

if not exist "%CMAKE%" (
  echo [ERROR] cmake not found: %CMAKE%
  echo         edit scripts\build.bat and point CMAKE at your cmake.exe
  popd & exit /b 1
)

if /i "%ACTION%"=="configure"  goto configure
if /i "%ACTION%"=="build"      goto build
if /i "%ACTION%"=="test"       goto test
if /i "%ACTION%"=="faultinject" goto faultinject
if /i "%ACTION%"=="ninja"      goto ninja
if /i "%ACTION%"=="clean"      goto clean
echo [ERROR] unknown action: %ACTION%
echo available: build ^| configure ^| test ^| faultinject ^| ninja ^| clean
popd & exit /b 2

REM ---------------------------------------------------------------------------
:configure
if not exist "%VCVARS%" (
  echo [ERROR] vcvars64.bat not found: %VCVARS%
  popd & exit /b 1
)
call "%VCVARS%" >nul
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\%BUILD%" -G "%VSGEN%" -A x64
set "RC=%errorlevel%"
popd & exit /b %RC%

:ensure_configured
if not exist "%ROOT%\%BUILD%\CMakeCache.txt" (
  call :do_configure
  if errorlevel 1 ( exit /b 1 )
)
exit /b 0

:do_configure
if not exist "%VCVARS%" (
  echo [ERROR] vcvars64.bat not found: %VCVARS%
  exit /b 1
)
call "%VCVARS%" >nul
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\%BUILD%" -G "%VSGEN%" -A x64
exit /b %errorlevel%

REM ---------------------------------------------------------------------------
:build
call :ensure_configured || ( popd & exit /b 1 )
call "%VCVARS%" >nul
"%CMAKE%" --build "%ROOT%\%BUILD%" --config %CONFIG%
set "RC=%errorlevel%"
popd & exit /b %RC%

REM ---------------------------------------------------------------------------
:test
call :ensure_configured || ( popd & exit /b 1 )
call "%VCVARS%" >nul
"%CMAKE%" --build "%ROOT%\%BUILD%" --config %CONFIG%
if errorlevel 1 ( popd & exit /b 1 )
echo.
echo ---- ctest -E validate  ^(^"????^"???????python scripts\validate_tree.py^) ----
"%CMAKE%" -E chdir "%ROOT%\%BUILD%" ctest -C %CONFIG% -E validate --output-on-failure
set "RC=%errorlevel%"
popd & exit /b %RC%

REM ---------------------------------------------------------------------------
:faultinject
if not exist "%VCVARS%" (
  echo [ERROR] vcvars64.bat not found: %VCVARS%
  popd & exit /b 1
)
call "%VCVARS%" >nul
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build-fi" -G "%VSGEN%" -A x64 -DLTM_ENABLE_FAULT_INJECT=ON
if errorlevel 1 ( popd & exit /b 1 )
"%CMAKE%" --build "%ROOT%\build-fi" --config %CONFIG%
set "RC=%errorlevel%"
popd & exit /b %RC%

REM ---------------------------------------------------------------------------
:ninja
if not exist "%VCVARS%" (
  echo [ERROR] vcvars64.bat not found: %VCVARS%
  popd & exit /b 1
)
call "%VCVARS%" >nul
echo [WARN] Ninja generator: local CMake may stall for minutes in MSVC ABI detection.
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build-ninja" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 ( popd & exit /b 1 )
"%CMAKE%" --build "%ROOT%\build-ninja"
set "RC=%errorlevel%"
popd & exit /b %RC%

REM ---------------------------------------------------------------------------
:clean
if exist "%ROOT%\build"       rmdir /s /q "%ROOT%\build"
if exist "%ROOT%\build-vs"    rmdir /s /q "%ROOT%\build-vs"
if exist "%ROOT%\build-fi"    rmdir /s /q "%ROOT%\build-fi"
if exist "%ROOT%\build-ninja" rmdir /s /q "%ROOT%\build-ninja"
echo cleaned build/ build-vs/ build-fi/ build-ninja/
popd & exit /b 0
