# DS2 iOS port — готовый к запуску репозиторий

Приватный форк `XDASCATX/DS2_switch` (GPL-3.0) + iOS-платформенный слой и полный CI-конвейер.
Верхний `README.md` — оригинальный README порта, этот файл — инструкция по сборке.

## Две строки в обычном cmd.exe

```bat
gh auth login --web --git-protocol https
C:\docs\darksouls2\ios_port\ci\launch.bat
```

**Никакого текста после первой команды** — `#` в cmd.exe не комментарий, а аргументы (была ошибка `accepts 0 arg(s), received 3`). `bash` в cmd тоже не нужен: есть нативный `launch.bat`.

`launch.bat` сам: создаст **приватный** репозиторий, запушит этот коммит, зальёт в приватный релиз `Default.xex` (21 МБ) и сжатый шейдерный пак (501 МБ), задаст секреты подписи, запустит workflow, дождётся и скачает `.ipa` в `C:\docs\darksouls2\ios_port\out\ci\`.

Альтернатива для Git Bash: `bash ios_port/ci/launch.sh` (или `DS2_PROJECT_ROOT=... bash ...` при запуске из копии репозитория).

Опционально, для подписи своим сертификатом:

```bash
export DS2_P12=/path/cert.p12
export DS2_P12_PASSWORD='...'
export DS2_MOBILEPROVISION=/path/profile.mobileprovision
bash ios_port/ci/launch.sh
```

## Что внутри

| Путь | Содержимое |
|---|---|
| `app/`, `config/`, `sdk/`, `tools/` | Исходники порта с **25 применёнными iOS-патчами** |
| `ios_port/cmake/ios-arm64.cmake` | CMake-тулчейн под `arm64-apple-ios` |
| `ios_port/include/ds2_ios/` | `spvz_reader.h` (сжатый шейдерный пак), `ios_paths.h`, `ios_touch_input_driver.h` |
| `ios_port/src/` | UIKit-оверлей и пути (`ios_paths.mm`), драйвер ввода `ios_touch_input_driver.cpp` |
| `ios_port/tools/mkipa.py` | Свой упаковщик `.ipa` без Xcode (проверен на 502.8 МиБ) |
| `ios_port/out/ds2_ios.toml` | Оптимизированный конфиг под iPhone 11 (93 ключа) |
| `.github/workflows/ds2-full.yml` | codegen (ubuntu) → сборка (macos-15) → упаковка/подпись (ubuntu) |

## Важно

- Репозиторий **приватный**. Игровые файлы (`Default.xex`, BDT, DLC) в git не коммитятся — только в приватный релиз, и только XEX + сжатые шейдеры.
- ~5.4 ГиБ `game_root/` копируются на телефон через **Files** в `Documents` после установки (`UIFileSharingEnabled`).
- GPL-3.0 + ассеты Bandai Namco ⇒ только личный sideload, без распространения.
- Первый прогон CI может упереться в линковку SDL3-iOS / MoltenVK — это нормально, лог покажет точное место. Известные точки: `ios_port/REMAINING.md`.

## Документы

`ios_port/ci/PRIVATE_ASSETS.md` — детали по приватным ассетам и лимитам.
`ios_port/TOUCH_CONTROLS.md` — раскладка управления.
`ios_port/REMAINING.md` — что осталось дописать.
