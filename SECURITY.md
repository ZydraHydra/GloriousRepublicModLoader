# Security Policy

## Reporting a Vulnerability

If you discover a security issue in this repository (for example, unsafe `.ini`/`.json` parsing, HTTP API exposure in the WebView2 launcher, memory safety issues in runtime hooks, or GitHub Actions / SteamCMD workflow secret exposure), please **do not** open a public issue.

Instead, report it privately via GitHub's **Security** tab (**Report a vulnerability** / Private Vulnerability Reporting), or contact the repository maintainer directly.

## Scope Notes

- `grml_runtime.dll` is a native x64 DLL injected into `SOVIET64.exe` at runtime, and `GloriousRepublicModLoader.exe` hosts a local WebView2 UI backed by an in-process localhost HTTP API. Only download prebuilt binaries from the official [GitHub Releases](https://github.com/ZydraHydra/GloriousRepublicModLoader/releases) or the official Steam Workshop item.
- Never share `.env.local`, SteamCMD `config.vdf` files, `%LOCALAPPDATA%\GRML\grml_session.json`, or Steam credentials in issues, logs, or pull requests.
