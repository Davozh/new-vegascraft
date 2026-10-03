#!/bin/bash
# Descarga en third_party/ lo que el lado de FNV necesita y no se puede redistribuir en este repo:
#   runtime/nvse/      xNVSE 6.4.9 (GitHub xNVSE/NVSE)
#   runtime/reshade/   ReShade32.dll 6.8.0 con soporte de add-ons (reshade.me)
#   runtime/d3dc/      d3dcompiler_47.dll de 32 bits de Microsoft, sacado del instalador de Firefox 62.0.3 como hace
#                      winetricks (el compilador de Proton no compila lo que ReShade genera para D3D9)
#   reshade/           cabeceras del API de add-ons de ReShade 6.8.0
#   ReShade.fxh, ReShadeUI.fxh   las que incluye VegasCraft.fx
# Necesita curl, 7z (p7zip / 7zip) y unzip.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TP="$HERE/../third_party"
RT="$TP/runtime"
XNVSE=6.4.9
RESHADE=6.8.0
UA="Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"
mkdir -p "$RT/nvse" "$RT/reshade" "$RT/d3dc" "$TP/reshade"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

curl -fsSL -o "$TMP/nvse.7z" "https://github.com/xNVSE/NVSE/releases/download/$XNVSE/nvse_${XNVSE//./_}.7z"
7z x -y -o"$RT/nvse" "$TMP/nvse.7z" >/dev/null

curl -fsSL -A "$UA" -H "Referer: https://reshade.me/" -o "$TMP/reshade.exe" "https://reshade.me/downloads/ReShade_Setup_${RESHADE}_Addon.exe"
# el instalador lleva las DLL en un zip pegado al final del exe (unzip avisa del exe delante)
unzip -qo "$TMP/reshade.exe" ReShade32.dll -d "$RT/reshade" 2>/dev/null || true
[ -f "$RT/reshade/ReShade32.dll" ] || { echo "no se pudo extraer ReShade32.dll"; exit 1; }

curl -fsSL -o "$TMP/firefox.exe" "https://download-installer.cdn.mozilla.net/pub/firefox/releases/62.0.3/win32/ach/Firefox%20Setup%2062.0.3.exe"
7z e -y -o"$RT/d3dc" "$TMP/firefox.exe" core/d3dcompiler_47.dll >/dev/null

for f in reshade.hpp reshade_api.hpp reshade_api_device.hpp reshade_api_pipeline.hpp reshade_api_resource.hpp reshade_api_format.hpp reshade_events.hpp reshade_overlay.hpp; do
	curl -fsSL "https://raw.githubusercontent.com/crosire/reshade/v$RESHADE/include/$f" -o "$TP/reshade/$f"
done
for f in ReShade.fxh ReShadeUI.fxh; do
	curl -fsSL "https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders/$f" -o "$TP/$f"
done
ls -R "$RT" | head -30
