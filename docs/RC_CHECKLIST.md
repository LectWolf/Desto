# Release Candidate Checklist

The automated checks below have been run on Windows 11 x64 with the current Release build. Windows 10 icon fallback is covered by forced MDL2 selection and glyph-index tests; final visual confirmation still needs a real Windows 10 desktop.

| Area | Automated result | Manual result |
| --- | --- | --- |
| CMake configure and Release build | Pass (2026-08-19 current worktree) | N/A |
| Debug/Release CTest | Pass: 24/24 Release on 2026-08-23; targeted Debug regressions pass | N/A |
| Empty/Typical performance | Pass: Release Empty 38.6042 ms startup / 0.6388 ms visibility P95 / 2,162,688 bytes private; Typical 446.041 ms / 14.6507 ms / 5,435,392 bytes / 0% idle CPU | N/A |
| Stability soak and resource counts | Pass: 30 s / 1108 iterations; HANDLE 213 -> 213, GDI 13 -> 13, USER 15 -> 15 | N/A |
| Installer install/start/uninstall | Release manifest and installer package verified with `VerifyRelease.ps1 -ExpectedVersion 0.1.0` on 2026-08-23 | N/A |
| Same-version data preservation | Pass with unique sentinel | N/A |
| Downgrade rejection | Pass (`0.0.9` rejected over `0.1.0`) | N/A |
| Release manifest and SHA-256 tamper rejection | Pass | N/A |
| Card settings layout and custom menus | N/A | User screenshot needed |
| Cross-display drag, DPI resize and placement memory | N/A | User screenshot needed |
| Card-under-settings z-order and optional always-on-top | N/A | User screenshot needed |
| Taskbar double-click ordering and new-window restoration | N/A | User scenario needed |
| Tray light/dark native menu and hit targets | N/A | User screenshot needed |

## Manual scenarios

1. With two displays at different scale factors, drag a Card across the settings window and back. Capture one screenshot while it is over settings and one after placement.
2. Set a Card to fixed 5-column large-icon layout, then change density to small, list mode and back to grid. Confirm the content width and height stay aligned without overlap.
3. Open several maximized and normal windows in a known top-to-bottom order, double-click the taskbar, browse a taskbar preview, open a new window, and double-click again. Record the exact expected and observed order.
4. Switch Windows light/dark mode, open the native tray menu and settings menu, and capture both states.
