@echo off
if not exist build mkdir build
cl.exe /EHsc /std:c++17 /MT /O2 /Fe:build\liveplayerstatsfetcher.exe livestatsfetcher.cpp /link Ws2_32.lib
