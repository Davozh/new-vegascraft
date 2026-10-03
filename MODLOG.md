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
3. [x] Plugin xNVSE mínimo: cargar, loguear, abrir WebSocket, enviar cámara.
4. [x] Add-on ReShade (x86, D3D9): subir frame y componer con el depth buffer de FNV.
5. [x] Un cubo de Minecraft visible en el sitio correcto del Mojave.
6. [x] Suelo por raycasts → barreras en Minecraft.
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
- 2026-10-03: **primera prueba en el juego.** Plugin carga, enlaza con MC, el add-on se registra y abre los frames.
  F9 de volcado confirmó: la vista es la columna 0 de la rotación (rumbo 1,3° → +Y norte; 121,6° → ESE); frustum
  t=0.3904 → FOV vertical ~42.6°, near 5, far 353840 unidades. Fallos:
  - F9 es la carga rápida de FNV → volcado movido a **F10**.
  - El pilar se colocó en el menú principal (jugador sin celda, pies en 2048,2048,128) → ahora se espera a
    `parentCell` (0x40) ≠ 0 y se re-nivela en PostLoadGame (8) / NewGame (14).
  - **El shader no compila: `E5002 Static variables cannot have both numeric and resource components`.** Es el
    compilador HLSL de Proton (vkd3d) rechazando lo que ReShade genera para D3D9 (struct estático sampler+float2).
    Arreglo: `d3dcompiler_47.dll` de Microsoft (32 bits, sacado del instalador de Firefox 62 como hace winetricks)
    en la carpeta del juego + `WINEDLLOVERRIDES="d3d9=n,b;d3dcompiler_47=n"`.
- 2026-10-03: **HITO: Minecraft visible dentro de FNV.** Con `d3dcompiler_47` nativo, `VegasCraft.fx` compila en
  1,7 s. Pilar de diamante 4 bloques delante del jugador en el Mojave, con la mano/HUD de Minecraft encima.
  Pantallazos del usuario. Dos GPUs: FNV en la AMD RX 6700 XT, Minecraft en la NVIDIA RTX 5060 Ti (la copia por
  /dev/shm lo hace indiferente). MC a ~120 fps.
  Pendiente visto en las capturas:
  - Minecraft se dibuja encima de los menús/mensajes de FNV (ReShade compone tras la UI) → apagar en menús.
  - Se ven las manos de FNV y las de Steve a la vez, y el HUD de FNV bajo la barra de MC.
  - Queda el pilar de una conexión anterior (los bloques de MC persisten en su mundo; `clear` solo quita barreras).
  - Falta comprobar la oclusión (depth test) contra rocas/edificios y el desfase al girar rápido.
- 2026-10-03: **suelo y construcción** (sin probar aún en el juego):
  - Rayos Havok: `TES::PickObject` (0x458440, thiscall, args `RayCastData*`, 1), estructura de 0xB0 alineada a 16
    montada como `_GetRayCastObject` de JIP (escala Havok = unidades/7, hitFraction en 0x40, -1 en 0x44 y 0x50,
    filtro en 0x24 = capa 6 | grupo de colisión del jugador vía player+0x68→+0x138→+0x594→+8→+0x2C).
    Respaldo: `TES::GetTerrainHeight` (0x4572E0). TES `*0x11DEA10`, currentInterior en +0x34.
  - Muestreo como en GTA: espiral de radio 32, 48 rayos por frame, desde 2,5 m sobre los pies (o sobre el terreno
    si la ladera está más alta) hacia abajo 100 m; columnas de 2 barreras. Nivelado: suelo bajo el jugador → y 64;
    se re-nivela en carga de partida, F8 o salto de >30 m en un frame (viaje rápido, puertas).
  - Menús: `InterfaceManager` `*0x11D8A80` +0x0C (>1 = menú) → Minecraft se oculta y no se reenvía el ratón.
  - Modo construcción (tecla B): bit Fight (1<<3) en player+0x680 (solo se quita si lo puso el plugin), clic
    izq/der → `attack`/`use`, teclas 1-9 → hotbar. Solo con la ventana de FNV enfocada.
  - is3rdPerson: JIP dice 0x64A, xNVSE 0x64C; F10 registra ambos para decidir.
  - Quitado el pilar de prueba automático.
- 2026-10-03: **crash al enlazar** (el log se corta antes de `levelled`, en el primer rayo). Causa: `RayCastData`
  en la pila sin alinear a 16 — GCC i686 supone pila alineada y no realineó (`subl $192,%esp`), Havok usa `movaps`.
  Arreglo: buffer alineado a mano + `-mincoming-stack-boundary=2` (GCC realinea con `andl $-16,%esp`).
  Lección: todo lo que se pase al motor/Havok con SSE debe alinearse a mano; FNV entra con pila de 4 bytes.
