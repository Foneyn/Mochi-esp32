# Mochi — ESP32-WROOM-32

## Изменения
- CURIOUS: глаза увеличиваются по вертикали, а не по горизонтали.
- Команда `p` в Serial Monitor запускает именно AFFECTION.
- AFFECTION: счастливый прищур, румянец, рот P или 3, настроение +20.
- Физическая кнопка GPIO25 запускает ту же реакцию.
- Исправлена опечатка `nuint8_t` на `uint8_t`.

## Подключение
- OLED SSD1306 I2C: SDA GPIO21, SCL GPIO18, адрес 0x3C.
- MPU6050: SDA GPIO21, SCL GPIO18, адрес 0x68 (необязательно).
- Кнопка: GPIO25 к GND (INPUT_PULLUP).

## Сборка
Откройте папку в PlatformIO и выполните `pio run`.
Serial Monitor: 115200 бод; команда `p` — AFFECTION.
