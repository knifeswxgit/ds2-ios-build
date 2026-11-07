# Что осталось дописать в коде

Конвейер (codegen → сборка → упаковка → подпись) готов, упаковщик проверен на реальном 501-МиБ паке. Ниже — **единственные** незакрытые части: платформенный слой iOS. Всё остальное уже либо сделано, либо автоматизировано.

## 1. Точка входа UIKit (блокер №1)

Сейчас `app/src/main.cpp` — это `REX_DEFINE_APP(ds2, Ds2App::Create)`, хостовый цикл. На iOS нужен `.app` с `UIApplicationMain`.

Новые файлы: `ios_port/src/main_ios.mm`, `DS2AppDelegate.h/.mm`, `DS2ViewController.h/.mm`.

- `UIViewController` с `+[CAEAGLLayer layerClass]`-аналогом: `CAMetalLayer` как backing layer;
- `CADisplayLink` на 60/120 Гц → тик presentations;
- `UIInterfaceOrientationLandscape*` только (уже в Info.plist);
- передать `(void*)CAMetalLayer` в `rex::ui::CAMetalLayerSurface` — этот класс **уже есть** в `sdk/include/rex/ui/surface_mac.h`, он создан под macOS/MoltenVK;
- заменить `windowed_app_context_switch.h` на iOS-аналог (`WindowedAppContextIOS`) — образец цикла сообщений лежит в Switch-версии.

CMake: добавить `main_ios.mm`/`AppDelegate.mm`/`ViewController.mm` в `DS2_SOURCES` при `REXGLUE_PLATFORM_IOS` и убрать `main.cpp` из этого таргета.

## 2. Ввод

`ios_port/TOUCH_CONTROLS.md` — готовая спецификация по реальному `InputDriver` / `X_INPUT_STATE`.

Новые файлы: `sdk/include/rex/input/ios/ios_touch_input_driver.h`, `sdk/src/input/ios_touch_input_driver.mm`.

- `UITouch` → снапшот в `std::atomic`, `GetDeviceState` читает без блокировки главного потока;
- `GCController` (`GCExtendedGamepad`) → тот же `X_INPUT_STATE`;
- `X_INPUT_VIBRATION` → `CoreHaptics`;
- **важно:** поля `be<uint16_t>`/`be<int16_t>` писать через обёртку, не `memcpy`.

## 3. Аудио

`rex/audio/sdl/sdl_audio_system.h` — SDL2 под iOS использует CoreAudio, это самый короткий путь. Альтернатива: `ios_audio_system.mm` на `AVAudioEngine`. XMA-декодер (`sdk/src/audio/xma/`) уже портабельный C, менять не надо.

## 4. Файловая система

`sdmc:/switch/ds2/...` → `NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, ...)`. Патчится в `sdk/src/core/filesystem_posix.cpp` (там уже есть `__APPLE__`-ветки) и `app/src/ds2_dlc.cpp`.

Данные (~5.4 ГиБ) кладутся в `Documents` через Files, а не в `.ipa`; `slim_manifest.txt` говорит, что именно копировать.

## 5. MoltenVK: довести линковку

Патч уже маршрутизирует iOS на статические entry points (`vkGetInstanceProcAddr` напрямую, как в NRO) и отключает macOS-staging dylib. Осталось на первой реальной сборке проверить:

- `REXGLUE_IOS_MOLTENVK` указывает на `MoltenVK.xcframework/ios-arm64/libMoltenVK.a`;
- `VK_EXT_depth_bias_control` на MoltenVK недоступен → использовать rollback-флаги из `ds2.toml` (`ds2_native_depth_bias_absolute = false`);
- `VK_EXT_metal_surface` создаётся из `CAMetalLayer`;
- `MoltenVK` лимит сэмплеров — обходки уже есть в `texture_cache.cpp`.

## 6. Память (iPhone 11, jetsam ~1.8–2 ГБ)

- Гостевая память X360 512 МБ + текстурный кеш + 6 блоков LRU шейдеров (~6 МиБ) должны уложиться;
- `ds2_render_scale = 0.75`, `ds2_native_msaa_host = false`, `ds2_fps_cap = 30` уже выставлены;
- при `didReceiveMemoryWarning` сбрасывать `texture_cache` и LRU `SpvzReader`;
- профилировать после первого запуска, не до.

## 7. Порядок

1. Запустить `ds2-full.yml` → получить первую ошибку линковки (это нормально, она укажет точное место).
2. Написать UIKit-точку входа → добиться первого кадра.
3. Ввод → аудио → VFS.
4. Профилирование памяти/FPS на девайсе, подстройка `ds2_ios.toml`.
5. Подпись своим сертификатом, установка.

## Что НЕ надо делать

- Не писать Metal-рендерер с нуля — MoltenVK + SPIR-V уже в SDK.
- Не писать asm-фиберы — `fiber_switch.cpp` переиспользован.
- Не писать JIT/эмулятор PowerPC — статическая рекомпиляция делает codegen.
- Не тащить 8 ГБ в CI — достаточно 21 МБ `Default.xex`.
- Не сжимать BDT — измерено: ×1.00–1.03, данные несжимаемы.
