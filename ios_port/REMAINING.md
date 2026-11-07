# Что осталось после 14:02

Платформенный слой написан, репозиторий собран и закоммичен (`ds2-ios-build/`, коммит `dd43634`). Ниже — честное состояние: что сделано, что осталось **только на первой реальной сборке**.

## ✅ Сделано в этом проходе

| Часть | Файлы |
|---|---|
| Точка входа | **не пишется с нуля**: SDK использует `windowed_app_main_sdl.cpp`, а SDL3-iOS сам предоставляет `UIApplicationMain`. Весь `SDLWindowedAppContext` / `WindowSDL` / `CAMetalLayerSurface` переиспользуется |
| Визуальный тач-оверлей | `ios_port/src/ios_paths.mm` — `DS2TouchOverlayView` на `CAShapeLayer`: два стика, A/B/X/Y, LB/RB, LT/RT, MENU, добавляется в key window |
| Пути | `ios_paths.h/.mm`: `DocumentsPath()`, `BundlePath()`, обработчик `UIApplicationDidReceiveMemoryWarningNotification`. `ds2_app.h::OnConfigurePaths` пропатчен: `game_root`/`user` в Documents, `ds2.toml` в бандле |
| Драйвер ввода | `ios_touch_input_driver.h/.cpp` — один XInput-геймпад, SDL event watch (не перехватывает события UI-цикла), мультитач-стики с мёртвой зоной, LT/RT, MFi/Bluetooth через `SDL_EVENT_GAMEPAD_*`, `packet_number`, вибрация. Зарегистрирован в `CreateDefaultInputSystem()` |
| CMake | iOS-сорцы, include-пути, `-fobjc-arc`, опциональный `REXGLUE_IOS_SDL3`, статический MoltenVK + zlib + фреймворки |
| Репозиторий | `ds2-ios-build/`: 1140 файлов, 4.04 МиБ в git, 2 коммита, workflow на месте, большие данные в git не попадают |
| Запуск | `ios_port/ci/launch.sh` — одна команда после `gh auth login` |

Всего **25 патчей / 12 файлов / +155 строк** в SDK, идемпотентно.

## ⚠️ Осталось (только по логу первой сборки)

1. **Линковка SDL3 под iOS.** `windowed_app_main_sdl.cpp` и SDL event watch требуют SDL3, собранный для `arm64-apple-ios`. Либо `fetch_thirdparty.py` принесёт SDL3 как сабмодуль и CMake соберёт его сам, либо передать `-DREXGLUE_IOS_SDL3=<SDL3.xcframework/ios-arm64/libSDL3.a>`. Что именно — покажет первый `cmake --build`.
2. **MoltenVK static entry points.** Патч маршрутизирует iOS на `vkGetInstanceProcAddr` напрямую, но `REXGLUE_IOS_MOLTENVK` должен указывать на реальный `libMoltenVK.a` из xcframework. Проверить, что MoltenVK экспортирует символ при статической линковке.
3. **`VK_EXT_metal_surface` из `CAMetalLayer`.** `surface_mac.h` уже умеет, но нужно подтвердить, что SDL3-iOS создаёт `SDL_MetalView` и отдаёт слой. Если нет — написать 20-строчный `CAMetalLayerSurface` без SDL.
4. **`ds2_native_depth_bias_absolute = false`** на MoltenVK (`VK_EXT_depth_bias_control` недоступен) — поправить в `ds2_ios.toml` после первого кадра.
5. **Аудио**: `sdl_audio_system` (SDL3-iOS → CoreAudio) либо `AVAudioEngine`.
6. **Jetsam-профилирование**: `SetLowMemoryCallback` уже зовётся, но сам сброс `texture_cache` + LRU `SpvzReader` надо привязать к нему в рантайме.

## 🚀 Запуск

```bash
gh auth login --web --git-protocol https     # один раз, единственный ручной шаг
bash ios_port/ci/launch.sh
```

`launch.sh` создаёт приватный репозиторий, пушит коммит, заливает `Default.xex` (21 МБ) + `ds2_shaders.spvz` (501 МБ) в приватный релиз, задаёт секреты подписи, запускает `ds2-full.yml`, ждёт и скачивает `.ipa`.

При первом прогоне почти наверняка выпадет ошибка линковки из пунктов 1–3 — это ожидаемо и нормально: кинь сюда лог, допишу по факту.

## Что НЕ надо делать

- Не писать Metal-рендерер с нуля — MoltenVK + SPIR-V уже в SDK.
- Не писать asm-фиберы — `fiber_switch.cpp` переиспользован.
- Не писать JIT/эмулятор PowerPC — статическая рекомпиляция делает codegen.
- Не писать UIKit `UIApplicationMain` вручную — его даёт SDL3-iOS.
- Не тащить 8 ГБ в CI — достаточно 21 МБ XEX + 501 МБ сжатых шейдеров.
- Не сжимать BDT — измерено ×1.00–1.03, данные несжимаемы.
