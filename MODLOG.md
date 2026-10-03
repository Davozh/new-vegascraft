# New VegasCraft — MODLOG

Minecraft Java dentro de Fallout: New Vegas mediante passthrough (patrón 2 de `universal-modder/skills/mashup-mods`),
partiendo de `universal-modder/examples/minecraft-gta5-passthrough`.

## Entorno (2026-10-03)

- **Host:** CachyOS. Steam + Proton. Prism Launcher en `~/.local/share/PrismLauncher` (solo instancias 1.20.1 por ahora).
- **Agente:** corre en un contenedor Debian 13 que comparte `/home/david` y `/mnt/juegos`, pero NO `/usr` del host.
  Herramientas instaladas en el contenedor: OpenJDK 25.0.4, `i686-w64-mingw32-g++-posix` 14, ffmpeg 7.1, uv (pipx).
- **FNV:** Steam appid 22380, buildid 1510068, `/mnt/juegos/SteamLibrary/steamapps/common/Fallout New Vegas`.
  `FalloutNV.exe` x86. Solo `FalloutNV.esm` (sin DLCs). Sin anti-cheat. Sin loaders instalados.
  Prefijo Proton: `/mnt/juegos/SteamLibrary/steamapps/compatdata/22380/pfx`.
- **Backup:** `~/.universal-modder/backups/fnv-saves/20261003-022828.zip` (Documents/My Games/FalloutNV, 30 ficheros).
  Restaurar: `um backup restore fnv-saves`.

## Ruta elegida: passthrough

FNV no dibuja nada de Minecraft con su motor. Minecraft renderiza desde la cámara de FNV y un add-on de ReShade
compone su color+profundidad contra el depth buffer de FNV. FNV envía cámara + suelo (raycasts) por WebSocket;
los bloques colocados vuelven como objetos invisibles con colisión.

### Diferencias con el ejemplo de GTA V

| GTA V | FNV |
|---|---|
| ScriptHookV `.asi` (x64, MSVC) | plugin xNVSE `.dll` (x86, mingw i686) |
| natives por hash | estructuras del motor (NiCamera, bhkWorld) — referencia: código de JIP LN NVSE |
| ReShade 64 / D3D11, reversed-Z | ReShade 32 / D3D9 sobre DXVK (Proton), Z normal |
| `Local\MCPassthroughFrame` (CreateFileMapping con nombre) | fichero en `/dev/shm` mapeado desde Java nativo (FileChannel) y desde Wine (`Z:\dev\shm\...`) |
| Windows + WSL | Linux nativo: Minecraft nativo, FNV en Proton |

## Plan (vertical slice primero)

1. [x] Fake host en Linux: `host/fakehost.py` adaptado a `/dev/shm` → valida export de frames del mod Fabric.
2. [x] Mod Fabric: sustituir `SharedMemory` por mapping de fichero; compilar con JDK 25.
3. [~] Plugin xNVSE mínimo: cargar, loguear, abrir WebSocket, enviar cámara.
4. [~] Add-on ReShade (x86, D3D9): subir frame y componer con el depth buffer de FNV.
5. [ ] Un cubo de Minecraft visible en el sitio correcto del Mojave.
6. [ ] Suelo por raycasts → barreras en Minecraft.
7. [ ] Bloques colocados → objetos invisibles con colisión en FNV.

## Pendiente de decidir / pedir al usuario

- (hecho) xNVSE 6.4.9 + ReShade 6.8.0 add-on (32 bits, como `d3d9.dll`) instalados con `fnv/install.sh`.
  `FalloutNVLauncher.exe` = nvse_loader; original en `FalloutNVLauncher.vegascraft.exe`. `--remove` deshace.
  Steam necesita `WINEDLLOVERRIDES="d3d9=n,b" %command%`.
- (hecho) Instancia Prism `vegascraft`: MC 26.3, intermediary 26.3, Fabric Loader 0.19.5, LWJGL 3.4.3,
  Fabric API 0.161.0+26.3 en `minecraft/mods`. Java automático (necesita 25).

## Diario

