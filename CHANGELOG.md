# Changelog / Журнал изменений

[Русский](#русский) · [English](#english)

## v1.3.0 — 2026-11-24

### Русский

- Добавлена страница настроек Media Import с переключателями выбора аудиодорожек, тонмаппинга, исправлений HEVC и пропуска кадров при перемотке.
- Настройки применяются без перезапуска REAPER. При отключении мультиаудио новые файлы импортируются штатно, а существующие тейки воспроизводят дорожку по умолчанию. Повторное включение возвращает сохранённую дорожку.
- Добавлен аппаратный тонмаппинг в SDR Rec.709 для HDR10, HDR10+ и Dolby Vision. Для Dolby Vision Profile 5 применяется RPU, для Profile 7 используется базовый слой с RPU, для Profile 8.1 используется базовый HDR10.
- Код кривых тонмаппинга и обработки RPU из libplacebo встроен в расширение. Для сборки используются штатные шейдеры D3D11 и OpenGL.
- Восстановленный видеоайтем с выбранной аудиодорожкой подключает обработку HDR до первой отрисовки при открытии проекта. На Windows обработка возобновляется и после штатной выгрузки в P010, если буфер выделен по запросу расширения.
- На Windows компилируемые HDR-шейдеры кэшируются в `UserPlugins/Shaders`. После обновления драйвера или смены набора видеокарт кэш подготавливается заново.
- Для аппаратного HEVC исправлена подача пакетов после `EAGAIN`. При перемотке расширение может подавлять вывод кадров до запрошенного участка и пропускать подходящие неопорные кадры. При рендере с оригинала сохраняются обработка HDR, исправление подачи пакетов и оптимизации перемотки.

### English

- Added a Media Import preferences page with controls for audio track selection, tone mapping, HEVC fixes, and frame skipping when seeking.
- Settings take effect without restarting REAPER. Disabling multiple audio tracks restores standard import and makes existing takes play the default track. Re-enabling the feature restores the selected track saved in each take.
- Added hardware tone mapping to SDR Rec.709 for HDR10, HDR10+, and Dolby Vision. Dolby Vision Profile 5 uses RPU metadata, Profile 7 uses the base layer with RPU, and Profile 8.1 uses the HDR10 base layer.
- Integrated the libplacebo tone-mapping curves and RPU handling into the extension with native D3D11 and OpenGL shaders.
- When reopening projects, video items with a selected audio stream attach HDR processing before their first displayed frame. On Windows, HDR processing can resume after a native P010 transfer into a buffer allocated at the extension’s request.
- On Windows, compiled HDR shaders are cached in `UserPlugins/Shaders`. Driver or graphics-adapter changes trigger cache preparation again.
- Fixed packet submission after `EAGAIN` in hardware HEVC decoding. Seeking can suppress output of earlier frames and skip eligible non-reference pictures. Rendering from the original keeps HDR processing, packet recovery, and seek optimizations active.

## v1.2.0 — 2026-11-22

### Русский

- Добавлен импорт WebVTT с сохранением тайминга, многострочного текста и имён говорящих.
- Добавлена поддержка UTF-16 LE/BE с BOM для всех форматов субтитров.
- При вставке многодорожечных MKV и MP4 можно выбрать аудиодорожку по языку, названию, кодеку, числу и порядку каналов. Видео и выбранная дорожка читаются из исходного контейнера. Вставка занимает один шаг Undo. Файлы с одной аудиодорожкой и форматы, для которых включён `ffmpeg_mta`, импортируются штатно.
- В контекстном меню айтема и верхнем меню **Item** сразу после **Take markers** добавлено подменю **Audio track**. Оно меняет звук активного тейка, сохраняя видео и границы секции. Undo и Redo переключают дорожку одним шагом.
- В обоих меню можно выбрать видео из выделения, если подходят несколько айтемов. Действие **Change Audio Track for selected video item** с тулбара открывает меню, а из **Action List** открывает окно выбора дорожки. При нескольких подходящих видео **Action List** сначала открывает окно выбора айтема.
- Для каждой дорожки сохраняется свой кэш пиков. REAPER показывает пики по мере построения и достраивает отсутствующие и неполные данные по текущим настройкам. Ручные команды построения пиков учитывают выбранные потоки и неактивные тейки.
- Из дорожек с высотными каналами читаются основные каналы и LFE. Например, 7.1.2 импортируется как 7.1, а 9.1.6 как 9.1. Пики строятся по сохранённым каналам выбранной дорожки. Команда **Explode multichannel audio** использует каналы выбранной дорожки.
- Исправлено воспроизведение начальных пауз выбранной дорожки в MKV и MP4, включая задержку декодера E-AC-3 в Matroska. Уточнена перемотка и исправлено чтение многодорожечных источников, открытых напрямую через ReaScript.

### English

- Added WebVTT import with cue timing, multiline text, and voice names.
- Added UTF-16 LE/BE support with a BOM for all subtitle formats.
- Native import shows language, title, codec, channel count, and channel order for multitrack MKV and MP4. Video and the selected audio track are read from the original container, with one undo step for insertion. Files with one audio stream and formats with `ffmpeg_mta` enabled use native import.
- **Audio track** appears directly after **Take markers** in the item context menu and the main **Item** menu. It switches the active take's audio while preserving video and section bounds. Undo and redo switch the stream in one step.
- Both menus let you choose a video when multiple selected items have switchable audio tracks. **Change Audio Track for selected video item** opens a popup from a toolbar and an audio track dialog from the **Action List**. With multiple eligible videos, the **Action List** first opens an item selection dialog.
- Each stream has its own peak cache. REAPER displays peaks as they are built and completes missing or incomplete data using the current settings. Manual peak commands use the selected streams and include inactive takes.
- Tracks with height channels retain their main channels and LFE. For example, 7.1.2 imports as 7.1 and 9.1.6 as 9.1. Peaks represent the retained channels of the selected track. **Explode multichannel audio** uses the selected stream's channels.
- Fixed playback of leading audio gaps in MKV and MP4, including E-AC-3 decoder delay in Matroska. Improved seek accuracy and reads from multitrack sources opened directly through ReaScript.

## v1.1.1 — 2026-11-21

### Русский

- Исправлена регрессия версии 1.1.0, из-за которой импорт субтитров создавал две записи в истории Undo. Теперь на импорт приходится одна запись, а Redo восстанавливает текст и тайминг даже без исходного файла.
- Предпросмотр субтитров не изменяет историю Undo. Правый трим временного айтема сохраняется.

### English

- Fixed a regression introduced in 1.1.0 that created two undo entries when importing subtitles. Importing now creates one entry, and redo restores the text and timing even if the original file is unavailable.
- Subtitle preview leaves undo history alone. Right-trimmed placeholders retain their bounds.

## v1.1.0 — 2026-11-19

### Русский

- Добавлена поддержка shared-библиотек FFmpeg 9.x в REAPER 7.80 и новее.
- Dolby, DTS и MLP импортируются как аудиоисточники. Проекты с источниками FRZZ_VIDEO открываются и при сохранении переходят на FRZZ_AUDIO.
- Субтитры с одинаковым началом и концом сохраняют нулевую длительность. SRT сохраняет абзацы и не принимает таймкод внутри реплики за начало нового айтема.
- ASS/SSA учитывает секцию [Events], пробелы вокруг таймкодов и оформление реплик при преобразовании в обычный текст.
- Исправлены чтение и перемотка TrueHD+AC3, включая сочетания с разными частотами и усечение открытого файла на macOS.
- Предпросмотр субтитров не меняет историю Undo. Временный айтем с правым тримом сохраняет свои границы.
- Исправлено открытие и сохранение путей с двойными кавычками на macOS.

### English

- Added support for shared versions of FFmpeg 9.x in REAPER 7.80 and later.
- Dolby, DTS and MLP import as audio sources. Projects with FRZZ_VIDEO sources load and save those sources as FRZZ_AUDIO.
- Zero-duration subtitle cues retain their timing. SRT preserves paragraphs and keeps timestamp examples inside the cue text.
- ASS/SSA scope dialogue to [Events], accept whitespace around timestamps, and turn subtitle formatting and drawing commands into plain notes.
- Fixed TrueHD+AC3 reading and seeking with mixed sample rates and when an open file is truncated on macOS.
- Subtitle preview leaves undo history alone. Right-trimmed placeholders retain their bounds.
- Fixed opening and saving paths containing double quotes on macOS.

## v1.0.0 — 2026-09-08

### Русский

#### Добавлено

- Первый публичный релиз.
- Нативные сборки для Windows x64 и macOS Universal 2.
- Индексирование `.thd+ac3` в памяти и потоковое чтение TrueHD без дискового кэша.
- Runtime-адаптеры для публичных ABI FFmpeg 5.x–8.x.
- Преобразование субтитров в обычные Text Item при импорте.

#### Проверено

- REAPER 7.79 с FFmpeg 5.1.2 и 8.1.2 на Windows.
- REAPER 7.79 с Homebrew FFmpeg 8.1.2 на macOS x86_64.
- Сборка Universal 2 из отдельно скомпилированных срезов x86_64 и arm64 в Xcode 26.2.
- Работа Universal 2 на реальном Apple Silicon.
- DTS, DTS-HD, MLP, E-AC-3 и чередующиеся TrueHD+AC3 тестовые файлы.

### English

#### Added

- Initial public release.
- Native Windows x64 and macOS Universal 2 builds.
- In-memory `.thd+ac3` indexing and TrueHD streaming without a disk cache.
- Runtime adapters for the public FFmpeg 5.x–8.x ABI families.
- Conversion of subtitles into regular Text Items during import.

#### Verified

- REAPER 7.79 with FFmpeg 5.1.2 and 8.1.2 on Windows.
- REAPER 7.79 with Homebrew FFmpeg 8.1.2 on macOS x86_64.
- Universal 2 packaging with separately compiled x86_64 and arm64 slices on Xcode 26.2.
- Universal 2 runtime confirmed on real Apple Silicon hardware.
- DTS, DTS-HD, MLP, E-AC-3 and interleaved TrueHD+AC3 test media.
