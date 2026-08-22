# Desto 0.2.2

- Fixes stable-channel update checks against the published GitHub Release API.
- Sends the required GitHub API headers and validates HTTP status codes.
- Selects the installer from Release assets instead of downloading metadata JSON.
- Keeps development-channel checks and mirror fallback behavior.
