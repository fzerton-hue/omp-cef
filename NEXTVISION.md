# Next-Vision: setările GTA din meniul ESC (modul `game_settings`)

Ce s-a adăugat față de omp-cef original (doar în **client.dll**, serverul și `cef.inc` rămân la fel):

| Fișier | Ce face |
|---|---|
| `src/client/core/browser/game_settings.hpp/.cpp` | aplică setările GTA pe firul jocului și răspunde paginii |
| `src/client/core/browser/client.cpp` | evenimentele `gta:get` / `gta:set` sunt tratate local, nu se trimit la server |
| `src/client/core/browser/manager.cpp` | `TickGameData()` rulează `GameSettings::Tick()` la fiecare cadru |

## Din pagină (JS)
```js
cef.on('gta:state', json => { /* {t:'state', v:{...}, keys:{...}, sup:{...}, restart:[...]} */ });
cef.emit('gta:get');                         // cere starea
cef.emit('gta:set', 'sfxVol', '70');         // volum efecte 0-100
cef.emit('gta:set', 'key.foot.jump', 'L SHIFT');
```
Chei: `invertMouse mouseSens(1-16) mouseFix steerMouse flyMouse sfxVol radioVol brightness drawDist fxQuality(0-3)
widescreen frameLimiter mipMap aa(0-3)` și `key.<foot|car>.<actiune>`. `mipMap` și `aa` se aplică după repornire.
Totul se salvează în `gta_sa.set` (CMenuManager::SaveSettings), la 1,5 s după ultima schimbare.

Adrese: plugin-sdk din `game_sa/` (FrontEndMenuManager, ControlsManager, CCamera, CVehicle, CRenderer, CGamma, Fx_c);
volumul: CAudioEngine 0xB6BC90 / 0x506E10 / 0x506DE0 (MTA:SA).

## Compilare
La fel ca înainte (Visual Studio + vcpkg + CEF, preset `client-win-x86-release`), sau din GitHub Actions
(workflow „Release”). Rezultă `client.dll` nou, care se pune în `cef/` din folderul jocului, prin launcher.
Versiunea pluginului nu s-a schimbat, deci serverul nu trebuie actualizat.
