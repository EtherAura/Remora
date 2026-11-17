@echo off
rem Run the test suites tools/windows-stage.sh --tests staged into this folder (bd remora-7oih.7):
rem
rem   run-tests.cmd <checkout> [suite ...]
rem
rem <checkout> is a clone of the commit the suites were built from: they read its fixtures and
rem vendor files through REMORA_TEST_SOURCE_DIR. With no suite named, every test_*.exe here runs.
rem Each log goes to the console and to results\<suite>.txt. The exit code is the number of suites
rem that failed, so a CI step fails on any of them.
setlocal EnableDelayedExpansion
if "%~1"=="" (
    echo usage: run-tests.cmd ^<checkout^> [suite ...] 1>&2
    exit /b 64
)
if not exist "%~1\tests\fixtures\" (
    echo run-tests: %~1 is not a Remora checkout: it has no tests\fixtures 1>&2
    exit /b 64
)
set "REMORA_TEST_SOURCE_DIR=%~f1"
if not defined QT_QPA_PLATFORM set "QT_QPA_PLATFORM=offscreen"
rem No labels: cmd finds them unreliably in a file with LF line ends, which is what a checkout has.
set "SUITES="
set "CHECKOUT=1"
for %%a in (%*) do (
    if defined CHECKOUT (set "CHECKOUT=") else set "SUITES=!SUITES! test_%%~a"
)
rem Relative names from here on: Wine's cmd expands no wildcard inside a quoted path.
pushd "%~dp0"
if not defined SUITES for %%f in (test_*.exe) do set "SUITES=!SUITES! %%~nf"
if not exist results mkdir results
set /a FAILED=0
for %%s in (%SUITES%) do (
    .\%%s.exe -o "results\%%s.txt,txt" -o -,txt
    if !errorlevel! neq 0 (
        echo run-tests: %%s FAILED, exit code !errorlevel!
        set /a FAILED+=1
    ) else (
        echo run-tests: %%s passed
    )
)
popd
echo run-tests: %FAILED% suite^(s^) failed
exit /b %FAILED%
