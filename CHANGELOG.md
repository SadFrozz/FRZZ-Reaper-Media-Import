# Changelog / Журнал изменений

[Русский](#русский) · [English](#english)

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
