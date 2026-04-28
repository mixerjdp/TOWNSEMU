# Plan definitivo: core libretro FM Towns basado en Tsugaru

## 1) Objetivo

Construir un core libretro eficiente, mantenible y con buen rendimiento, usando el arbol actual de Tsugaru (`C:/sw/Tsugaru/src`) sin rehacer todo ni depender de hacks de GUI.

Objetivo del primer entregable (MVP):
- Boot estable de BIOS/OS.
- Carga de 1 medio principal (CD o floppy).
- Video estable.
- Audio estable.
- Input util (pad, teclado, mouse).
- Persistencia basica de CMOS.

No objetivo del MVP:
- Save states expuestos al usuario final sin validacion robusta.
- Swap multi-disco complejo con M3U.
- Cobertura completa de todos los perfiles/modelos desde dia 1.

---

## 2) Aseveraciones validadas en codigo (ajuste del plan)

1. **Build principal en `src/CMakeLists.txt`**: correcto.  
   Implicacion: integrar libretro en este arbol, no en un repo nuevo aparte.

2. **`Outside_World` es el punto correcto de adaptacion**: correcto.  
   Implicacion: crear implementaciones libretro de `Outside_World`, `WindowInterface` y `Sound`.

3. **Video en Tsugaru no debe colgarse de `SendNewImage()` como unico camino**: correcto.  
   El frame final tambien llega por `FMTownsCommon::ForceRender()` -> `WindowInterface::UpdateImage()`.

4. **`TownsThread::VMMainLoop()` requiere `TownsUIThread*`**: correcto.  
   Implicacion: se necesita stub UI no bloqueante o refactor minimo del loop para modo libretro.

5. **`FMTownsCommon::Setup()` consume `TownsStartParameters` directo**: correcto.  
   Implicacion: no es obligatorio pasar por `TownsARGV::AnalyzeCommandParameter()`.

6. **Audio interno trabaja en 44.1kHz (FM/PCM/CDDA)**: correcto.  
   Implicacion: para MVP anunciar `sample_rate=44100` en libretro y evitar resample temprano.

7. **Formatos de CD confirmados por parser real**: `.cue`, `.bin`(+cue), `.iso`, `.mds`, `.mdf`(+mds), `.ccd`, `.chd`.  
   Implicacion: no limitar metadata a solo `cue|iso|mds`.

8. **Floppy confirmado**: `.d77`, `.d88`, `.rdd` y crudo binario (fallback).  
   Implicacion: declarar extensiones con cuidado y probar cada una.

9. **Save state ya existe en Tsugaru**: correcto (`SaveStateMem/LoadStateMem`, serializacion por dispositivo).  
   Implicacion: se puede planear soporte, pero no prometerlo sin pruebas round-trip bajo libretro.

---

## 3) Arquitectura objetivo del core

Ruta recomendada dentro del repo actual:

```text
src/
  libretro/
    CMakeLists.txt
    libretro.cpp
    libretro_core_options.h
    libretro_content.cpp
    libretro_content.h
    libretro_outside_world.h
    libretro_outside_world.cpp
    libretro_window.h
    libretro_window.cpp
    libretro_sound.h
    libretro_sound.cpp
    libretro_input.h
    libretro_input.cpp
    libretro_ui_stub.h
```

Principio de integracion:
- Mantener `towns`, `outside_world`, `discimg`, `diskdrive`, etc. sin forks agresivos.
- Agregar una capa libretro delgada y refactor minimo donde sea necesario (solo para stepping de frame y sync).

---

## 4) Estrategia de ejecucion (punto tecnico clave)

### 4.1 Decisiones

- Evitar loop infinito dentro de `retro_run()`.
- Evitar sincronizacion por reloj real en modo libretro.
- Ejecutar emulacion en porciones deterministas por frame.

### 4.2 Implementacion recomendada

Agregar en `TownsThread` un modo/lib helper de stepping (sin romper uso actual):

