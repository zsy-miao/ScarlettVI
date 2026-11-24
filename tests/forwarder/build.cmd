@echo off
setlocal
rem Build this independent fixture using the local VS2026 x64 toolchain.
rem All generated binaries go into the ignored build/fixtures directory.
set "ROOT=%~dp0..\.."
for %%A in ("%ROOT%") do set "ROOT=%%~fA"
set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo Could not locate Visual Studio 2026 Community tools.
  exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%ROOT%"
if not exist "build\fixtures" mkdir "build\fixtures"
cl /nologo /std:c++20 /EHsc /LD /MD /Fo:"build\fixtures\FixtureBase.obj" /Fe:"build\fixtures\FixtureBase.dll" "tests\forwarder\FixtureBase.cpp"
if errorlevel 1 exit /b 1
link /nologo /DLL /NOENTRY /DEF:"tests\forwarder\ForwardBridge.def" /OUT:"build\fixtures\ForwardBridge.dll" /IMPLIB:"build\fixtures\ForwardBridge.lib"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /MD /Fo:"build\fixtures\ForwardApp.obj" /Fe:"build\fixtures\ForwardApp.exe" "tests\forwarder\ForwardApp.cpp" "build\fixtures\ForwardBridge.lib"
if errorlevel 1 exit /b 1
"build\x64\Release\ScarlettVI.exe" map "build\fixtures\ForwardApp.exe"
exit /b %errorlevel%
