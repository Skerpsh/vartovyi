# Vartovyi

Повітряні тривоги Києва та Київської області на [M5StickC Plus2](https://docs.m5stack.com/en/core/M5StickC%20PLUS2): сирена, кольоровий екран, історія і статистика.
Дані офіційні, з [api.ukrainealarm.com](https://api.ukrainealarm.com), без Telegram і сторонніх каналів.

## Прошити стік

**Найпростіше:** відкрийте **https://skerpsh.github.io/vartovyi/** у Chrome або Edge на комп'ютері, підключіть стік USB-кабелем і натисніть «Прошити стік».

**Через PlatformIO:**
```sh
git clone https://github.com/Skerpsh/vartovyi && cd vartovyi
./flash.sh            # --erase також стирає збережені мережі й налаштування
```

## Перший запуск

Стік покаже QR-код мережі **Vartovyi**. Відскануйте його телефоном: відкриється сторінка налаштувань, де можна додати свій Wi-Fi (тільки 2,4 ГГц), вибрати зони, нічний режим, гучність і яскравість.
Натисніть **Close setup**, і стік підключиться. Пізніше сторінка відкривається з меню: Settings > Phone setup.

## Оновлення

Нові версії з'являються в [Releases](https://github.com/Skerpsh/vartovyi/releases). На стіку: Settings > Update firmware.
Якщо нова прошивка за 5 хвилин не дістане сервер, стік сам повернеться до попередньої.

## Як це працює

```
api.ukrainealarm.com ──webhook + опитування──► міст (vps/peace_bridge.py) ──Caddy, https──► стік
```
Міст отримує підписані вебхуки й раз на 65 с звіряється з повним списком тривог, а результат пише у невеликий текстовий файл. Стік читає його кожні 3 секунди.

## Свій сервер

Потрібні Linux із systemd, [Caddy](https://caddyserver.com) з сайтом на `*.nip.io` або вашому домені і ключ API від ukrainealarm.com.
```sh
mkdir -p /etc/peace-stick && echo 'ВАШ_КЛЮЧ' > /etc/peace-stick/api-key
echo 'WEBHOOK_URL=https://ваш.сервер/peace-stick/webhook' > /etc/peace-stick/env
curl -fsSL https://raw.githubusercontent.com/Skerpsh/vartovyi/main/vps/update.sh | sh
```
Скрипт ставить міст, додає маршрути в Caddyfile і вмикає таймер, який щогодини забирає новий реліз (і міст, і прошивку для стіка).
Прошивку зберіть зі своєю адресою:
```sh
PLATFORMIO_BUILD_FLAGS="-DALERT_URL='\"https://ваш.сервер/peace-stick\"'" ./flash.sh
```

## Розробка

- `test/`: тести логіки стіка (`g++`) і моста (`python3 test/test_bridge.py`).
- `./deploy.sh`: ставить поточну робочу копію моста на сервер, щоб перевірити її до релізу.
- Реліз: `git tag v1.2.0 && git push --tags`. GitHub Actions збирає прошивку, публікує реліз і оновлює веб-прошивальник, а сервер підхоплює реліз протягом години.
