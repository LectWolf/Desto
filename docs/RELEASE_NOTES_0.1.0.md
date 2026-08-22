# Desto 0.1.0 Release Candidate

## Included

- Native Win32 desktop host with per-display Card placements, stable display identity and DPI-aware projection.
- Application, Mapping and Todo Card foundations with incremental content updates, custom ordering and grid/list presentation.
- Native tray menu, startup integration, desktop/taskbar triggers, experimental window restoration and Card z-order rules.
- Versioned JSON configuration with atomic writes, backups, migration and storage-root migration.
- Inno Setup 6 per-user x64 installer with upgrade preservation, downgrade protection, uninstall data choice and startup cleanup.
- GitHub Actions Debug/Release CI, draft release workflow, SHA-256 manifest, build provenance and third-party notices.
- Windows 10 1809+ icon compatibility through automatic fallback from `Segoe Fluent Icons` to the system `Segoe MDL2 Assets` font.
- Internal build identity uses `0.1.0.<build>`; package builds increment the fourth component and display the same value in the executable and About page.

## Verification

- Debug/Release: current worktree regression suite is 24/24 on Release; targeted Debug regressions pass.
- Empty gate: startup `39.9765 ms`, visibility P95 `0.0967 ms`, private bytes `2,281,472`, seven threads, idle CPU `0%`.
- Typical gate: two displays, four Cards, 100 items; startup `151.918 ms`, visibility P95 `12.7129 ms`, private bytes `5,414,912`, seven threads, idle CPU `0%`.
- Stability soak: 30 seconds, 1081 iterations, handles `213 -> 212`, GDI `13 -> 13`, USER `15 -> 15`, private bytes `3.22 -> 3.12 MiB`.
- Installer smoke must be rerun after removal of the former extension payloads.

## Known Limitations Before Formal Release

- The host project license has not been selected. The release workflow intentionally refuses public release until a root `LICENSE` is committed.
- The installer and executable are unsigned in this local build. A code-signing certificate is required to avoid SmartScreen warnings.
- Window restoration, taskbar ordering, cross-display drag/resize and visual layer behavior still require manual testing on the user's actual monitor/DPI/window setup.
- The 0.1.0 host does not silently self-update. The release manifest and verified installer are the update contract; any future in-app updater must download, verify and launch the installer transactionally.
