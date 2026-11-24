# Changelog

## [0.4.4] - 2026-11-24

### Fixed

- Correctly mark every occupied date for events that use `DURATION`, including multi-day and week-long events, midnight boundaries, and leap days.
- Calculate UTC event durations before converting to local time, and consistently normalize leap seconds.
- Parse event properties with quoted parameters correctly, including titles with an `ALTREP` URL.
- Reject malformed events and incomplete or failed CalDAV resource responses so an unsuccessful refresh cannot replace saved events with an incomplete agenda.
- Show refresh failures alongside saved events and known-empty dates, making stale cached results visible while retaining offline access.
- Reclaim unaccepted attachment handles, handle delayed callbacks safely, and allow the calendar integration to reconnect after detachment.
- Install a new versioned flyout DLL when upgrading from 0.4.3, while preserving the saved connection and event cache and deferring removal of locked old modules.

### Validation

- Added regression coverage for event parsing, cache publication limits, overlap and truncation, concurrent attachment ownership, and locked-DLL upgrades.
- Passed x64 builds, parser/cache and installer tests, HTTP integration, and registry install/uninstall checks.
- Passed native XAML lifecycle tests covering selection, event markers, concurrent publication, mutex contention, cleanup, and repeated installation in a disposable test host.
- Cross-built the ARM64 installer and native payloads. Live ARM64 validation remains pending.

### Upgrade notes

Run the installer for your PC's architecture and choose **Keep settings**. Your saved calendar connection and event cache carry over. If an older module remains locked by Windows, it can be removed after signing out and back in.

The installers remain unsigned. SHA-256 checksums are available with the release downloads. The integration still depends on Windows' internal Notification Center layout; future Windows updates may require compatibility changes.

## [0.4.3] - 2026-11-17

- Display CalDAV event dots and event titles in the native Windows 11 taskbar calendar.
- Open cached dates immediately and retain access to saved events offline.
- Provide x64 and native ARM64 installers, with saved settings and event-cache migration from Win11CalendarFix.

[0.4.4]: https://github.com/Eng-ReQTeCH/Wincal/compare/v0.4.3...v0.4.4
[0.4.3]: https://github.com/Eng-ReQTeCH/Wincal/releases/tag/v0.4.3
