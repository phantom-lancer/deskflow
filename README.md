# Deskflow Personal

Персональная сборка [Deskflow](https://github.com/deskflow/deskflow) для совместного использования клавиатуры и мыши между компьютерами Windows и Linux.

## Возможности

- Передача файлов через буфер обмена и перетаскивание между компьютерами.
- Медиаклавиши F6 / F7 / F8 с выбором получателей.
- Русский интерфейс.
- Windows: темы System, Light и Dark.
- Linux: тёмная тема и изменяемый размер окна.
- Windows: необязательный Game Mode для удержания курсора в полноэкранной игре.

## Загрузка

Последняя стабильная сборка: [GitHub Releases](https://github.com/phantom-lancer/deskflow/releases/latest).

- Windows x64 - MSI.
- Linux x86_64 - Flatpak.

## Совместимость

Для передачи файлов обе стороны должны использовать Deskflow Personal с протоколом Hopflow 1.100. Обычная сборка Deskflow этот протокол не поддерживает.

Файловый обмен и нативный drag-and-drop в Linux рассчитаны на X11. На Wayland передача файлов через URI и нативный drop пока не поддерживаются.

Подробности: [DESKFLOW-CUSTOM.md](DESKFLOW-CUSTOM.md).

## Сборка

Windows MSI, Linux и Flatpak собираются через [GitHub Actions](https://github.com/phantom-lancer/deskflow/actions).

## Основа

- [Deskflow](https://github.com/deskflow/deskflow)
- [Hopflow](https://github.com/Eyad-Alqaysi/hopflow) - файловый транспорт

Лицензии и авторские заголовки исходных компонентов сохранены.