- 2026-10-03: proyecto creado, universal-modder clonado, `um scan` OK, backup de saves hecho.
- 2026-10-03: el ratón no aparece en FNV (Hyprland + Proton, pantalla completa exclusiva). En `Fallout.ini` y
  `FalloutPrefs.ini`: `bFull Screen=0` y `bBackground Mouse=1` (copias `*.vegascraft.bak`). Si sigue fallando:
  gamescope en las opciones de lanzamiento. Para el passthrough conviene ventana de todas formas.
- 2026-10-03: xNVSE 6.4.9 y ReShade 6.8.0 verificados en el juego (consola `GetNVSEVersion`, `ReShade.log`: D3D9,
  2560x1440 ventana, AMD RX 6700 XT — el ejemplo de GTA solo se probó en NVIDIA).
- 2026-10-03: `mc/` copiado del ejemplo (MIT, ver `mc/LICENSE-universal-modder`). `SharedMemory` mapea
  `/dev/shm/VegasCraftFrame` en Linux; nombre Win32 `Local\VegasCraftFrame`. MAX 2560x1440 (FNV es 32 bits y
  también mapea esto: ~44 MB por slot; en FNV mapear solo el slot que se lee). Compila con JDK 25 / Gradle 9.7.1;
  jar copiado a la instancia Prism. `host/mcframe.py` lee `/dev/shm` en Linux.
- Nota: el contenedor del agente comparte `/dev/shm` con el host (se ven `pulse-shm-*`), así que las pruebas
  del agente pueden leer los frames que escribe Minecraft en el host.
- 2026-10-03: **fakehost OK en Linux.** MC nativo (Prism) + `host/fakehost.py` desde el contenedor: WebSocket
  127.0.0.1:25599 alcanzable, `/dev/shm/VegasCraftFrame` 126 MB, 1796 frames en 15 s (~120 fps), 1 frame de
  retraso, flags 7. `test_out/fakehost/comp_*.png`: bloques alineados sobre el suelo sintético y oclusión correcta.
  Problema: la ventana de MC salió 1053x1384 (Hyprland en mosaico ignora el resize). Hay que dejarla flotante
  (regla de Hyprland) para que tenga el tamaño y aspecto de FNV (gotcha 7 de GTA: HUD aplastado, ángulos torcidos).
- 2026-10-03: **plugin `fnv/` escrito y compilado** (`fnv/build.sh`, mingw i686 posix, estático, 787 KB):
  un solo `vegascraft.dll` en `Data/NVSE/Plugins` que es plugin xNVSE (MainGameLoop = 20) y add-on ReShade.
  - Motor (de las cabeceras de JIP LN, GPL: solo se usan direcciones/offsets, no código): SceneGraph `*0x11DEB7C`,
    cámara `+0xAC`; NiCamera: rot mundo 0x68, pos 0x8C, frustum 0xDC (l,r,t,b,near,far). Player `*0x11DEA3C`:
    rot 0x24, pos 0x30, is3rdPerson 0x64A. Runtime 1.4.0.525 = 0x040020D0 (confirmado en nvse.log).
  - Supuesto a verificar con F9: la dirección de vista es la columna 0 de la rotación (NiCamera mira por +X local).
  - **Gotcha ABI:** ReShade está compilado con MSVC; un método virtual que devuelve struct
    (`find_uniform_variable`) devuelve por puntero oculto en MSVC y en EDX:EAX en GCC. `find_uniform()` llama al
    slot del vtable con `thiscall` y el puntero oculto. Otros métodos así (get_resource_desc, find_technique…) no se
    usan; si se usan, necesitan el mismo trato.
  - Mapping en Wine: `CreateFileW("Z:\\dev\\shm\\VegasCraftFrame")` + `CreateFileMapping`; cabecera fija, y cada slot
    se mapea solo al subirlo (offset alineado a la granularidad) por el espacio de direcciones de 32 bits.
  - Shader `VegasCraft.fx`: el de GTA con Z normal y planos en metros. Sin probar en D3D9/SM3.
  - Escala: 70 unidades = 1 m. yOffset = 64 − pies/70 (el suelo bajo el jugador en y=64). Al enlazar se pone un
    pilar de diamante 4 bloques delante del jugador para alinear.
