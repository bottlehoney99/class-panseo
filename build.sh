#!/bin/sh
set -e
x86_64-w64-mingw32-windres -c 65001 app.rc -O coff -o app.res
x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows -static -static-libgcc -static-libstdc++ \
  -Wall -Wno-unused-function panseo.cpp app.res -o Panseo.exe \
  -lgdiplus -lcomctl32 -lgdi32 -luser32 -lmsimg32
x86_64-w64-mingw32-strip Panseo.exe
