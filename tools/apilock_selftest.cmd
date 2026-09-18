@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /MT /utf-8 ^
  /I D:\Workspace\rime\weasel\include ^
  apilock_selftest.cc /link ^
  kernel32.lib /OUT:apilock_selftest.exe
endlocal
