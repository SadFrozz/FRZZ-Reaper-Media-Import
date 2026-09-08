# Changelog

All notable changes are documented here.

## [1.0.0] - 2026-09-08

### Added

- Initial public release.
- Native Windows x64 and macOS Universal 2 builds.
- In-memory `.thd+ac3` indexing and TrueHD streaming without a disk cache.
- Runtime adapters for the public FFmpeg 5.x through 8.x ABI families.
- Native ASS, SSA and SRT import with a single REAPER Undo operation.

### Verified

- REAPER 7.79 with FFmpeg 5.1.2 and 8.1.2 on Windows.
- REAPER 7.79 with Homebrew FFmpeg 8.1.2 on macOS x86_64.
- Universal 2 packaging with separately compiled x86_64 and arm64 slices on Xcode 26.2.
- Universal 2 runtime confirmed on real Apple Silicon hardware.
- DTS, DTS-HD, MLP, E-AC-3 and interleaved TrueHD+AC-3 test media.
