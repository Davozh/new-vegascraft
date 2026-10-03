#!/bin/bash
# Instala en la carpeta de FNV lo que necesita New VegasCraft (y el plugin de build.sh si está compilado). Solo añade ficheros; `install.sh --remove` quita
# exactamente esos y devuelve el launcher original.
#   xNVSE 6.4.9   nvse_1_4.dll, nvse_steam_loader.dll, Data/NVSE/nvse_config.ini; nvse_loader.exe ocupa el sitio de
#                 FalloutNVLauncher.exe (el original queda como FalloutNVLauncher.vegascraft.exe), porque Steam en
#                 Proton lanza el launcher.
#   ReShade 6.8.0 (con add-ons, 32 bits) como d3d9.dll. En Proton necesita WINEDLLOVERRIDES="d3d9=n,b;d3dcompiler_47=n" en las
#                 opciones de lanzamiento de Steam.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
RUNTIME=${RUNTIME:-$HERE/../third_party/runtime}
FNV=${FNV_DIR:-/mnt/juegos/SteamLibrary/steamapps/common/Fallout New Vegas}
FILES=(nvse_1_4.dll nvse_steam_loader.dll Data/NVSE/nvse_config.ini d3d9.dll ReShade.ini ReShadePreset.ini
	Data/NVSE/Plugins/vegascraft.dll reshade-shaders/Shaders/VegasCraft.fx reshade-shaders/Shaders/ReShade.fxh
	reshade-shaders/Shaders/ReShadeUI.fxh d3dcompiler_47.dll)

if [ "$1" = "--remove" ]; then
	for f in "${FILES[@]}"; do rm -fv "$FNV/$f"; done
	if [ -f "$FNV/FalloutNVLauncher.vegascraft.exe" ]; then
		mv -v "$FNV/FalloutNVLauncher.vegascraft.exe" "$FNV/FalloutNVLauncher.exe"
	fi
	rmdir "$FNV/Data/NVSE/Plugins" "$FNV/Data/NVSE" "$FNV/reshade-shaders/Shaders" "$FNV/reshade-shaders/Textures" "$FNV/reshade-shaders" 2>/dev/null || true
	exit 0
fi

[ -f "$FNV/FalloutNV.exe" ] || { echo "no encuentro FalloutNV.exe en $FNV (usa FNV_DIR=...)"; exit 1; }
[ ! -f "$FNV/d3d9.dll" ] || cmp -s "$RUNTIME/reshade/ReShade32.dll" "$FNV/d3d9.dll" ||
	{ echo "ya hay un d3d9.dll que no es nuestro ReShade; no lo toco"; exit 1; }

cp -v "$RUNTIME/nvse/nvse_1_4.dll" "$RUNTIME/nvse/nvse_steam_loader.dll" "$FNV/"
mkdir -p "$FNV/Data/NVSE"
[ -f "$FNV/Data/NVSE/nvse_config.ini" ] || cp -v "$RUNTIME/nvse/Data/NVSE/nvse_config.ini" "$FNV/Data/NVSE/"
if [ ! -f "$FNV/FalloutNVLauncher.vegascraft.exe" ]; then
	mv -v "$FNV/FalloutNVLauncher.exe" "$FNV/FalloutNVLauncher.vegascraft.exe"
fi
cp -v "$RUNTIME/nvse/nvse_loader.exe" "$FNV/FalloutNVLauncher.exe"

cp -v "$RUNTIME/reshade/ReShade32.dll" "$FNV/d3d9.dll"
# Microsoft's shader compiler (32-bit, from Firefox 62's installer, as winetricks gets it): Proton's builtin one
# (vkd3d) rejects the HLSL ReShade generates for D3D9 (E5002, a static struct holding a sampler and a float2)
cp -v "$RUNTIME/d3dc/d3dcompiler_47.dll" "$FNV/"
mkdir -p "$FNV/reshade-shaders/Shaders" "$FNV/reshade-shaders/Textures"
if [ ! -f "$FNV/ReShade.ini" ]; then
	# FNV usa Z normal (no reversed) y profundidad sin invertir; el plano lejano se ajustará al calibrar
	# [DEPTH]: FNV clears its depth buffer before drawing the first-person arms; Generic Depth must copy it before
	printf '[GENERAL]\r\nEffectSearchPaths=.\\reshade-shaders\\Shaders\\\r\nTextureSearchPaths=.\\reshade-shaders\\Textures\\\r\nPresetPath=.\\ReShadePreset.ini\r\nPreprocessorDefinitions=RESHADE_DEPTH_INPUT_IS_REVERSED=0,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0,RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0,RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000\r\n\r\n[DEPTH]\r\nDepthCopyBeforeClears=1\r\nDepthCopyAtClearIndex=0\r\nUseAspectRatioHeuristics=1\r\n\r\n[OVERLAY]\r\nTutorialProgress=4\r\n' > "$FNV/ReShade.ini"
fi
# the plugin (build.sh) and the effect, enabled in the preset; McActive keeps FNV untouched until Minecraft is linked
if [ -f "$HERE/build/vegascraft.dll" ]; then
	mkdir -p "$FNV/Data/NVSE/Plugins"
	cp -v "$HERE/build/vegascraft.dll" "$FNV/Data/NVSE/Plugins/"
fi
cp -v "$HERE/shaders/VegasCraft.fx" "$HERE/../third_party/ReShade.fxh" "$HERE/../third_party/ReShadeUI.fxh" "$FNV/reshade-shaders/Shaders/"
printf 'Techniques=VegasCraft@VegasCraft.fx\r\nTechniqueSorting=VegasCraft@VegasCraft.fx\r\n' > "$FNV/ReShadePreset.ini"
echo "listo. Opciones de lanzamiento en Steam: WINEDLLOVERRIDES=\"d3d9=n,b;d3dcompiler_47=n\" %command%"
