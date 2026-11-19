# Changelog / Журнал изменений

[Русский](#русский) · [English](#english)

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
