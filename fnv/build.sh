#!/bin/bash
# Builds build/vegascraft.dll: a 32-bit xNVSE plugin that is also a ReShade add-on, with mingw-w64 (i686, posix
# threads), statically linked so FNV needs no mingw runtime DLLs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CXX=${CXX:-i686-w64-mingw32-g++-posix}
mkdir -p "$HERE/build"
# FNV calls in with 4-byte stack alignment: let GCC realign where it needs 16
"$CXX" -std=c++20 -O2 -mincoming-stack-boundary=2 -shared -o "$HERE/build/vegascraft.dll" \
	-I"$HERE/../third_party/reshade" -I"$HERE/compat" \
	"$HERE/src/plugin.cpp" "$HERE/src/compositor.cpp" "$HERE/src/ws.cpp" \
	-static -static-libgcc -static-libstdc++ -lws2_32 \
	-Wall -Wno-unknown-pragmas -Wno-cast-function-type
ls -la "$HERE/build/vegascraft.dll"