- Extraer la logica util de `RUNMODE_RUN` a una rutina reusable:
  - Ejecutar instrucciones.
  - `RunScheduledTasks()`.
  - `RunFastDevicePolling()`.
  - `CheckRenderingTimer(...)`.
  - `ProcessSound(...)`.
  - `cdrom.UpdateCDDAState(...)`.
  - `DevicePolling(...)` + `sound->Polling()` cuando toque.
- Desactivar para este camino:
  - `AdjustRealTime(...)`.
  - sleeps/esperas.
  - dependencia de hilo UI real (usar `TownsUIThread` stub no bloqueante).

### 4.3 Resultado esperado

`retro_run()` llama a un `RunOneFrame()` o `RunForNanoseconds(16666667)` y luego:
- envia frame mas reciente a `video_cb`.
- drena audio acumulado a `audio_batch_cb`.

---

## 5) Plan por fases (detallado y corregido)

## Fase 0 - Integracion de build (2-3 dias)

Tareas:
- Agregar opcion CMake: `BUILD_LIBRETRO_CORE`.
- Crear `src/libretro/CMakeLists.txt`.
- Enlazar target libretro contra librerias ya existentes (`towns`, `outside_world`, etc.).
- Evitar que el target libretro dependa de GUI real.

Validacion:
- Build limpio en Windows (MSVC) y Linux (GCC/Clang) del target libretro.

---

## Fase 1 - Esqueleto libretro minimo (2-3 dias)

Tareas:
- Implementar callbacks obligatorios:
  - `retro_set_environment`
  - `retro_set_video_refresh`
  - `retro_set_audio_sample_batch`
  - `retro_set_input_poll/state`
  - `retro_init/deinit`
  - `retro_api_version`
  - `retro_get_system_info`
  - `retro_get_system_av_info`
  - `retro_load_game/unload_game`
  - `retro_run`
- Pixel format inicial: `XRGB8888`.
- AV inicial:
  - fps: 60.0
  - sample rate: 44100.0
  - base geometry: 640x480
  - max geometry: conservador (por ejemplo 1024x1024), ajustable tras pruebas.

Validacion:
- RetroArch carga core y llama `retro_run()` sin crash.

---

## Fase 2 - Adaptador `Outside_World` para libretro (4-6 dias)

Tareas:
- Implementar `LibretroOutsideWorld`.
- Implementar `LibretroWindow : Outside_World::WindowInterface`.
  - Capturar frame en `UpdateImage(TownsRender::ImageCopy&)`.
  - Convertir RGBA interno a XRGB8888.
  - Mantener buffer frontal listo para `video_cb`.
- Implementar `LibretroSound : Outside_World::Sound`.
  - Bufferizar `FMPCMPlay()` y `BeepPlay()`.
  - `FMPCMChannelPlaying()` y `BeepChannelPlaying()` deben reflejar estado real de cola.
  - `Polling()` drena/actualiza cola.
- Implementar input bridge dentro de `DevicePolling()`:
  - Pad -> `SetGamePadState`.
  - Mouse -> `SetMouseMotion` + `SetMouseButtonState`.
  - Teclado -> `towns.keyboard.PushFifo(...)` (no depender de `ProcessInkey()`, que en base esta vacio).

Validacion:
- Sin emulacion completa aun, pero con callbacks de video/audio/input activos y sin bloqueos.

---

## Fase 3 - Arranque de VM y carga de contenido (3-5 dias)

Tareas:
- Construir `TownsStartParameters` directo en `retro_load_game()`.
- Poblar minimo:
  - `ROMPath`
  - `CMOSFName`
  - `cdImgFName` o `fdImgFName[0]`
  - `autoStart=true`
  - `noWait=true`
  - `catchUpRealTime=false`
  - `keyboardMode`
  - `specialPath` segun `system/save/content dirs`.
