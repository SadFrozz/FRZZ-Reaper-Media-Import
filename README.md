# FRZZ REAPER Media Import

A native extension that builds on REAPER's built-in FFmpeg support on Windows and macOS and works with FFmpeg 5.x–9.x. Drag standalone AC-3 (Dolby Digital), E-AC-3 (Dolby Digital Plus), or DTS files directly into REAPER without converting to WAV or remuxing into MKV or MP4. The extension also adds MKV/MP4 audio track selection, subtitle import, and hardware HDR-to-SDR tone mapping.

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

FRZZ REAPER Media Import расширяет штатную поддержку FFmpeg в REAPER. Работает с FFmpeg 5.x–9.x и добавляет новые возможности для аудио, видео и субтитров.

- Отдельные файлы AC-3 (Dolby Digital), E-AC-3 (Dolby Digital Plus), Dolby TrueHD, DTS, DTS-HD и MLP можно перетащить прямо в проект. Конвертация в WAV, переименование и упаковка в MKV или MP4 не требуются.
- [Выбор аудиодорожки при импорте MKV и MP4](#выбор-аудиодорожки-из-mkv-или-mp4) по языку, названию, кодеку и каналам.
- Импорт ASS, SSA, SRT и WebVTT в обычные текстовые айтемы REAPER.
- Аппаратный тонмаппинг HDR10, HDR10+ и Dolby Vision в SDR BT.709 и исправления аппаратного декодирования HEVC на Windows x64 и Intel Mac.
- Совместимо с Windows x64 и macOS Universal 2 с архитектурами Intel и Apple Silicon.
- Работа с полными shared-сборками FFmpeg 5.x–9.x без запуска `ffmpeg` или `ffprobe`.
- Коррекция длительности, частоты и числа каналов raw-аудио.
- Потоковое чтение `.thd+ac3` без промежуточного файла и дискового кэша.

### Быстрая установка расширения

1. Скачайте архив для своей платформы из [последнего релиза](../../releases/latest).
2. В REAPER откройте `Options → Show REAPER resource path in explorer/finder`.
3. Закройте REAPER и скопируйте `reaper_frzz_media_import.dll` или `reaper_frzz_media_import.dylib` в каталог `UserPlugins`.
4. Для корректной работы требуются shared-библиотеки FFmpeg 5.x–9.x, размещённые в каталоге `UserPlugins`.
5. В `Preferences → Media → Video/Import/Misc` укажите `ffmpeg_hw` в приоритете видеодекодеров и перезапустите REAPER.

Если macOS блокирует загруженный файл, снимите quarantine после копирования.

```bash
xattr -d com.apple.quarantine \
  "$HOME/Library/Application Support/REAPER/UserPlugins/reaper_frzz_media_import.dylib"
```

### Быстрая установка библиотек FFMPEG

Ниже приведена установка FFmpeg 9 для REAPER 7.80 и новее. Для REAPER 7.66–7.79 используйте shared-сборку FFmpeg 5.x–8.x. Для FFmpeg 8 на macOS замените `ffmpeg@9` на `ffmpeg@8` в командах ниже.

#### Windows

1. Откройте раздел [Releases репозитория GyanD/codexffmpeg](https://github.com/GyanD/codexffmpeg/releases).
2. Выберите релиз **FFmpeg 9.0.2** и скачайте архив `ffmpeg-9.0.2-full_build-shared.7z`. Нужна именно shared-сборка: обычные `full_build` и `essentials_build` являются статическими и не содержат DLL, необходимые REAPER.
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

3. Установите FFmpeg 9:

   ```bash
   brew install ffmpeg@9
   ```

4. Создайте ссылки на библиотеки FFmpeg в каталоге `UserPlugins` REAPER:

   ```bash
   plugin_dir="$HOME/Library/Application Support/REAPER/UserPlugins"
   ffmpeg_lib="$(brew --prefix ffmpeg@9)/lib"
   mkdir -p "$plugin_dir"
   ln -sf "$ffmpeg_lib"/libavcodec*.dylib "$ffmpeg_lib"/libavformat*.dylib \
     "$ffmpeg_lib"/libavutil*.dylib "$ffmpeg_lib"/libswresample*.dylib \
     "$ffmpeg_lib"/libswscale*.dylib "$plugin_dir/"
   ```

### Поддерживаемые форматы

Расширение регистрирует 26 расширений без учёта регистра:

| Семейство | Расширения |
| --- | --- |
| Субтитры | `.srt`, `.ass`, `.ssa`, `.vtt` |
| Dolby / MLP | `.ac3`, `.eac3`, `.ec3`, `.ddp`, `.ac4`, `.thd`, `.truehd`, `.thd+ac3`, `.mlp` |
| DTS | `.dts`, `.dtshd`, `.dts-hd`, `.dtsma`, `.dts-ma`, `.dts-hdma`, `.dts-hd-ma`, `.dtses`, `.dts-es`, `.dtsx`, `.dts-x`, `.dtsuhd`, `.dts-uhd` |

Кодек внутри raw-файла должен соответствовать его расширению. Переименование неподдерживаемого файла не делает его совместимым.

### Как это работает

В `Preferences → Plug-ins → Media Import` можно отключить выбор аудиодорожек, преобразование HDR и Dolby Vision в SDR, исправления HEVC и пропуск кадров при перемотке HEVC. Все четыре настройки включены по умолчанию там, где поддерживаются. Пропуск доступен при включённых исправлениях HEVC. После `Apply` новые импорты и запросы видеокадров используют обновлённые настройки без перезапуска.

При отключении мультиаудио MKV и MP4 импортируются штатно, а ранее импортированные тейки воспроизводят дорожку по умолчанию. Повторное включение возвращает дорожку, сохранённую в тейке.

#### Выбор аудиодорожки из MKV или MP4

При обычной вставке многодорожечного MKV или MP4 открывается окно выбора с языком, названием, кодеком, числом каналов и их порядком. Если в файле есть видео, REAPER вставляет его с выбранной аудиодорожкой и читает оба потока из исходного файла. **Selected default audiotrack** и Escape выбирают доступную дорожку, помеченную в контейнере как дорожка по умолчанию, или первую доступную при отсутствии такой отметки. Один Undo отменяет вставку, а Redo возвращает видео с выбранной дорожкой.

Пункт **Audio track** находится сразу после **Take markers** в контекстном меню айтема и в верхнем меню **Item**. Если в выделении несколько видео с несколькими аудиодорожками, сначала выберите видео. Если такое видео одно, меню сразу покажет его дорожки. Действие **Change Audio Track for selected video item** с тулбара открывает выпадающее меню. Из **Action List** оно открывает окно выбора дорожки. Если подходят несколько выбранных видео, перед ним открывается окно **Choose video item**.

Видео остаётся в том же тейке. Смена дорожки занимает одно действие Undo. Выбранная дорожка сохраняется в тейке и восстанавливается при открытии проекта. Если включена автоматическая генерация пиков, REAPER достраивает отсутствующие пики выбранной дорожки с текущими настройками.

Первый переход далеко вперёд в длинном MKV с округлёнными временными метками может занять больше времени. Расширение уточняет позиции аудиокадров, чтобы при повторном чтении получить те же сэмплы.

Из выбранной дорожки с высотными каналами импортируются основные каналы и LFE. Например, 7.1.2 импортируется как 7.1 без подмешивания верхней пары. Окно выбора показывает каналы полученного источника.

Если в файле меньше двух аудиопотоков или для выбранного формата включён `ffmpeg_mta`, видео импортируется без меню выбора.

На Windows x64 (проверенные сборки REAPER 7.66, 7.77, 7.85 и 7.86) и на macOS Intel (REAPER 7.85 с FFmpeg 9) для штатного источника VIDEO REAPER управляет очередью, окном **Building Peaks…** и обновлением волны по мере построения. Для каждого потока используется отдельный файл `.trackN.reapeaks`. При импорте и открытии проекта нативный источник попадает в очередь REAPER штатно. Если при смене дорожки её кэш отсутствует или неполный, расширение запрашивает построение с текущими настройками REAPER. Штатный источник VIDEO декодирует выбранный поток через FFmpeg, когда его временные метки и раскладка каналов это позволяют. Если выбранная дорожка начинается позже видео, расширение проверяет первый декодированный кадр и сохраняет начальную паузу в штатном источнике. Для потоков, которые штатный источник не может воспроизвести с нужным таймингом или порядком каналов, расширение читает сэмплы напрямую через FFmpeg.

#### Raw-аудио и внешние субтитры

REAPER получает Dolby, DTS и MLP как аудиоисточники. Расширение считывает точную длительность, частоту и число каналов через FFmpeg и использует штатный декодер REAPER для получения аудиосэмплов.

Комбинированный `.thd+ac3` содержит чередующиеся кадры TrueHD и AC3. Расширение строит компактный индекс в памяти, пропускает AC3 и передаёт TrueHD access units декодеру FFmpeg прямо из исходного файла. Индекс не учитывает пики и строится отдельно от них; после перезапуска REAPER он создаётся заново. Копия медиа на диск не записывается.

REAPER сохраняет эти аудиоисточники в проекте как `FRZZ_AUDIO`. Расширение также открывает источники `FRZZ_VIDEO` из старых проектов и при следующем сохранении записывает их как `FRZZ_AUDIO`. Для повторного открытия после пересохранения нужна версия расширения с поддержкой `FRZZ_AUDIO`.

Субтитры в UTF-8 и UTF-16 LE/BE с BOM импортируются как обычные текстовые айтемы относительно фактической позиции импорта. Поддерживаются BOM, окончания строк LF/CRLF/CR и нулевая длительность реплик.

ASS/SSA учитывает секцию [Events] и порядок полей Format, сохраняет Actor и запятые в тексте, убирает служебные теги и команды рисования. Реплики с неизвестным стилем и разными слоями сохраняют текст и тайминг, включая пересечения. Оформление и визуальный порядок слоёв ASS применяются только при показе субтитров в плеере.

SRT поддерживает миллисекунды, нумерованные и ненумерованные реплики, пустые строки внутри реплики и до 10 строк исходного текста на айтем. Оформительские теги вроде текста курсивом убираются.

WebVTT импортирует реплики с полным таймингом и многострочным текстом. Имя из `<v>` переносится в заметку как `[Имя]`. Стили, регионы и настройки расположения предназначены для отображения субтитров в плеере.

Импорт субтитров занимает одну запись в истории Undo REAPER. Одно действие Redo восстанавливает текстовые айтемы. Если до конвертации укоротить временный айтем, он сохраняет заданные границы.

### Совместимость

Целевая среда:

- REAPER 7.66 или новее. Для FFmpeg 9.x нужен REAPER 7.80 или новее.
- Windows x64 либо macOS 11+ на Intel или Apple Silicon.
- Полный shared FFmpeg 5.x–9.x с major ABI libavcodec/libavformat 59–63.

Аппаратный тонмаппинг включён при сборке по умолчанию на Windows x64 и Intel Mac. Настроить сборку можно через `FRZZ_ENABLE_WINDOWS_HDR` и `FRZZ_ENABLE_MACOS_HDR`. Расширение использует ABI FFmpeg 5–9, который загрузил REAPER.

Кривые тонмаппинга и обработка метаданных Dolby Vision адаптированы из libplacebo и компилируются вместе с расширением. Шейдеры работают через D3D11 на Windows и OpenGL на Intel Mac.

На Windows аппаратный путь также может подключиться к незнакомой сборке REAPER, если в её видеомодуле однозначно находятся необходимые функции загруженного FFmpeg. Штатный выбор аудиодорожек по-прежнему использует проверенные точки подключения конкретных сборок REAPER.

На Windows кэш HDR-шейдеров сохраняется в `UserPlugins/Shaders`. При отсутствии подходящего кэша расширение готовит общие шейдеры в фоне при запуске REAPER. Обновление графического драйвера или изменение набора видеокарт запускает подготовку заново. Метаданные RPU передаются шейдеру для каждого кадра.

При перемотке аппаратного HEVC расширение повторяет подачу пакета, если декодер временно вернул `EAGAIN`. На поддерживаемых потоках оно также может не отдавать ранние кадры на вывод и пропускать неопорные кадры до запрошенного участка. При рендере сохраняются аппаратное декодирование, эти исправления и тонмаппинг исходного видео.

Для Intel Arc применяется общий путь HDR, если REAPER выбирает декодирование D3D11VA. Дополнительный экспериментальный путь QSV включается флагами CMake `FRZZ_ENABLE_WINDOWS_HDR=ON` и `FRZZ_ENABLE_WINDOWS_QSV=ON`. `FRZZ_ONEVPL_INCLUDE_DIR` указывает на каталог заголовков SDK oneVPL 2.x, содержащий `vpl/mfxstructures.h`. Тонмаппинг QSV требует кадр P010 с публичным callback готовности oneVPL и прямым отображением в текстуру D3D11 через FFmpeg. На Intel UHD 770 штатный `hevc_qsv` из FFmpeg 9.0.2 выдавал кадры без такого callback, и их обработка оставалась в FFmpeg. Работа на реальном Intel Arc пока не проверена.

На Windows в REAPER 7.85 с FFmpeg 8.1.2 и 9.0.2 проверены импорт E-AC-3, DTS, MLP, TrueHD и TrueHD+AC3, чтение аудио, повторное открытие проекта и миграция `FRZZ_VIDEO`.

На Windows импорт MKV и MP4 с выбором дорожки проверен в REAPER 7.77 с FFmpeg 5.0, 5.1, 6.1, 7.1 и 8.1, а также в REAPER 7.85 с FFmpeg 9.0.2. Проверены два айтема с разными дорожками одного файла, Undo/Redo, повторное открытие проекта и повторное использование отдельных кэшей пиков. В предыдущих прогонах на том же диапазоне версий REAPER, включая 7.66 с FFmpeg 8.1, также проверены дорожка по умолчанию и изображение в окне Video. На Intel Mac в REAPER 7.85 проверено восстановление двух выбранных дорожек с разной задержкой на FFmpeg 5.0, 8.1 и 9.0.2.

На macOS Intel (Sequoia 15.7.1) в REAPER 7.85 проверена shared-сборка FFmpeg 9.0.2 из релизных исходников. Проверены импорт, чтение и повторное открытие E-AC-3, DTS, DTS-HD, MLP, TrueHD и TrueHD+AC3. Для текущей версии также собран срез arm64.

Для версии 1.0.0 проверены REAPER 7.79, FFmpeg 5.1.2, 6.1.1, 7.1.1 и 8.1.2 на Windows, а также Homebrew FFmpeg 8.1.2 на macOS Sequoia 15.7.1. Universal 2 библиотека проверена на Intel и реальном Apple Silicon. Версия FFmpeg 5.0 отдельно не испытывалась. REAPER 7.00 не входит в целевую конфигурацию.

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

Публикация тега версии, например `v1.1.1`, запускает сборку Windows и macOS и создаёт GitHub Release с ZIP-архивами и SHA-256. Тег с суффиксом, например `v1.1.1-rc.1`, создаёт prerelease.

Для ручной сборки откройте **Actions → Build, test and release → Run workflow**. Выберите ветку, затем коммит по короткому SHA и сообщению. Пункт `HEAD — latest commit in the selected branch` собирает вершину выбранной ветки. Список исторических коммитов задан в workflow и обновляется вместе с его файлом. Архивы будут доступны в Artifacts запуска.

### Структура проекта

| Путь | Назначение |
| --- | --- |
| `src/` | Код расширения — интеграция с REAPER, импорт аудио и субтитров, выбор дорожек в видео |
| `tests/` | Автоматические тесты парсинга, аудио и пиков |
| `external/` | Зафиксированные зависимости — REAPER SDK, WDL и заголовки FFmpeg |
| `CMakeLists.txt` | Сборка расширения и тестов |
| `.github/workflows/` | Сборка, тестирование и публикация релизов на GitHub |

Сторонние компоненты сохраняют собственные лицензии и уведомления в своих каталогах.

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

FRZZ REAPER Media Import extends REAPER's built-in FFmpeg support. It works with FFmpeg 5.x–9.x and adds features for audio, video, and subtitles.

- Drag standalone AC-3 (Dolby Digital), E-AC-3 (Dolby Digital Plus), Dolby TrueHD, DTS, DTS-HD, and MLP audio directly into a project. No conversion to WAV, renaming, or remuxing is required.
- [Audio track selection when importing MKV and MP4](#selecting-an-audio-track-from-mkv-or-mp4) by language, title, codec, and channels.
- Import ASS, SSA, SRT, and WebVTT subtitles as regular REAPER text items.
- Hardware tone mapping from HDR10, HDR10+, and Dolby Vision to SDR BT.709, plus fixes for hardware HEVC decoding on Windows x64 and Intel Mac.
- Compatible with Windows x64 and macOS Universal 2 on Intel and Apple Silicon.
- Full shared FFmpeg 5.x–9.x support without launching `ffmpeg` or `ffprobe`.
- Corrected duration, sample rate and channel metadata for raw audio.
- Streaming `.thd+ac3` access without a temporary file or disk cache.

### Quick extension installation

1. Download the package for your platform from the [latest release](../../releases/latest).
2. In REAPER, open `Options → Show REAPER resource path in explorer/finder`.
3. Close REAPER and copy `reaper_frzz_media_import.dll` or `reaper_frzz_media_import.dylib` to `UserPlugins`.
4. The extension requires shared FFmpeg 5.x–9.x libraries in `UserPlugins`.
5. Put `ffmpeg_hw` in the decoder priority under `Preferences → Media → Video/Import/Misc`, then restart REAPER.

If macOS blocks the downloaded binary, remove quarantine after copying it.

```bash
xattr -d com.apple.quarantine \
  "$HOME/Library/Application Support/REAPER/UserPlugins/reaper_frzz_media_import.dylib"
```

### Quick FFmpeg library installation

The steps below install FFmpeg 9 for REAPER 7.80 or later. For REAPER 7.66–7.79, use a shared build of FFmpeg 5.x–8.x. To install FFmpeg 8 on macOS, replace `ffmpeg@9` with `ffmpeg@8` in the commands below.

#### Windows

1. Open the [GyanD/codexffmpeg Releases](https://github.com/GyanD/codexffmpeg/releases) page.
2. Select the **FFmpeg 9.0.2** release and download `ffmpeg-9.0.2-full_build-shared.7z`. Use the shared package: the regular `full_build` and `essentials_build` packages are static and do not contain the DLLs required by REAPER.
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

3. Install FFmpeg 9:

   ```bash
   brew install ffmpeg@9
   ```

4. Create links to the FFmpeg libraries in REAPER's `UserPlugins` directory:

   ```bash
   plugin_dir="$HOME/Library/Application Support/REAPER/UserPlugins"
   ffmpeg_lib="$(brew --prefix ffmpeg@9)/lib"
   mkdir -p "$plugin_dir"
   ln -sf "$ffmpeg_lib"/libavcodec*.dylib "$ffmpeg_lib"/libavformat*.dylib \
     "$ffmpeg_lib"/libavutil*.dylib "$ffmpeg_lib"/libswresample*.dylib \
     "$ffmpeg_lib"/libswscale*.dylib "$plugin_dir/"
   ```

### Supported formats

The extension registers 26 case-insensitive file extensions:

| Family | Extensions |
| --- | --- |
| Subtitles | `.srt`, `.ass`, `.ssa`, `.vtt` |
| Dolby / MLP | `.ac3`, `.eac3`, `.ec3`, `.ddp`, `.ac4`, `.thd`, `.truehd`, `.thd+ac3`, `.mlp` |
| DTS | `.dts`, `.dtshd`, `.dts-hd`, `.dtsma`, `.dts-ma`, `.dts-hdma`, `.dts-hd-ma`, `.dtses`, `.dts-es`, `.dtsx`, `.dts-x`, `.dtsuhd`, `.dts-uhd` |

The codec inside a raw file must match its extension family. Renaming an unsupported file does not make it compatible.

### How it works

Use `Preferences → Plug-ins → Media Import` to toggle audio track selection, HDR and Dolby Vision conversion to SDR, HEVC playback fixes, and frame skipping during HEVC seeks. All four settings are enabled by default where supported. Frame skipping requires the HEVC playback fixes. After you click `Apply`, new imports and video frame requests use the updated settings without a restart.

With multiple audio tracks disabled, REAPER imports MKV and MP4 as usual, and existing takes play the default track. Re-enabling the feature restores the track saved in the take.

#### Selecting an audio track from MKV or MP4

Insert an MKV or MP4 using REAPER's regular media import. The multitrack dialog shows language, title, codec, channel count, and channel order when available. When the file contains video, REAPER inserts it with the selected audio track and reads both from the original file. **Selected default audiotrack** or Escape uses the playable track marked as default in the container, or the first playable track when no default is marked. A single Undo removes the item, and Redo restores the video with the selected track.

Choose **Audio track** just below **Take markers** in the item context menu or the main **Item** menu. If the selection contains multiple videos with more than one audio track, choose the video first. If only one such video is selected, the menu shows its tracks directly. **Change Audio Track for selected video item** opens a popup from a toolbar. From the **Action List**, it opens an audio track dialog. If multiple selected videos qualify, it first opens the **Choose video item** dialog.

The video stays in the same take. The change takes one Undo step. When automatic peak generation is enabled, REAPER builds missing peaks for that stream using the current settings.

The first seek far into a long MKV with rounded timestamps can take longer while the extension indexes audio frames. This makes repeated reads return the same samples.

For a selected track with height channels, the extension imports the main channels and LFE. For example, 7.1.2 imports as 7.1 without mixing in the height pair. The track dialog shows the resulting source channels.

Files with fewer than two audio streams and formats with `ffmpeg_mta` enabled are imported without a track menu.

The audio stream choice is saved with the take and restored when the project opens. On Windows x64 (verified REAPER builds 7.66, 7.77, 7.85 and 7.86) and macOS Intel (REAPER 7.85 with FFmpeg 9), REAPER manages the queue, the **Building Peaks…** window, and waveform updates for native VIDEO sources. Each stream has its own `.trackN.reapeaks` file. REAPER queues native sources during import and project loading. Switching a track requests a build if its cache is missing or incomplete, using the current REAPER settings. The native VIDEO source decodes the selected stream through FFmpeg when its timing and channel layout permit. When the selected stream starts later than the video, the extension verifies the first decoded frame and preserves the initial silence in the native source. The extension reads samples directly through FFmpeg for streams the native source cannot play with the required timing or channel order.

#### Raw audio and external subtitles

REAPER receives Dolby, DTS and MLP files as audio sources. The extension reads the exact duration, sample rate and channel count through FFmpeg and uses REAPER's built-in decoder to read audio samples.

An interleaved `.thd+ac3` file contains alternating TrueHD and AC3 frames. The extension builds a compact in-memory index, skips AC3 and streams TrueHD access units from the original file directly into FFmpeg. Index construction is separate from peak calculation, the index is rebuilt after REAPER restarts, and no media copy is written to disk.

REAPER saves these audio sources as `FRZZ_AUDIO`. The extension also opens `FRZZ_VIDEO` sources from existing projects and saves them as `FRZZ_AUDIO` on the next save. Reopening a resaved project requires a version of the extension that supports `FRZZ_AUDIO`.

UTF-8 subtitles and UTF-16 LE/BE subtitles with a BOM import as regular text items at the actual insertion position. The parser supports a BOM, LF/CRLF/CR line endings, and zero-duration cues.

ASS/SSA honors the [Events] section and Format column order, preserves Actor names and text commas, and strips override tags and drawing commands. Cues with unknown styles or different layers retain their text and timing, including overlaps. ASS styling and visual layer order apply when rendering subtitles in a player.

SRT supports millisecond timing, numbered and unnumbered cues, blank lines within a cue, and up to 10 source text lines per item. Formatting tags such as italics are stripped.

WebVTT imports cue timing and multiline text. The voice name from `<v>` appears as `[Name]` in the note. Styles, regions, and layout settings apply to subtitle rendering in a player.

Subtitle import creates one entry in REAPER’s undo history. One redo restores the text items. If the placeholder is shortened before conversion, it retains its trimmed bounds.

### Compatibility

Target environment:

- REAPER 7.66 or newer. FFmpeg 9.x requires REAPER 7.80 or newer.
- Windows x64 or macOS 11+ on Intel or Apple Silicon.
- Full shared FFmpeg 5.x–9.x with major ABI libavcodec/libavformat 59–63.

Hardware tone mapping is enabled by default when building for Windows x64 and Intel Mac. Use `FRZZ_ENABLE_WINDOWS_HDR` and `FRZZ_ENABLE_MACOS_HDR` to configure the build. The extension uses the FFmpeg 5–9 ABI loaded by REAPER.

The tone-mapping curves and Dolby Vision metadata handling are adapted from libplacebo and compiled into the extension. The shaders use D3D11 on Windows and OpenGL on Intel Mac.

On Windows, the hardware path can also run on an unfamiliar REAPER build when its video module exposes an unambiguous match for the required functions of the loaded FFmpeg. Native audio-track selection still uses the verified entry points for specific REAPER builds.

On Windows, the HDR shader cache is stored in `UserPlugins/Shaders`. When a matching cache is unavailable, the extension prepares common shaders in the background at REAPER startup. Updating the graphics driver or changing the set of graphics adapters triggers preparation again. The shader receives RPU metadata for each frame.

When seeking hardware-decoded HEVC, the extension retries submitting a packet if the decoder temporarily returns `EAGAIN`. For supported streams, it can also suppress earlier output and skip non-reference pictures before the requested range. Rendering retains hardware decoding, these fixes, and tone mapping of the original video.

Intel Arc uses the common HDR path when REAPER selects D3D11VA decoding. The additional experimental QSV path requires `FRZZ_ENABLE_WINDOWS_HDR=ON` and `FRZZ_ENABLE_WINDOWS_QSV=ON`. Set `FRZZ_ONEVPL_INCLUDE_DIR` to the oneVPL 2.x SDK include directory containing `vpl/mfxstructures.h`. QSV tone mapping requires a ready P010 surface with a public oneVPL readiness callback and a direct FFmpeg mapping to a D3D11 texture. On Intel UHD 770, stock `hevc_qsv` from FFmpeg 9.0.2 returned frames without that callback, so FFmpeg handled their transfer. This has yet to be tested on Intel Arc hardware.

On Windows, REAPER 7.85 with FFmpeg 8.1.2 and 9.0.2 was tested for E-AC-3, DTS, MLP, TrueHD and TrueHD+AC3 import, audio reads, project reopening and `FRZZ_VIDEO` migration.

On Windows, MKV and MP4 import with audio track selection was tested in REAPER 7.77 with FFmpeg 5.0, 5.1, 6.1, 7.1 and 8.1, and in REAPER 7.85 with FFmpeg 9.0.2. The tests covered two items with different tracks from the same file, Undo/Redo, reopening the project, and reuse of separate peak caches. Earlier tests, including REAPER 7.66 with FFmpeg 8.1, also covered the default track and the picture in the Video window. On Intel Mac, REAPER 7.85 restored two selected tracks with different start offsets on FFmpeg 5.0, 8.1 and 9.0.2.

On macOS Intel (Sequoia 15.7.1), REAPER 7.85 was tested against shared FFmpeg 9.0.2 built from the release source. Import, audio reads, and reopening passed for E-AC-3, DTS, DTS-HD, MLP, TrueHD, and TrueHD+AC3. The arm64 slice of the current version was also built.

Testing for version 1.0.0 covers REAPER 7.79; FFmpeg 5.1.2, 6.1.1, 7.1.1 and 8.1.2 on Windows; and Homebrew FFmpeg 8.1.2 on macOS Sequoia 15.7.1. The Universal 2 binary was tested on Intel and real Apple Silicon hardware. FFmpeg 5.0 was not tested separately. REAPER 7.00 is outside the target configuration.

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

Pushing a version tag such as `v1.1.1` builds Windows and macOS and publishes a GitHub Release with ZIP packages and SHA-256 checksums. A tag with a suffix such as `v1.1.1-rc.1` creates a prerelease.

For a manual build, open **Actions → Build, test and release → Run workflow**. Pick a branch, then select a commit by its short SHA and message. `HEAD — latest commit in the selected branch` builds the branch tip. Historical choices are defined in the workflow file and updated with it. The packages appear under the run’s Artifacts.

### Project layout

| Path | Purpose |
| --- | --- |
| `src/` | Extension code for REAPER integration, audio and subtitle import, and video audio track selection |
| `tests/` | Automated tests for parsing, audio, and peaks |
| `external/` | Pinned dependencies including the REAPER SDK, WDL, and FFmpeg headers |
| `CMakeLists.txt` | Extension and test build configuration |
| `.github/workflows/` | GitHub builds, tests, and release publishing |

Third-party components retain their own licenses and notices in their directories.

### License

Original project code: [MIT License](LICENSE), copyright © 2026 Andrew Brodsky.
