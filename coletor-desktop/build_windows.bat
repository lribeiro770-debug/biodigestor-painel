@echo off
rem Gera dist\ColetorBiodigestor.exe (um arquivo so, sem console).
rem Requisitos: Python 3.9+ para Windows (python.org, com "tcl/tk and IDLE" marcado) e internet.
setlocal
cd /d "%~dp0"

set "PY=python"
where py >nul 2>nul
if not errorlevel 1 set "PY=py -3"

echo == Criando ambiente virtual (.venv)...
%PY% -m venv .venv
if errorlevel 1 goto erro
call .venv\Scripts\activate.bat
if errorlevel 1 goto erro

python -c "import tkinter" 2>nul
if errorlevel 1 (
  echo ERRO: este Python nao tem tkinter. Reinstale o Python marcando "tcl/tk and IDLE".
  goto erro
)

echo == Instalando PyInstaller...
python -m pip install --upgrade pip
python -m pip install pyinstaller
if errorlevel 1 goto erro

rem Pillow so serve para gerar o icone; se falhar, o .exe sai com o icone padrao.
if not exist icone.ico (
  python -m pip install pillow
  python gerar_icone.py
)

set "ICONE="
if exist icone.ico set ICONE=--icon icone.ico --add-data "icone.ico;."

echo == Gerando o executavel...
pyinstaller --noconfirm --clean --onefile --windowed --name ColetorBiodigestor %ICONE% coletor.py
if errorlevel 1 goto erro

echo.
echo Pronto: %CD%\dist\ColetorBiodigestor.exe
echo Copie esse arquivo para o notebook de campo (nao precisa instalar Python la).
pause
exit /b 0

:erro
echo.
echo Falhou. Veja as mensagens acima.
pause
exit /b 1
