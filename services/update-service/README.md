# RearSilver Avatar Suite update service

This Worker serves isolated Owner Build and Private Beta update channels. Each channel has its own manifest, download credential, strict version family, filename rule and private R2 release path.

The production configuration binds the private R2 bucket `rearsilver-avatar-releases` as `RELEASES`. Public bucket access must remain disabled. Deployment is a separate, deliberate operation; changing repository files does not deploy the Worker.

## Channels and manifests

Desktop clients request:

```text
GET /v1/updates/owner-build/windows-x64
GET /v1/updates/private-beta/windows-x64
```

Versions must use the channel's exact numeric format:

```text
<major>.<minor>.<patch>-owner.<number>
<major>.<minor>.<patch>-beta.<number>
```

Numeric components do not allow leading zeroes. Owner versions are accepted only on `owner-build`; Beta versions are accepted only on `private-beta`. The minimum supported version must use the same channel family and cannot be newer than the release version.

The live manifests are stored at:

```text
channels/owner-build/windows-x64/manifest.json
channels/private-beta/windows-x64/manifest.json
```

`OWNER_BUILD_MANIFEST` and `PRIVATE_BETA_MANIFEST` remain optional fallback bindings for local or recovery use when the corresponding live R2 manifest does not exist. Stored and fallback manifests receive the same product, channel, platform, version, timestamp, installer, notes and URL validation.

`owner-manifest.example.json` is an inert local-testing example. It is not a live release.

## Immutable release storage

New installers are addressed by their lowercase SHA-256 identity:

```text
releases/owner-build/<version>/windows-x64/<sha256>/<Owner installer filename>
releases/private-beta/<version>/windows-x64/<sha256>/<Private Beta installer filename>
```

The expected filenames are derived exactly from channel and version:

```text
RearSilver-Avatar-Suite-Owner-<version>-Setup.exe
RearSilver-Avatar-Suite-Private-Beta-<version>-Setup.exe
```

Every new installer object must have the expected byte size and these R2 custom metadata values:

```text
product=rearsilver-avatar-suite
channel=<owner-build or private-beta>
version=<release version>
sha256=<lowercase 64-character SHA-256>
expected_size=<installer size in bytes>
```

Upload creation, multipart part requests, completion, publication and download all use the same channel, version, filename, SHA-256 and size identity. An existing object is reusable only when its path, size and metadata all match. Matching size alone is insufficient.

Each release also receives an immutable versioned manifest:

```text
releases/<channel>/<version>/windows-x64/manifest.json
```

The Worker creates this object conditionally with `If-None-Match: *`. An existing manifest is accepted only when its immutable release details match; changing the installer, minimum version, requirement, notes or release-notes URL requires a new version.

The live channel manifest advances conditionally using the current R2 ETag. The Worker rejects publication behind the live version, conflicting details for the same version and unresolved concurrent publication changes. This prevents an older or competing request from silently replacing the live release.

Owner `1.0.0-owner.3` is the explicit legacy exception. Its previously uploaded installer remains at:

```text
releases/owner-build/1.0.0-owner.3/windows-x64/RearSilver-Avatar-Suite-Owner-1.0.0-owner.3-Setup.exe
```

The Worker can serve that known object without the newer metadata, but it cannot be uploaded or republished through the hardened release path.

## Credentials

The service uses three independent Worker secrets:

```text
RELEASE_UPLOAD_TOKEN
OWNER_DOWNLOAD_TOKEN
PRIVATE_BETA_DOWNLOAD_TOKEN
```

`RELEASE_UPLOAD_TOKEN` authorises the administrative multipart-upload and manifest-publication endpoints. It does not authorise installer downloads.

`OWNER_DOWNLOAD_TOKEN` authorises only Owner download routes. `PRIVATE_BETA_DOWNLOAD_TOKEN` authorises only Private Beta download routes. Neither channel token authorises the other channel, and neither authorises upload or publication. Secret values must not be stored in Git, manifests, object metadata or documentation.

The administrative page is:

```text
GET /admin/releases/upload
```

Uploading an installer does not make it live. Publication is a separate authenticated action after manifest review.

## Download and byte-range behavior

Release downloads use:

```text
GET /v1/download/owner-build/<live-version>/windows-x64
GET /v1/download/private-beta/<live-version>/windows-x64
```

The requested version must exactly match the channel's validated live manifest. The Worker derives the R2 key and filename; callers cannot provide an arbitrary bucket key.

Before serving a new-format installer, the Worker verifies its byte size and required custom metadata against the manifest. It repeats that validation on the retrieved R2 object.

The Worker supports one validated HTTP byte range in these forms:

```text
Range: bytes=<start>-<end>
Range: bytes=<start>-
Range: bytes=-<suffix-length>
```

Malformed, empty, reversed or out-of-bounds ranges return HTTP `416` with `Content-Range: bytes */<total-size>`. Valid partial responses return HTTP `206` with exact `Content-Range`, `Content-Length`, `ETag` and `Accept-Ranges: bytes` headers. Full responses return HTTP `200`.

The fixed Owner route used for isolated download testing is:

```text
GET /v1/download/owner-build/test
Authorization: Bearer <OWNER_DOWNLOAD_TOKEN>
```

It can access only `tests/owner-build/download-route.txt`. Missing or incorrect credentials return HTTP `401`; a missing object returns HTTP `404`.
