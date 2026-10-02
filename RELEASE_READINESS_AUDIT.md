# RearSilver Avatar Suite release-readiness audit

Audit date: 1 October 2026

This audit separates checks completed in the development environment from tests that require a clean or materially different Windows system. A successful local build proves the packaged payload is internally complete; it does not prove every Windows, GPU, audio-device, OBS, security-policy, or upgrade combination.

## Release profiles

| Profile | Version | Expiry | Intended use |
| --- | --- | --- | --- |
| Owner Build | `1.0.0-owner.2` | None | Internal owner testing |
| Private Beta | `1.0.0-beta.1` | 31 October 2026 | Time-limited external testing |

Both profiles display their channel and version in the Settings navigation footer. Private Beta also displays its expiry state. After expiry, avatar output and mutating automation commands are disabled while Settings, Feedback & Diagnostics, Updates, and Help remain available so a tester can recover settings and create a report.

## Runtime dependency and payload audit

Avatar Suite is a native 64-bit Windows application. It does not use Qt or CEF.

| Component | How it is supplied | Installer handling |
| --- | --- | --- |
| Application executable | Built by CMake/MSVC | Installed with version/channel metadata |
| Microsoft Visual C++ x64 Runtime | Official Microsoft redistributable | Bundled, signature-checked at packaging time, installed only when required |
| Microsoft Edge WebView2 Runtime | Official Microsoft evergreen standalone installer | Bundled, signature-checked at packaging time, installed only when absent |
| `WebView2Loader.dll` | Application payload | Installed beside the executable |
| Spout2 | Statically linked library | No separate application DLL required; OBS still needs its own Spout2 capture plugin when that capture method is used |
| Settings HTML, font, branding, defaults, rail icons | Application payload | Explicitly enumerated and validated during packaging |
| Built-in layer PNGs | Application payload | Copied recursively and checked against the source asset count |

The release executable dependency scan found only Windows system libraries, the WebView2 loader, and the Microsoft Visual C++ runtime. No development checkout, Qt installation, CEF installation, SDK, CMake, compiler, or PowerShell environment is needed by an installed user.

## Defects corrected during this audit

- The old staging process copied only top-level files and omitted `built-in-layers`. Packaging now includes and validates the directory.
- The uninstaller now removes the nested built-in layer payload while preserving `%LOCALAPPDATA%\RearSilver Avatar`, including user settings and logs.
- Install and uninstall now detect a running Avatar Suite window and ask the user to close it before files are replaced or removed.
- The installer rejects unsupported non-64-bit and pre-Windows 10 systems with a clear message.
- Upgrades replace only explicitly owned Avatar Suite files and directories; a user-selected installation root is never recursively deleted.
- Visual C++ and WebView2 prerequisite progress is reported clearly, and WebView2 is verified again after its installer returns success.
- The installer uses Avatar-specific header/welcome artwork and presents Avatar Suite's own software licence.
- Missing settings HTML and WebView2 startup failures now produce actionable repair/reinstall messages instead of a blank Settings window.
- Logs now live in `%LOCALAPPDATA%\RearSilver Avatar\Logs` instead of the shared temporary directory and remain available to Feedback & Diagnostics.
- Owner and Private Beta builds now carry distinct executable metadata, UI identity, artifact directories, installer names, and expiry behavior.
- The installer payload is explicit. Unexpected or missing runtime files fail the build instead of silently producing an incomplete installer.

## Locally verified

- Debug compilation after the audit changes.
- Settings-page JavaScript syntax.
- Whitespace/error scan with `git diff --check`.
- Separate Owner and Private Beta CMake configurations.
- Owner and Private Beta now use separate Visual Studio `RelWithDebInfo` CMake configure/build presets, matching Stream Suite's preset-driven profile and artifact layout while staging Avatar's standalone payload under each versioned `app` directory.
- Earlier Owner and Private Beta installers completed successfully but were superseded by the final responsive-branding and preset-structure cleanup.
- A separate already-expired Private Beta configuration compiles successfully, confirming the expiry enforcement path is buildable; runtime expiry behavior remains on the external test list below.
- The final staged application payload is expected to contain 34 runtime files, including the compact Settings badge and all 17 built-in layer PNGs. Packaging rejects a missing or unexpected file.
- Embedded executable metadata identifies `Owner Build / 1.0.0-owner.2` and `Private Beta / 1.0.0-beta.1` independently.
- Microsoft prerequisite files have valid Microsoft Authenticode signatures.
- The release executable dependency set contains no Qt or CEF dependency.
- User data is outside the installation directory and is intentionally retained by uninstall.
- Owner upgrade smoke testing preserved the existing settings and completed Guided Setup flag, created both Start Menu shortcuts, and removed the Program Files installation directory cleanly on uninstall.
- WebSocket protocol version 1 runtime testing confirmed capability negotiation, request-ID correlation, dynamic catalogue discovery, catalogue revisions, `catalog.changed` broadcasts, and stable machine-readable error codes.
- SAMMI extension 0.3.2 remained connected and automatically refreshed its friendly preset, layer, group, and effect catalogue after an Avatar Suite catalogue change without manual intervention.

## Required external validation before public release

These checks cannot be honestly confirmed from the development PC alone:

- Install on clean, fully updated Windows 10 x64 and Windows 11 x64 virtual machines.
- Install with WebView2 absent and confirm the bundled runtime installs; repeat with a current WebView2 already present.
- Install with the Visual C++ runtime absent and confirm the bundled runtime installs; repeat with a current runtime already present.
- Launch as a standard non-administrator after an administrator performs the machine-wide install.
- Confirm first-run Guided Setup, restart/resume behavior, and `Run guided setup again` at compact and full window sizes.
- Upgrade Owner-to-Owner and Private-Beta-to-newer-Beta while retaining presets, images, layers, effects, and general settings.
- Attempt upgrade and uninstall while Avatar Suite is running and confirm the close/retry flow.
- Uninstall and confirm application files and shortcuts are removed while `%LOCALAPPDATA%\RearSilver Avatar` remains.
- Test installation and user paths containing spaces and non-ASCII characters.
- Test Game Capture, Window Capture, and Spout2 capture in supported OBS versions. Spout2 requires the separate OBS Spout2 plugin.
- Test integrated and discrete GPUs, mixed-GPU laptop configurations, DPI scaling, and multiple monitors.
- Test no microphone, disabled microphone, changed default device, USB disconnect/reconnect, and multiple input devices.
- Test WebSocket port `17891` already occupied, local firewall/security software, SAMMI reconnect, and more than one local automation client.
- Advance a disposable test system beyond 31 October 2026 and confirm Private Beta output and mutation commands stop while diagnostics and settings remain accessible.
- Digitally sign the application and installer for distribution, or explicitly accept Windows SmartScreen reputation warnings during the private test. The current build process verifies Microsoft's bundled installers but does not apply a RearSilver code-signing certificate.
- When the updater is implemented, add and test a silent handoff that preserves the installed instance and does not depend on interactive close/retry dialogs.

## Release-candidate gate

The earlier Owner installer passed the install, settings-preservation, Start Menu shortcut, diagnostics, and clean-uninstall smoke tests. Its recorded size and hash no longer identify the final candidate because the compact Settings badge was subsequently added to the packaged payload.

Record and smoke-test the final Owner installer before building the Private Beta. The Private Beta must then be rebuilt, inspected, and tested before it is sent to Chrizzz. Generated installers remain unsigned until a RearSilver code-signing certificate is added, so private testers may receive a Windows SmartScreen warning.
