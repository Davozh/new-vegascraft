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

1. [ ] Fake host en Linux: `host/fakehost.py` adaptado a `/dev/shm` → valida export de frames del mod Fabric.
2. [ ] Mod Fabric: sustituir `SharedMemory` por mapping de fichero; compilar con JDK 25.
3. [ ] Plugin xNVSE mínimo: cargar, loguear, abrir WebSocket, enviar cámara.
4. [ ] Add-on ReShade (x86, D3D9): subir frame y componer con el depth buffer de FNV.
5. [ ] Un cubo de Minecraft visible en el sitio correcto del Mojave.
6. [ ] Suelo por raycasts → barreras en Minecraft.
7. [ ] Bloques colocados → objetos invisibles con colisión en FNV.

## Pendiente de decidir / pedir al usuario

- Permiso para instalar xNVSE (y quizá JIP LN NVSE) y ReShade en la carpeta del juego.
- Instancia de Prism con Minecraft 26.3 + Fabric Loader 0.19.5 + Fabric API 0.161.0+26.3, game dir propio.

## Diario

- 2026-10-03: proyecto creado, universal-modder clonado, `um scan` OK, backup de saves hecho.
