# Reaper Media Import Extension

Native Dolby, DTS and subtitle import for REAPER on Windows and macOS.

[Русский](#русский) · [English](#english)

## Русский

### Содержание

- [Возможности](#возможности)
- [Быстрая установка расширения](#быстрая-установка-расширения)
- [Быстрая установка библиотек FFmpeg](#быстрая-установка-библиотек-ffmpeg)
- [Поддерживаемые форматы](#поддерживаемые-форматы)
- [Как это работает](#как-это-работает)
- [Совместимость](#совместимость)
- [Сборка](#сборка)
- [Структура проекта](#структура-проекта)
- [Лицензия](#лицензия)

### Возможности

Расширение Reaper Media Import добавляет в штатный импорт REAPER поддержку raw Dolby/DTS/MLP-аудио и субтитров ASS, SSA и SRT.

- Нативная поддержка импорта файлов данных форматов любом удобным способом (как через штатное контекстное меню, так и через горячие клавиши или перетаскивание в окно Reaper).
- Совместимо с Windows x64 и macOS Universal 2 с архитектурами Intel и Apple Silicon.
- Работа с полными shared-сборками FFmpeg 5.x–8.x без запуска `ffmpeg` или `ffprobe`.
- Коррекция длительности, частоты и числа каналов raw-аудио.
- Потоковое чтение `.thd+ac3` без промежуточного файла и дискового кэша.
- Преобразование субтитров в обычные Text Item при импорте.

### Быстрая установка расширения

1. Скачайте архив для своей платформы из [последнего релиза](../../releases/latest).
2. В REAPER откройте `Options → Show REAPER resource path in explorer/finder`.
3. Закройте REAPER и скопируйте `reaper_frzz_media_import.dll` или `reaper_frzz_media_import.dylib` в каталог `UserPlugins`.
4. Для корректной работы требуются shared-библиотеки FFmpeg 5.x–8.x, размещённые в каталоге `UserPlugins`.
5. В `Preferences → Media → Video/Import/Misc` укажите `ffmpeg_hw` в приоритете видеодекодеров и перезапустите REAPER.

Если macOS блокирует загруженный файл, снимите quarantine после копирования:

```bash
xattr -d com.apple.quarantine \
  "$HOME/Library/Application Support/REAPER/UserPlugins/reaper_frzz_media_import.dylib"
```

### Быстрая установка библиотек FFMPEG

#### Windows

1. Откройте раздел [Releases репозитория GyanD/codexffmpeg](https://github.com/GyanD/codexffmpeg/releases).
2. Выберите релиз **FFmpeg 8.1.2** и скачайте архив `ffmpeg-8.1.2-full_build-shared.7z`. Нужна именно shared-сборка: обычные `full_build` и `essentials_build` являются статическими и не содержат DLL, необходимые REAPER.
3. Распакуйте архив с помощью [7-Zip](https://www.7-zip.org/) или другого архиватора с поддержкой 7z.
4. Скопируйте все файлы `.dll` из каталога `bin` распакованного архива в каталог `UserPlugins` REAPER рядом с `reaper_frzz_media_import.dll`.
5. Запустите REAPER и убедитесь, что в `Preferences → Media → Video/Import/Misc` доступен декодер `ffmpeg_hw`.

#### macOS

1. Откройте приложение `Terminal`.
2. Если Homebrew ещё не установлен, установите его официальной командой:

   ```bash
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

   После установки выполните команды, которые Homebrew выведет в разделе `Next steps`, чтобы добавить `brew` в `PATH`, затем откройте новое окно Terminal.

3. Установите FFmpeg 8:

   ```bash
   brew install ffmpeg@8
   ```

4. Формула `ffmpeg@8` является keg-only, поэтому создайте ссылки на её библиотеки в каталоге `UserPlugins` REAPER:

   ```bash
   plugin_dir="$HOME/Library/Application Support/REAPER/UserPlugins"
   ffmpeg_lib="$(brew --prefix ffmpeg@8)/lib"
   mkdir -p "$plugin_dir"
   ln -sf "$ffmpeg_lib"/libavcodec*.dylib "$ffmpeg_lib"/libavformat*.dylib \
     "$ffmpeg_lib"/libavutil*.dylib "$ffmpeg_lib"/libswresample*.dylib \
     "$ffmpeg_lib"/libswscale*.dylib "$plugin_dir/"
   ```

### Поддерживаемые форматы

Расширение регистрирует 25 расширений без учёта регистра:

| Семейство | Расширения |
| --- | --- |
| Субтитры | `.srt`, `.ass`, `.ssa` |
| Dolby / MLP | `.ac3`, `.eac3`, `.ec3`, `.ddp`, `.ac4`, `.thd`, `.truehd`, `.thd+ac3`, `.mlp` |
| DTS | `.dts`, `.dtshd`, `.dts-hd`, `.dtsma`, `.dts-ma`, `.dts-hdma`, `.dts-hd-ma`, `.dtses`, `.dts-es`, `.dtsx`, `.dts-x`, `.dtsuhd`, `.dts-uhd` |

Кодек внутри raw-файла должен соответствовать его расширению. Переименование неподдерживаемого файла не делает его совместимым.

### Как это работает

Обычные Dolby, DTS и MLP-файлы используют штатный источник VIDEO REAPER. Расширение получает метаданные через FFmpeg и исправляет параметры источника, сохраняя нативное размещение и работу с аудио.

Комбинированный `.thd+ac3` содержит чередующиеся кадры TrueHD и AC3. Расширение строит компактный индекс в памяти, пропускает AC3 и передаёт TrueHD access units декодеру FFmpeg прямо из исходного файла. Индекс не учитывает пики и строится отдельно от них; после перезапуска REAPER он создаётся заново. Копия медиа на диск не записывается.

Новые raw-аудиоисточники сохраняются в проектах как `FRZZ_VIDEO`, поэтому для их повторного открытия расширение должно оставаться установленным.

Субтитры сначала полностью разбираются как UTF-8, затем создаются обычные текстовые items относительно фактической позиции импорта. Поддерживаются BOM, окончания строк LF/CRLF, ASS/SSA `[Events] Format`, миллисекунды SRT, удаление тегов `{...}` и преобразование `\N` в перенос строки. Runtime Lua не требуется.

### Совместимость

Целевая среда:

- REAPER 7.66 или новее.
- Windows x64 либо macOS 11+ на Intel или Apple Silicon.
- Полный shared FFmpeg 5.x–8.x с major ABI 59–62.

Проверены REAPER 7.79, FFmpeg 5.1.2, 6.1.1, 7.1.1 и 8.1.2 на Windows, а также Homebrew FFmpeg 8.1.2 на macOS Sequoia 15.7.1. Universal 2 библиотека проверена на Intel и реальном Apple Silicon. Версия FFmpeg 5.0 отдельно не испытывалась. REAPER 7.00 не входит в целевую конфигурацию.

Проверки охватывают DTS, DTS-HD, MLP, E-AC-3 и чередующийся TrueHD+AC3: импорт, длительность, параметры каналов, чтение аудио, пики, сохранение и повторное открытие проекта. AC4 и редкие варианты DTS-HD/ES/X/UHD без воспроизводимых образцов пока не заявлены как проверенные.

### Сборка

Клонируйте репозиторий вместе с submodules:

```bash
git clone --recursive <repository-url>
```

Для Windows нужны CMake 3.24+, Visual Studio 2022 Build Tools и Windows SDK:

```powershell
cmake --preset vs2022-x64
cmake --build --preset release
ctest --preset release
```

Результат: `build/Release/reaper_frzz_media_import.dll`.

Для macOS нужны CMake 3.24+ и Xcode Command Line Tools:

```bash
cmake --preset macos-universal
cmake --build --preset mac-universal-release
ctest --preset mac-universal-release
```

Результат: `build-macos-universal/reaper_frzz_media_import.dylib` с архитектурами `x86_64` и `arm64`.

Workflow `.github/workflows/ci.yml` собирает обе платформы при push и pull request. Тег `v1.0.0` создаёт GitHub Release с ZIP-архивами и SHA-256 checksums; суффикс, например `v1.0.0-rc.1`, создаёт prerelease.

### Структура проекта

| Путь | Назначение |
| --- | --- |
| `src/plugin.cpp` | Регистрация расширений, интеграция с REAPER и импорт субтитров |
| `src/media_metadata.cpp` | Метаданные raw-аудио и выбор загруженного FFmpeg ABI |
| `src/ffmpeg_scan.cpp` | Адаптеры FFmpeg 5.x–8.x |
| `src/thd_ac3.cpp` | Выбор FFmpeg ABI для `.thd+ac3` |
| `src/thd_ac3_decoder.cpp` | Индексирование и потоковое декодирование TrueHD |
| `src/subtitle_parser.cpp` | Парсер ASS, SSA и SRT |
| `tests/` | Автоматические тесты парсера |
| `.github/workflows/ci.yml` | CI и автоматическая публикация релизов |

REAPER SDK, WDL и заголовки FFmpeg зафиксированы в `external/`. Сторонние компоненты сохраняют собственные лицензии и уведомления в своих каталогах.

### Лицензия

Оригинальный код проекта: [MIT License](LICENSE), copyright © 2026 Andrew Brodsky.

## English

### Contents

- [Features](#features)
- [Quick extension installation](#quick-extension-installation)
- [Quick FFmpeg library installation](#quick-ffmpeg-library-installation)
- [Supported formats](#supported-formats)
- [How it works](#how-it-works)
- [Compatibility](#compatibility)
- [Building](#building)
- [Project layout](#project-layout)
- [License](#license)

### Features

Reaper Media Import extends REAPER's native media import with raw Dolby/DTS/MLP audio and ASS, SSA and SRT subtitles.

- Native file import through REAPER's standard context menu, keyboard shortcuts, or drag and drop.
- Compatible with Windows x64 and macOS Universal 2 on Intel and Apple Silicon.
- Full shared FFmpeg 5.x–8.x support without launching `ffmpeg` or `ffprobe`.
- Corrected duration, sample rate and channel metadata for raw audio.
- Streaming `.thd+ac3` access without a temporary file or disk cache.
- Subtitle conversion to regular Text Items during import.

### Quick extension installation

1. Download the package for your platform from the [latest release](../../releases/latest).
2. In REAPER, open `Options → Show REAPER resource path in explorer/finder`.
3. Close REAPER and copy `reaper_frzz_media_import.dll` or `reaper_frzz_media_import.dylib` to `UserPlugins`.
4. The extension requires shared FFmpeg 5.x–8.x libraries in `UserPlugins`.
5. Put `ffmpeg_hw` in the decoder priority under `Preferences → Media → Video/Import/Misc`, then restart REAPER.

If macOS blocks the downloaded binary, remove quarantine after copying it:

```bash
xattr -d com.apple.quarantine \
  "$HOME/Library/Application Support/REAPER/UserPlugins/reaper_frzz_media_import.dylib"
```

### Quick FFmpeg library installation

#### Windows

1. Open the [GyanD/codexffmpeg Releases](https://github.com/GyanD/codexffmpeg/releases) page.
2. Select the **FFmpeg 8.1.2** release and download `ffmpeg-8.1.2-full_build-shared.7z`. Use the shared package: the regular `full_build` and `essentials_build` packages are static and do not contain the DLLs required by REAPER.
3. Extract the archive with [7-Zip](https://www.7-zip.org/) or another archiver that supports 7z.
4. Copy every `.dll` file from the extracted `bin` directory to REAPER's `UserPlugins` directory next to `reaper_frzz_media_import.dll`.
5. Start REAPER and confirm that `ffmpeg_hw` is available under `Preferences → Media → Video/Import/Misc`.

#### macOS

1. Open the `Terminal` application.
2. If Homebrew is not installed, install it with the official command:

   ```bash
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

   After installation, run the commands shown by Homebrew under `Next steps` to add `brew` to `PATH`, then open a new Terminal window.

3. Install FFmpeg 8:

   ```bash
   brew install ffmpeg@8
   ```

4. The `ffmpeg@8` formula is keg-only, so create links to its libraries in REAPER's `UserPlugins` directory:

   ```bash
   plugin_dir="$HOME/Library/Application Support/REAPER/UserPlugins"
   ffmpeg_lib="$(brew --prefix ffmpeg@8)/lib"
   mkdir -p "$plugin_dir"
   ln -sf "$ffmpeg_lib"/libavcodec*.dylib "$ffmpeg_lib"/libavformat*.dylib \
     "$ffmpeg_lib"/libavutil*.dylib "$ffmpeg_lib"/libswresample*.dylib \
     "$ffmpeg_lib"/libswscale*.dylib "$plugin_dir/"
   ```

### Supported formats

The extension registers 25 case-insensitive file extensions:

| Family | Extensions |
| --- | --- |
| Subtitles | `.srt`, `.ass`, `.ssa` |
| Dolby / MLP | `.ac3`, `.eac3`, `.ec3`, `.ddp`, `.ac4`, `.thd`, `.truehd`, `.thd+ac3`, `.mlp` |
| DTS | `.dts`, `.dtshd`, `.dts-hd`, `.dtsma`, `.dts-ma`, `.dts-hdma`, `.dts-hd-ma`, `.dtses`, `.dts-es`, `.dtsx`, `.dts-x`, `.dtsuhd`, `.dts-uhd` |

The codec inside a raw file must match its extension family. Renaming an unsupported file does not make it compatible.

### How it works

Regular Dolby, DTS and MLP files use REAPER's native VIDEO source. The extension reads metadata through FFmpeg and corrects the source properties while preserving native placement and audio editing.

An interleaved `.thd+ac3` file contains alternating TrueHD and AC3 frames. The extension builds a compact in-memory index, skips AC3 and streams TrueHD access units from the original file directly into FFmpeg. Index construction is separate from peak calculation, the index is rebuilt after REAPER restarts, and no media copy is written to disk.

New raw audio sources are stored in projects as `FRZZ_VIDEO`, so the extension must remain installed when those projects are reopened.

Subtitle files are parsed as UTF-8 before regular text items are created at the actual import position. The parser supports BOM, LF/CRLF line endings, ASS/SSA `[Events] Format`, SRT milliseconds, `{...}` tag removal and `\N` line breaks. No Lua runtime is required.

### Compatibility

Target environment:

- REAPER 7.66 or newer.
- Windows x64 or macOS 11+ on Intel or Apple Silicon.
- Full shared FFmpeg 5.x–8.x with major ABI 59–62.

Testing covers REAPER 7.79; FFmpeg 5.1.2, 6.1.1, 7.1.1 and 8.1.2 on Windows; and Homebrew FFmpeg 8.1.2 on macOS Sequoia 15.7.1. The Universal 2 binary was tested on Intel and real Apple Silicon hardware. FFmpeg 5.0 was not tested separately. REAPER 7.00 is outside the target configuration.

DTS, DTS-HD, MLP, E-AC-3 and interleaved TrueHD+AC3 were checked for import, duration, channel metadata, audio reads, peak generation, project saving and reopening. AC4 and uncommon DTS-HD/ES/X/UHD variants without reproducible samples are not yet claimed as verified.

### Building

Clone the repository with its submodules:

```bash
git clone --recursive <repository-url>
```

Windows requires CMake 3.24+, Visual Studio 2022 Build Tools and the Windows SDK:

```powershell
cmake --preset vs2022-x64
cmake --build --preset release
ctest --preset release
```

Output: `build/Release/reaper_frzz_media_import.dll`.

macOS requires CMake 3.24+ and Xcode Command Line Tools:

```bash
cmake --preset macos-universal
cmake --build --preset mac-universal-release
ctest --preset mac-universal-release
```

Output: `build-macos-universal/reaper_frzz_media_import.dylib` with `x86_64` and `arm64` slices.

The `.github/workflows/ci.yml` workflow builds both platforms on pushes and pull requests. A `v1.0.0` tag creates a GitHub Release with ZIP packages and SHA-256 checksums; a suffix such as `v1.0.0-rc.1` creates a prerelease.

### Project layout

| Path | Purpose |
| --- | --- |
| `src/plugin.cpp` | Extension registration, REAPER integration and subtitle import |
| `src/media_metadata.cpp` | Raw audio metadata and loaded FFmpeg ABI selection |
| `src/ffmpeg_scan.cpp` | FFmpeg 5.x–8.x adapters |
| `src/thd_ac3.cpp` | FFmpeg ABI selection for `.thd+ac3` |
| `src/thd_ac3_decoder.cpp` | TrueHD indexing and streaming decoder |
| `src/subtitle_parser.cpp` | ASS, SSA and SRT parser |
| `tests/` | Automated parser tests |
| `.github/workflows/ci.yml` | CI and automated release publishing |

The REAPER SDK, WDL and FFmpeg headers are pinned under `external/`. Third-party components retain their own licenses and notices in those directories.

### License

Original project code: [MIT License](LICENSE), copyright © 2026 Andrew Brodsky.
