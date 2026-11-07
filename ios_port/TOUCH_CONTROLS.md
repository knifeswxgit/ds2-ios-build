# Полное управление DS2 на iOS: спецификация

Опирается на реальные интерфейсы клона `DS2_switch_src/`:
- `sdk/include/rex/input/input_driver.h` — абстрактный `InputDriver`
- `sdk/include/rex/input/input.h` — `X_INPUT_STATE`, `X_INPUT_GAMEPAD`, константы кнопок

## 1. Контракт драйвера

```cpp
class InputDriver {
 public:
  virtual X_STATUS Setup() = 0;
  virtual void EnumerateDevices(std::vector<DeviceInfo>& out) = 0;
  virtual X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) = 0;
  virtual X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) = 0;
  virtual X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) = 0;
  virtual X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) = 0;
};
```

`X_INPUT_GAMEPAD` (12 байт):

```cpp
struct X_INPUT_GAMEPAD {
  be<uint16_t> buttons;        // битовые флаги, см. ниже
  uint8_t      left_trigger;   // 0..255
  uint8_t      right_trigger;  // 0..255
  be<int16_t>  thumb_lx, thumb_ly;
  be<int16_t>  thumb_rx, thumb_ry;  // -32768..32767
};
struct X_INPUT_STATE {
  be<uint32_t> packet_number;  // обязан инкрементироваться при изменении
  X_INPUT_GAMEPAD gamepad;
};
```

**Важно:** поля помечены `be<>` (big-endian, наследие PowerPC/X360). Писать нужно через обёртку-присваивание, а не `memcpy` сырых little-endian значений — иначе кнопки перепутаются.

Константы кнопок (`input.h`):

| Флаг | Значение |
|---|---|
| `X_INPUT_GAMEPAD_DPAD_UP/DOWN/LEFT/RIGHT` | 0x0001 / 0x0002 / 0x0004 / 0x0008 |
| `X_INPUT_GAMEPAD_START` / `BACK` | 0x0010 / 0x0020 |
| `LEFT_THUMB` / `RIGHT_THUMB` (L3/R3) | 0x0040 / 0x0080 |
| `LEFT_SHOULDER` / `RIGHT_SHOULDER` (LB/RB) | 0x0100 / 0x0200 |
| `GUIDE` | 0x0400 |
| `A` / `B` / `X` / `Y` | 0x1000 / 0x2000 / 0x4000 / 0x8000 |

Драйвер публикует **одно** устройство (`XINPUT_DEVTYPE_GAMEPAD`, `X_INPUT_FLAG_GAMEPAD`). Боевые биндинги DS2 остаются теми, что читает рекомпилированный код из игры, — оверлей только подаёт состояние геймпада X360.

## 2. Новый драйвер

Файлы: `sdk/include/rex/input/ios/ios_touch_input_driver.h`, `sdk/src/input/ios_touch_input_driver.mm`.

```cpp
class IosTouchInputDriver final : public InputDriver {
 public:
  IosTouchInputDriver(rex::ui::Window* w, size_t z)
      : InputDriver(w, z) {}
  X_STATUS Setup() override;
  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId, uint32_t, X_INPUT_CAPABILITIES*) override;
  X_RESULT SetDeviceVibration(DeviceId, X_INPUT_VIBRATION*) override;   // -> CoreHaptics
  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE*) override;

  // Вызывается из UIKit-потока; состояние хранится в std::atomic-снэпшоте,
  // чтобы GetDeviceState (поток XAM) не блокировался на главном потоке.
  void ApplyTouchSnapshot(const TouchSnapshot& s);
  void ApplyControllerSnapshot(const ControllerSnapshot& s);
};
```

`GetDeviceState` читает атомарный снэпшот, инкрементирует `packet_number` только при изменении, возвращает `X_STATUS_SUCCESS`.

## 3. Раскладка (ландшафт, 19.5:9)

| Зона | Действие | XINPUT |
|---|---|---|
| Левый нижний угол, виртуальный стик, радиус ~110 pt | Движение | `thumb_lx/ly`, радиальная мёртвая зона 0.12 |
| Правый нижний угол, виртуальный стик | Камера | `thumb_rx/ry` |
| Нажатие на левый стик | Спринт / L3 | `LEFT_THUMB` |
| Нажатие на правый стик | L3/R3 (лок) | `RIGHT_THUMB` |
| Большая кнопка справа, низ | Взаимодействие / A | `A` |
| Средняя кнопка справа | Перекат / B | `B` |
| Верхняя кнопка справа | Смена оружия / Y | `Y` |
| Левая кнопка справа | Использовать предмет / X | `X` |
| LB / RB (плечевые, верх экрана) | Блок / лёгкая атака | `LEFT_SHOULDER` / `RIGHT_SHOULDER` |
| LT / RT (широкие полосы по краям) | Тяжёлая атака / парад | `left_trigger` / `right_trigger` (аналог 0..255) |
| Крестовина (слева сверху) | D-pad | 0x0001..0x0008 |
| Маленькие кнопки по центру | Меню / карта | `START` / `BACK` |
| Hold `Y` + `LB`/`RB` | Двуручный хват / power stance | комбинация флагов, без синтетики |
| Свайп двумя пальцами | Открыть меню порта | `GUIDE` (опционально) |

Прозрачность оверлея, размеры и позиции — в `touch_layout.json`; пользовательская перенастройка сохраняется в `Documents/`.

## 4. Мультитач

- `UITouch` идентифицируется по `hash` + указателю; максимум ~10 одновременных касаний (iOS сам ограничивает).
- Каждая зона владеет `pointer_id`; при `touchesEnded` зона освобождается, флаг снимается в том же кадре.
- Виртуальные стики «прилипают» к первому касанию в зоне (floating stick), но не перескакивают между пальцами.
- Частота опроса — на `CADisplayLink` (60/120 Гц), `GetDeviceState` вызывается потоком XAM по его собственному расписанию.

## 5. Физические геймпады

`GCController` (`GCExtendedGamepad`) → тот же `X_INPUT_STATE`:
`buttonA/B/X/Y`, `leftShoulder/rightShoulder`, `leftTrigger/rightTrigger`, `leftThumbstick/rightThumbstick`, `dpad`, `buttonMenu`/`buttonOptions`, `leftThumbstickButton`/`rightThumbstickButton`.
При подключении геймпада тач-оверлей скрывается, но остаётся доступным по свайпу. Вибрация `X_INPUT_VIBRATION` → `CoreHaptics` (`CHHapticEngine`) + `GCController.playerIndex`-двигатели, если доступны.

## 6. Проверки перед сдачей

1. `packet_number` меняется только на реальное изменение состояния.
2. `be<>`-поля записаны через обёртку; дамп `X_INPUT_STATE` совпадает с эталоном X360.
3. Одновременное нажатие A+B+L3+R3 не теряется (стресс-тест 10 пальцев).
4. Нет блокировок главного потока в `GetDeviceState`.
5. Оверлей не перекрывает зону камеры на iPhone 11 с Safe Area.
