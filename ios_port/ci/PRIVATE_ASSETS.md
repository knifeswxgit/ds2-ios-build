# Приватные ассеты для GitHub Actions

## Почему это безопасно и дёшево

В CI уезжает **только `Default.xex` (21 МБ)**. Он нужен исключительно для codegen:
`app/ds2_manifest.diag-dataonly.toml` ссылается на `../assets/game_root/Default.xex`.

**Не загружай** `GameData1/2.bdt` (4.8 ГБ), `dlc/`, `sound/`, `movie/` — они не участвуют ни в codegen, ни в сборке Mach-O. Эти ~5.4 ГБ копируются на телефон через Files после установки (`UIFileSharingEnabled` в Info.plist). Так мы не тратим лимит LFS/релизов, не светими данные и не замедляем раннер.

## Порядок

```bash
# 0. gh CLI должен быть авторизован: gh auth login

# 1. Приватный репозиторий. Обязательно --private.
gh repo create ds2-ios-build --private --clone
cd ds2-ios-build

# 2. Содержимое: клон DS2_switch_src/ (с нашими патчами) + папка ios_port/.
#    Проще всего скопировать оба дерева в корень нового репозитория:
#      cp -r <project>/DS2_switch_src/* .
#      cp -r <project>/ios_port ./ios_port
#    .github/workflows/ds2-full.yml <- ios_port/ci/ds2-full.yml
mkdir -p .github/workflows
cp ios_port/ci/ds2-full.yml .github/workflows/
git add -A && git commit -m "DS2 iOS port + build pipeline" && git push -u origin main

# 3. Приватный релиз с ОДНИМ файлом.
XEX="/c/docs/darksouls2/darksouls_nintendo/DARK SOULS II Scholar of the First Sin (homebrew port) for Nintendo Switch [NSP]/switch/ds2/game_root/Default.xex"
gh release create ds2-private-assets \
  --title "private build inputs" \
  --notes "Personal use only. Contains no game data archives." \
  "$XEX"

# 4. Секреты для подписи твоим сертификатом (опционально).
gh secret set P12_BASE64             --body "$(base64 -w0 cert.p12)"
gh secret set P12_PASSWORD           --body '<пароль от p12>'
gh secret set MOBILEPROVISION_BASE64 --body "$(base64 -w0 profile.mobileprovision)"

# 5. Запуск.
gh workflow run ds2-full.yml -f asset_release=ds2-private-assets -f do_sign=true
gh run list --workflow=ds2-full.yml
gh run watch

# 6. Забрать .ipa
gh run download <run-id> -n ds2-ios-ipa
```

## Проверки перед запуском

- `gh repo view --json isPrivate` → `true`. Публичный репозиторий с игровыми файлами делать нельзя.
- В релизе ровно один ассет `Default.xex`, размер 22 167 552 байт.
- `permissions: contents: read` в workflow достаточно для `gh release download`.
- Артефакт `ds2-generated` (сгенерированный C++) тоже остаётся внутри приватного репозитория, `retention-days: 7`.

## Лимиты

GitHub Actions: Linux-минуты дешёвые, **macOS-раннер считается с множителем ~10×**. На Free ~2000 минут/месяц, этого хватает на несколько полных прогонов; кодген и упаковка намеренно вынесены на `ubuntu-24.04`, macOS используется только для финальной компиляции. Если упрёшься в квоту — `macos-15` можно заменить self-hosted Mac или поднять план.

## Что делать, если codegen попросит ещё файлы

`[project] game_root = "../assets/game_root"` — генератор может обратиться к мелким файлам рядом с XEX. Если в логе появится «file not found», добавь только этот файл в тот же приватный релиз и расширь шаг `Fetch Default.xex` в workflow (`--pattern`). BDT-архивы не добавлять.

## Чего в CI нет

- UIKit app delegate / UI-цикл (`CADisplayLink` + `CAMetalLayer`);
- iOS-драйвер ввода и аудио;
- ремап путей VFS `sdmc:/switch/ds2/` → `Documents/`.

Это следующий слой кода, см. `ios_port/REMAINING.md`. Конвейер уже умеет собрать и упаковать всё остальное.
