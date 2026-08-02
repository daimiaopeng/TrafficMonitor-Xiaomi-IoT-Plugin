call "D:\app\Microsoft Visual Studio\VC\Auxiliary\Build\vcvars64.bat"
cd /d "%~dp0"
cl /std:c++20 /EHsc test_native_plug.cpp /link ws2_32.lib advapi32.lib
