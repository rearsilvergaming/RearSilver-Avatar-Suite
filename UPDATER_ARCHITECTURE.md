# Avatar Suite updater contracts

## Release baseline

Owner `1.0.0-owner.4` is the currently published detection-only build. Private Beta `1.0.0-beta.1` was also published with detection only; the repository's Beta `.2` profile is preparation for the first complete Private Beta updater release and is not evidence that `.2` has been published. The first complete updater baselines are Owner `.5` and Private Beta `.2`. A staging `.6` manifest used to validate an installed Owner `.5` must therefore declare `.5` as its minimum supported version.

## Credentials

Private Beta receives `RS_UPDATE_DOWNLOAD_TOKEN` only at configure time. CMake rejects an update-enabled Private Beta configuration when it is absent. The value must be supplied outside Git and must never be placed in `CMakePresets.json`, logs, diagnostics, manifests, command lines, or the helper arguments. This token is embedded in the resulting client binary, so it is an access-control credential for a limited beta group rather than a secret that can be kept from a determined recipient.

Owner builds obtain their credential at runtime, first from `REARSILVER_AVATAR_OWNER_UPDATE_TOKEN`, then from the Windows generic credential `RearSilverAvatarSuite/OwnerUpdateToken`. The application copies a token only for the download and clears buffers it owns with `SecureZeroMemory` as a best-effort reduction of memory lifetime. Windows, the C++ runtime, and compiler-generated copies cannot be guaranteed to be cleared. The application creates a replacement environment block for the updater helper that explicitly removes `REARSILVER_AVATAR_OWNER_UPDATE_TOKEN`; the token is never passed to that helper.

Rotating the shared Private Beta token immediately prevents already-installed beta builds containing the previous token from downloading later releases. Normal rotation therefore requires a grace period in which the service accepts both old and new tokens: publish a build containing the new token while both are valid, wait for the agreed migration window, then revoke the old token. An emergency revocation intentionally breaks in-app download for affected builds and requires testers to install a replacement manually. A leaked token must be treated as access to beta installers and rotated under that emergency policy.

## User-visible behaviour

Automatic checks fail quietly and write only a diagnostic log entry. A manual check reports its failure on the open Updates page. One non-activating notification is shown per available version during a process run; the Updates navigation badge remains visible, including as a dot in compact navigation. Patch notes appear only when a newer version is available.

Downloads use a SHA-keyed `.partial` file and matching resume metadata, check available disk space, require a valid `206 Content-Range` for resumed transfers, use `If-Range` when an ETag exists, and delete untrusted partial data after range, size, or hash failures. Cancellation also removes the partial download.

## Install handoff

Avatar Suite preserves the existing draft or saves the active preset according to the user's choice. It copies the helper to a uniquely named external runner, starts it with `CREATE_BREAKAWAY_FROM_JOB` and an environment without the Owner token, and waits for an acknowledgement before closing. The helper opens and waits for the exact Avatar PID, receives the expected installer size and SHA-256, and verifies both immediately before elevation. It has no in-process or path fallback.

After Avatar closes, the helper owns visible errors, logging, elevation, installer waiting, and cleanup. It relaunches only the executable under the registered installation location and only after the registered `DisplayVersion` exactly matches the target. A fresh installation retains the existing guided-setup flow. A successful upgrade records its previous and completed versions; on first launch, Avatar offers a separate one-time guided review without clearing saved presets or settings.

## Production gate

Publisher guidance, Worker deployment, production version changes, installers, and manifests remain blocked until a disposable VM or snapshot proves an installed Owner `.5` can discover, authenticate, resume, verify, install staging `.6` (whose minimum is `.5`), relaunch from registered identity, offer the post-upgrade review, and report itself up to date.
