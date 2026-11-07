# DS2 → iPhone: текущее состояние (07.11.2026, вечер)

Репозиторий: `knifeswxgit/ds2-ios-build` (PRIVATE). CI: `.github/workflows/ds2-full.yml`,
`codegen` (ubuntu) → `build` (macos-15 / Xcode 16.4 / iPhoneOS18.5) → `package` (ubuntu).

## Что уже работает
- Статическая рекомпиляция `Default.xex` (PowerPC → ARM64 C++) проходит в CI, артефакт закеширован.
- Configure iOS (CMake + Xcode generator) проходит; MoltenVK v1.4.2 `libMoltenVK.a` подключён статически.
- **Вся компиляция SDK + игрового кода + iOS-слоя проходит**; осталась финальная сборка/линковка Mach-O.
- 42 идемпотентных iOS-патча / 19 файлов (`ios_port/tools/apply_ios_patches.py`), проверены на чистом апстриме.
- Свой упаковщик `.ipa` без Xcode (`ios_port/tools/mkipa.py`), оптимизированный `ds2_ios.toml`, сжатый шейдерный пак `DS2SPVZ1` (501 МБ вместо 1643 МБ).

## Что исправлено по логам (по порядку)
| № | Ошибка | Фикс |
|---|---|---|
| 1 | `vkGetInstanceProcAddr` / `vkDestroyInstance` не объявлены | `extern "C"` объявления для iOS (`VK_NO_PROTOTYPES`), пропуск `GetSymbol<>()` |
| 2 | `X_STATUS` / `X_INPUT_*` не в глобале | `using` в `namespace ds2_ios` |
| 3 | `mach_vm.h unsupported` | `vm_region_64()` из `mach/vm_map.h` |
| 4 | `.type` / `.size` в asm (`unknown directive`) | Mach-O-ветка `REX_FIBER_ASM_*` с `_`-префиксом символов |
| 5 | librexruntime.dylib: undefined `_vk*`, `IosTouchInputDriver` | STATIC `rexruntime` / `rexgpu-xenos`, статический GPU-плагин, `SDL3/SDL_main.h` |
| 6 | `ds2_ios/spvz_reader.h` не найден | `#include "spvz_reader.h"` (файл лежит в корне `ios_port/include`) |
| 7 | `std::max(0, int16_t)`, `XINPUT_DEVTYPE_GAMEPAD` | `std::max<int16_t>`, `rex::input::` |
| 8 | `malloc.h` / `mallinfo()` (glibc-only) | `malloc_zone_statistics()` на Darwin |

## Скорость CI
- codegen: **11 мин → ~30 сек** (кеш сгенерированного C++ + `reuse_run_id`).
- `Configure iOS` (~3 мин) пропускается при точном попадании в кеш `out/app-ios`
  (`actions/cache/restore@v4` + `save@v4` под `always()`, `save-always:` в v4 не работает).
- `concurrency: cancel-in-progress`, `-jobs $(sysctl -n hw.ncpu)`, кеш MoltenVK.
- Ручные рычаги при `workflow_dispatch`: `force_codegen`, `force_configure`, `reuse_run_id`, `do_sign`.

## Что осталось
1. **Финальная линковка `ds2`** и артефакт Mach-O → `mkipa.py` → `ds2-ios.ipa`.
2. Подпись своим сертификатом (`do_sign=true` + секреты `P12_BASE64`, `P12_PASSWORD`, `MOBILEPROVISION_BASE64`).
3. Перенос данных на телефон через Files в `Documents`: `game_root/` (~5.4 ГБ), `ds2_shaders.spvz` (501 МБ) + `.spvz.idx`.
4. Первый запуск на устройстве: проверить `VK_EXT_metal_surface` / `CAMetalLayer`, выставить
   `ds2_native_depth_bias_absolute = false` (MoltenVK не имеет `VK_EXT_depth_bias_control`), аудио SDL3 → CoreAudio.
5. Jetsam-профилирование на iPhone 11 (A13 / 4 ГБ): `malloc_zone_statistics` уже даёт телеметрию в watchdog;
   привязать сброс `texture_cache` и LRU `SpvzReader` к memory warning.
6. FPS/температура: `render_scale 0.75`, `fps_cap 30` уже в `ds2_ios.toml`.

## Важно
- GPL-3.0 + ассеты Bandai Namco ⇒ только личный sideload, без распространения.
- Порт **не** эмулятор и **не** клон: это статическая рекомпиляция XEX2 + Vulkan→Metal через MoltenVK.
- Публичный CI не содержит игровых данных: только 21 МБ `Default.xex` в приватном релизе.
