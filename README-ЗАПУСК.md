# HackiOS на Redmi Note 8 Pro (begonia) — как загрузить

## Что это

Стадийный порт XNU (ядро iOS) на MediaTek Helio G90T (MT6785):

```
MTK LK (bootloader) ──>
  stage-2 loader (boot.img/kernel slot) ──>
    находит XNU Mach-O в ramdisk-слоте,
    копирует сегменты, собирает Apple-DT и boot_args,
    включает MMU (identity DRAM + KVA) ──>
       XNU entry (x0 = boot_args)
```

Первый этап bring-up проверяет саму цепочку: лоадер печатает баннер на UART
до того, как что-либо начинает копировать.

## Что нужно

- ПК с Windows (или любым другим ОС)
- Redmi Note 8 Pro с **разблокированным bootloader'ом** (у вас уже есть)
- USB-кабель; USB-отладка включена
- [Android platform-tools](https://developer.android.com/tools/releases/platform-tools)
  (adb + fastboot) — распаковать архив
- программный UART (опционально, но без UART вы НИЧЕГО не увидите):
  реплика begonia имеет debug UART на тест-пойнтах на плате; USB-UART
  адаптер (CP2102) на 1.8V, 115200 8N1. Когда сделаем framebuffer-вывод,
  UART станет необязателен.

## Шаги

### 1. Получить boot.img

- Push-репозиторий `HackiOS-begonia/` на GitHub (Actions включён сам).
- Открыть вкладку Actions → последний запуск → скачать артефакт
  `hackios-begonia-boot.img`.

Настройка CI описана в `.github/workflows/build.yml`.

### 2. Настроить adb

1. На телефоне: Настройки → О телефоне → 7 раз нажать «Версия MIUI» →
   появится «Расширенные настройки» → «Для разработчиков» → включить
   «Отладка по USB».
2. В распаковке platform-tools из cmd (запущенной в той папке):
   ```
   adb devices
   ```
   Разрешить отладку на экране телефона.

### 3. Загрузить образ БЕЗ перепрошивки (безопасный режим)

```
adb reboot bootloader
fastboot boot hackios-begonia-boot.img
```

`fastboot boot` не трогает память телефона — образ запускается из RAM
после перезагрузки телефон вернётся к нормальной системе.

### 4. Что вы увидите

Сейчас артефакт содержит stage-2 лоадер + заглушку вместо ядра.

- **На UART** увидите баннер, дамп Mach-O magic, главное: `LOADER PANIC`
  с сообщением («ramdisk slot does not contain a Mach-O64 kernel») — это
  ожидаемо на этапе bring-up: значит цепочка LK → лоадер работает.
- Без UART телефон «зависнет» на логотипе (ядро/лоадер не вернёт
  управление) — норм. Перезагрузите кнопку, ~10 сек.

### 5. С серийным выводом

Активный UART порт: UART0 у MT6785 на 0x11002000 (значение TBD-
фиксируется при первом реальном запуске через bring-up логов).

Драйвер MediaTek в ядре: `pexpert/arm/pe_serial.c` (переписан под
`compatible = mediatek,mt6785-uart`).

## Дальше

- Собрать XNU в CI (функция в repo `.github/workflows/build.yml`, job `xnu`).
- Подставить в boot.img реальный XNU Mach-O вместо заглушки.
- Отладка halts через UART-логи (TBD в `loader.h` фиксируются по логам).

## Research-only disposition

Это исследовательский порт. Никаких гарантий. Не прошейте
`fastboot flash boot` без дампа стокового boot.img.
