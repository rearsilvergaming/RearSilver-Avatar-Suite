# RearSilver Avatar Suite update service

This Worker serves isolated Owner Build and Private Beta update channels. Each channel has its own manifest, download credential, filename rules and private R2 release path.

The Worker has a private R2 binding named `RELEASES` connected to the `rearsilver-avatar-releases` bucket. Public bucket access must remain disabled.

The desktop clients request:

```text
GET /v1/updates/owner-build/windows-x64
GET /v1/updates/private-beta/windows-x64
```

The required manifest variables are:

```text
OWNER_BUILD_MANIFEST
PRIVATE_BETA_MANIFEST
```

The manifest product must be `rearsilver-avatar-suite`, its `channel` must match the requested URL, and its `platform` must be `windows-x64`. A mismatched manifest is rejected.

`owner-manifest.example.json` is an inert starting point. For local Worker testing, copy its JSON onto one line as the value of `OWNER_BUILD_MANIFEST` in `.dev.vars`.

The Private Beta manifest uses the same schema with `channel` set to `private-beta`, a Private Beta installer filename and a Private Beta download URL.

Manifests may include up to six concise notes in an optional `release_notes` string array. Avatar Suite shows this list in Updates before download. Keep `release_notes_url` as the HTTPS link to the complete Avatar Suite release history once the website exists.

Only Owner Build and Private Beta channels are configured. Deployment remains a separate, deliberate operation.

## Phase 2 Owner download-route test

Upload a small plain-text object to the private R2 bucket using this exact object key:

```text
tests/owner-build/download-route.txt
```

The download credentials are separate Worker secrets:

```text
OWNER_DOWNLOAD_TOKEN
PRIVATE_BETA_DOWNLOAD_TOKEN
```

The existing Owner fixed-object test route is:

```text
GET /v1/download/owner-build/test
Authorization: Bearer <OWNER_DOWNLOAD_TOKEN>
```

The route returns only that fixed test object. It does not accept an object key from the request and cannot expose another R2 object. Missing or incorrect credentials return HTTP 401. A missing test object returns HTTP 404.

The release routes are:

```text
GET /v1/download/owner-build/<manifest-version>/windows-x64
GET /v1/download/private-beta/<manifest-version>/windows-x64
```

Each request must use its channel's Bearer token. An Owner token does not authorise a Private Beta download, and a Private Beta token does not authorise an Owner download.

The requested version must exactly match that channel's current manifest. The Worker derives the immutable R2 key from the validated manifest and accepts only that channel's installer filename format:

```text
releases/owner-build/<version>/windows-x64/<manifest installer filename>
releases/private-beta/<version>/windows-x64/<manifest installer filename>
```

The caller cannot supply a bucket key or filename. A channel cannot resolve an object from the other channel. Old manifests stop authorising new downloads as soon as that channel's manifest is advanced, while R2 remains private.