- 2026-10-03: **rayos OK, construcción llega a MC, pero los bloques no se ven en FNV.** El frame de MC (leído de
  /dev/shm) muestra los bloques a 0,25–1,2 m; la mano/HUD sí se componen → el depth test los descarta (FNV "más
  cerca"). Sospechas: depth buffer de FNV mal elegido/limpio (FNV limpia la profundidad antes de las manos en 1ª
  persona), planos near/far distintos de los del NiCamera, o barreras por debajo del suelo real. Además la ventana de
  MC es 2536x1384 (no 16:9, Hyprland ignora el resize). Añadido F11: cicla DebugView del shader para medir.
- 2026-10-03: **F11 (vista "profundidad de FNV") = gris uniforme → el depth buffer llega a 0** (z = near). Generic
  Depth (add-on integrado de ReShade) está activo pero FNV limpia la profundidad antes de las manos en 1ª persona.
  Arreglo estándar: `[DEPTH] DepthCopyBeforeClears=1` (copia antes de los clears; índice 0 = el clear con más
  draws). Puesto en ReShade.ini e install.sh. Las capturas con Impr Pant no llegaron a la carpeta del juego (Hyprland
  se queda la tecla); el usuario pasó capturas propias. "No deja interactuar": el modo construcción arranca apagado
  tras reiniciar (B).
- 2026-10-03: **panel Generic Depth** (captura del usuario): elegido `D24S8 2560x1440, 1664 draws`, copia en CLEAR 1
  (1628 draws); también hay un INTZ 2560x1440 de 56 draws y D24S8 1024² (sombras). Causa del depth vacío:
  Generic Depth solo cambia a INTZ las *surfaces*; las *texturas* D24S8 las salta (supone PCF de sombras), y en D3D9
  una textura D24S8 no se puede muestrear ni copiar → backup vacío. Arreglo: el add-on registra `create_resource` y
  convierte a INTZ las texturas de profundidad D24S8 no cuadradas de >1024 de ancho. El add-on se registra ya en
  `NVSEPlugin_Load` (ReShade está cargado con el exe) para llegar antes de que FNV cree sus render targets.
  Riesgo: si FNV muestrease esa textura con PCF, se vería mal algo de FNV.
- 2026-10-03: **causa real del depth vacío: MSAA.** Registro de recursos de profundidad en create_resource:
  la de la escena es *surface* (type 5) D24S8 2560x1440 **samples 4** (usage 0x3030); también 1024² x4, una
  2560x1440 x1 (la INTZ de 56 draws) y 512². En D3D9 un depth multimuestreado no se puede leer ni convertir a INTZ.
  `FalloutPrefs.ini` tenía `iMultiSample=4` → puesto a 0 (y `bTransparencyMultisampling=0`). El parche de texturas
  INTZ era una pista falsa (no es textura): retirado. **Requisito: FNV sin antialiasing MSAA.**
- 2026-10-03: **HITO 2: construir en el Mojave.** Sin MSAA, Generic Depth convierte la profundidad a INTZ y el
  depth test funciona: bloques de tierra apoyados en la carretera de Nipton, ocultos correctamente por el terreno,
  con una flecha de ballesta clavada. Captura del usuario. Pendiente:
  - Colisión en FNV: los bloques no paran al jugador ni a los NPCs (paso 7: eventos `blocks` → objetos invisibles).
  - Las teclas 1-8 también disparan los accesos rápidos de FNV; manos y HUD de FNV duplicados.
  - Ventana de MC 2536x1384 (no 16:9): fijarla flotante a 2560x1440 o al aspecto de FNV.
  - Probar interiores, viaje rápido, giros rápidos (desfase), tercera persona (0x64A vs 0x64C).
- 2026-10-03: **temblor de los bloques al moverse.** Medido: MC publica a 120 fps constantes (intervalo máx 10 ms,
  8 ms captura→publicación) → no es MC. Causa: la pose del compositor se leía en MainGameLoop, antes de que FNV
  actualice la cámara del frame que dibuja → reproyección con la pose del frame anterior. Arreglo: xNVSE
  `kMessage_OnFramePresent` (24, data int* = pantalla de carga; se envía justo antes de 0xB6B730, que presenta y donde
  corre ReShade): ahí se fija la pose del compositor y se manda la cámara a MC. Además `install.sh` reemplaza el DLL
  con cp+mv (cp sobre un DLL cargado lo reescribe en el sitio y puede tumbar FNV).
- Lanzar los juegos desde el contenedor: socket IPC de Hyprland (config Lua): `dispatch hl.dsp.exec_cmd("...")`
  (`prismlauncher -l vegascraft`, `steam steam://rungameid/22380`).