- Llamar `FMTownsCommon::Setup(...)`.
- Implementar selector por extension:
  - CD: `cue|bin|iso|mds|mdf|ccd|chd`
  - FD: `d77|d88|rdd|img|fdi|bin` (los no reconocidos por extension entran como RAW para floppy si se cargo como FD).

Validacion:
- Boot a BIOS/OS con al menos un contenido real.

---

## Fase 4 - Loop por frame y sincronizacion correcta (4-6 dias)

Tareas:
- Introducir stepping determinista para libretro (sin reloj real).
- Asegurar orden de trabajo por frame:
  - CPU/devices
  - render trigger
  - mezcla de audio
  - polling de input
  - entrega de frame/audio al frontend
- Mantener codigo actual de escritorio intacto.

Validacion:
- frame pacing estable.
- sin drift notable audio/video en 15+ minutos.

---

## Fase 5 - Input completo y ergonomia (2-4 dias)

Tareas:
- Mapear RetroPad standard a pad Towns.
- Teclado: set basico util para BIOS/OS/juegos.
- Mouse relativo como default para libretro.
- Core options iniciales:
  - modelo (`auto`, `MODEL2`, `2F`, `20F`, `UX`, `HR`, `MX`, `MARTY`)
  - CPU fidelity (`mid`, `high`)
  - RAM (`4`, `6`, `8`, `10`, `12`, `16`)
  - modo mouse (`relative`, `integrated`)

Validacion:
- Navegacion en BIOS/OS con pad + teclado + mouse.

---

## Fase 6 - Persistencia minima (2-3 dias)

Tareas:
- Guardar/cargar CMOS en `retro_unload_game` / `retro_load_game`.
- Usar `save_directory` de libretro.
- Mantener rutas estables por contenido.

Validacion:
- Configuracion CMOS persiste entre sesiones.

---

## Fase 7 - Save states libretro (segunda etapa real) (5-10 dias)

Tareas:
- Conectar `retro_serialize_size`, `retro_serialize`, `retro_unserialize` a `SaveStateMem/LoadStateMem`.
- Pruebas repetibles:
  - BIOS
  - CDDA activo
  - floppy activo
  - cambios de modo de video

Condicion de salida:
- habilitar oficialmente solo si round-trip es estable y sin corrupcion visible.

---

## Fase 8 - Multi-disco / M3U / disk control (post-MVP) (4-8 dias)

Tareas:
- Implementar disk control interface de libretro.
- Soporte `.m3u` propio del core (no existe hoy en Tsugaru).
- manejo de swap seguro para CD multi-disco.

Validacion:
- juego multi-CD con cambio de disco sin reset inesperado.

---

## 6) Riesgos reales y mitigacion

1. **Dependencia de `TownsUIThread` en el loop**  
   Mitigacion: stub UI + refactor acotado para stepping.

2. **Sincronizacion audio por semantica `FMPCMChannelPlaying/BeepChannelPlaying`**  
   Mitigacion: cola interna con umbrales claros y telemetria de underrun/overrun.

3. **Diferencias de timing al quitar realtime sync**  
   Mitigacion: stepping por nanosegundos + pruebas largas de estabilidad.

4. **Formatos y metadata sobre-prometidos**  
   Mitigacion: declarar solo extensiones validadas por parser real.

---

## 7) Cronograma recomendado

- Semana 1: Fases 0-1
- Semana 2: Fase 2
- Semana 3: Fases 3-4
- Semana 4: Fases 5-6
- Semana 5+: Fases 7-8 (segun prioridad)

Total MVP realista: 4 semanas.  
Total con save states + multi-disco maduro: 6-8 semanas.

---

## 8) Definicion de exito del MVP

- Core carga en RetroArch sin crash.
- Arranca BIOS/OS.
- Carga al menos un CD o floppy.
- Video estable y usable.
- Audio estable sin cortes severos.
- Input util (pad+teclado+mouse).
- CMOS persistente.

