@echo off
rem Pubblica una versione nuova di Iris: gli utenti la ricevono da soli con
rem l'aggiornamento automatico (controlla GitHub ogni qualche ora e all'avvio).
rem
rem Uso:   pubblica.bat 1.3.0 "Cosa cambia in questa versione"
rem
rem Cosa fa: scrive la versione nel file VERSION, compila, fa girare le prove
rem e crea la release V1.3.0 su GitHub con build\iris.exe allegato (serve
rem GitHub CLI, "gh auth login" una volta sola). La versione deve essere
rem più alta di quella pubblicata prima, se no nessuno la riceve.
setlocal

if "%~1"=="" (
    echo Uso: pubblica.bat 1.3.0 "Cosa cambia"
    exit /b 1
)
set "VER=%~1"
set "NOTES=%~2"
if "%NOTES%"=="" set "NOTES=Iris %VER%"

cd /d "%~dp0"
where gh >nul 2>nul || (
    echo GitHub CLI non trovata: installala con "winget install GitHub.cli" e poi "gh auth login".
    exit /b 1
)
gh release view "V%VER%" >nul 2>nul && (
    echo La versione V%VER% esiste gia' su GitHub: scegli un numero piu' alto.
    exit /b 1
)

rem La versione entra nell'exe: si scrive PRIMA di compilare.
> VERSION echo %VER%

call compila.bat || exit /b 1
build\iris_tests.exe > build\prove.log 2>&1
if errorlevel 1 (
    type build\prove.log
    echo.
    echo Le prove non passano: niente pubblicazione.
    exit /b 1
)

gh release create "V%VER%" "build\iris.exe#iris.exe" --title "Iris V%VER%" --notes "%NOTES%" || exit /b 1
echo.
echo Pubblicata Iris V%VER%. Ricordati di fare commit e push del codice (VERSION compreso).
endlocal
