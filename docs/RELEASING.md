# Release Process

Desto releases are built on `windows-2022` from an exact Git commit. CI builds and tests both Debug and Release. Stable release tags use `v<major>.<minor>.<patch>`; development builds use a fourth build component (`v<major>.<minor>.<patch>.<build>`).

## Artifacts

`release/BuildRelease.ps1` produces the following. Official CI passes `-RequireClean`; local diagnostic builds retain `workingTreeDirty: true` in `BUILDINFO.json` instead of pretending to be reproducible releases.

- `Desto-<major>.<minor>.<patch>.<build>-win-x64-setup.exe`: per-user installer; the fourth component is automatically incremented for every local/CI package build;
- `release-manifest.json`: version, source commit, minimum Windows build, asset sizes and SHA-256 values;
- `SHA256SUMS.txt`: hashes for every published metadata and binary artifact;
- `BUILDINFO.json`: source commit, source time, tool version and dirty-tree state;
- `THIRD-PARTY-NOTICES.md`: runtime and build-time attribution inventory.

The release workflow creates a draft GitHub Release only after the Release test suite and `VerifyRelease.ps1` pass. A failed build therefore does not replace an installed application or publish a partial release. Inno Setup also retains its transactional rollback behavior during file replacement. Older installers are rejected before mutation by the installed-version check.

## External Requirements

The following are intentionally not guessed or embedded in the repository:

- a project license for the Desto host, selected by the owner before making the repository public;
- a Windows Authenticode code-signing certificate and its protected CI secret;
- final release notes and manual release-candidate approval;
- GPL-3.0-or-later is declared in the repository root `LICENSE`.

## Local Verification

```powershell
cmake -S . -B build -DDESTO_BUILD_PROTOTYPES=OFF -DDESTO_BUILD_BENCHMARKS=ON
cmake --build build --config Release --parallel 4 -- /nodeReuse:false
ctest --test-dir build -C Release --output-on-failure
./release/BuildRelease.ps1 -Version 0.1.0
./release/VerifyRelease.ps1 -ReleaseDirectory dist -ExpectedVersion 0.1.0
```

Do not publish an unsigned draft as a formal release. After signing is configured, rebuild so the signed executable and installer hashes are the ones recorded in the manifest and `SHA256SUMS.txt`.
