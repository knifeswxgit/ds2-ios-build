# DS2 → iPhone: текущее состояние (07.11.2026, вечер)

## ✅ СБОРКА ПРОШЛА (run 42374462716)

`codegen` ✅ → `build` ✅ → `package` ✅. Артефакт **`ds2-ios.ipa`** (43 439 916 B,
скачан в `ios_port/out/ci/ds2-ios.ipa`), внутри:
`Payload/Dark Souls II.app/{ds2, Info.plist, ds2_ios.toml}`.
Mach-O 64 / **arm64** (`cputype 0x0100000C`), `MH_EXECUTE`, 144.9 МБ. `--verify`: STRUCTURE OK, CRC OK.

**Важно:** `.ipa` неподписанный (`do_sign=false`, секреты не заданы). Это заготовка для
личного sideload, а не готовый к установке файл.

## Что нужно сделать вручную
1. **Подпись:** AltStore / SideStore / Sideloadly / TrollStore своим Apple ID, либо
   задать секреты `P12_BASE64`, `P12_PASSWORD`, `MOBILEPROVISION_BASE64` и запустить
   workflow с `do_sign=true` (подпись `zsign` в CI).
2. **Данные через Files → Documents приложения:**
   - `game_root/` из раздачи Switch-порта (~5.4 ГБ после вырезов),
   - `ios_port/out/ds2_shaders.spvz` (501 МБ),
   - `ios_port/out/ds2_shaders.spvz.idx` (1.4 МБ).
3. **Первый запуск** — смотреть лог: `VK_EXT_metal_surface`/`CAMetalLayer`,
   `ds2_native_depth_bias_absolute=false` (нет `VK_EXT_depth_bias_control`),
   SDL3-аудио → CoreAudio, jetsam на A13.

---


Репозиторий: `knifeswxgit/ds2-ios-build` (PRIVATE). CI: `.github/workflows/ds2-full.yml`,
`codegen` (ubuntu) → `build` (macos-15 / Xcode 16.4 / iPhoneOS18.5) → `package` (ubuntu).

## Что уже работает
- Статическая рекомпиляция `Default.xex` (PowerPC → ARM64 C++) проходит в CI, артефакт закеширован.
- Configure iOS (CMake + Xcode generator) проходит; MoltenVK v1.4.2 `libMoltenVK.a` подключён статически.
- **Вся компиляция + линковка Mach-O проходят** (338 сгенерированных `.cpp`, 369 МБ исходников).
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
| 9 | линковка: `dynamic/.../libMoltenVK.a` не существует | детерминированный `/tmp/moltenvk/libMoltenVK.a` + `include/` |
| 10 | Mach-O артефакт не находился | отдельный шаг `Stage Mach-O` (Xcode пишет в `sdk/out/ios-arm64/Release`) |

## Скорость CI
- codegen: **11 мин → ~30 сек** (кеш сгенерированного C++ + `reuse_run_id`).
- `Configure iOS` (~3 мин) пропускается при точном попадании в кеш `out/app-ios`
  (`actions/cache/restore@v4` + `save@v4` под `always()`, `save-always:` в v4 не работает).
- `concurrency: cancel-in-progress`, `-jobs $(sysctl -n hw.ncpu)`, кеш MoltenVK.
- Ручные рычаги при `workflow_dispatch`: `force_codegen`, `force_configure`, `reuse_run_id`, `do_sign`.

## Что осталось (только устройство)
1. Подпись и установка (см. выше).
2. Перенос данных и первый запуск; по логу — правки `ds2_ios.toml` / MoltenVK-фолбэки.
3. Jetsam-профилирование на iPhone 11 (A13 / 4 ГБ): `malloc_zone_statistics` уже даёт телеметрию в watchdog;
   привязать сброс `texture_cache` и LRU `SpvzReader` к memory warning.
4. FPS/температура: `render_scale 0.75`, `fps_cap 30` уже в `ds2_ios.toml`.
5. На устройстве не проверялось — рантайм-поведение (Vulkan surface, аудио, память) подтвердится только после sideload.

## Важно
- GPL-3.0 + ассеты Bandai Namco ⇒ только личный sideload, без распространения.
- Порт **не** эмулятор и **не** клон: это статическая рекомпиляция XEX2 + Vulkan→Metal через MoltenVK.
- Публичный CI не содержит игровых данных: только 21 МБ `Default.xex` в приватном релизе.
