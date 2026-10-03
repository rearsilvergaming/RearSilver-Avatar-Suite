import { releaseUploadPage } from "./release-upload-page.js";

const PRODUCT = "rearsilver-avatar-suite";
const PLATFORM = "windows-x64";
const MAX_MANIFEST_BYTES = 128 * 1024;
const MAX_INSTALLER_BYTES = 5 * 1024 * 1024 * 1024;
const PART_SIZE = 10 * 1024 * 1024;

const channels = {
  "owner-build": {
    family: "owner",
    downloadTokenBinding: "OWNER_DOWNLOAD_TOKEN",
    filename: version =>
      `RearSilver-Avatar-Suite-Owner-${version}-Setup.exe`
  },
  "private-beta": {
    family: "beta",
    downloadTokenBinding: "PRIVATE_BETA_DOWNLOAD_TOKEN",
    filename: version =>
      `RearSilver-Avatar-Suite-Private-Beta-${version}-Setup.exe`
  }
};

const legacyReleases = {
  "owner-build:1.0.0-owner.3": {
    filename: "RearSilver-Avatar-Suite-Owner-1.0.0-owner.3-Setup.exe",
    key: "releases/owner-build/1.0.0-owner.3/windows-x64/RearSilver-Avatar-Suite-Owner-1.0.0-owner.3-Setup.exe"
  }
};

const ownerTestObjectKey = "tests/owner-build/download-route.txt";

const responseJson = (body, status = 200, cache = false) =>
  new Response(JSON.stringify(body), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": cache && status === 200
        ? "public, max-age=300"
        : "no-store",
      "x-content-type-options": "nosniff"
    }
  });

const adminJson = (body, status = 200) =>
  responseJson(body, status, false);

const unauthorised = () =>
  responseJson({ error: "Unauthorised" }, 401);

const plainObject = value =>
  value !== null &&
  typeof value === "object" &&
  !Array.isArray(value);

const validSha256 = value =>
  typeof value === "string" &&
  /^[0-9a-f]{64}$/.test(value);

const parseVersion = (value, channel) => {
  const policy = channels[channel];

  if (!policy || typeof value !== "string") {
    return null;
  }

  const match = value.match(
    /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)-(owner|beta)\.(0|[1-9]\d*)$/
  );

  if (!match || match[4] !== policy.family) {
    return null;
  }

  return {
    raw: value,
    major: BigInt(match[1]),
    minor: BigInt(match[2]),
    patch: BigInt(match[3]),
    prerelease: BigInt(match[5])
  };
};

const compareVersions = (left, right, channel) => {
  const a = parseVersion(left, channel);
  const b = parseVersion(right, channel);

  if (!a || !b) {
    return null;
  }

  for (const field of ["major", "minor", "patch", "prerelease"]) {
    if (a[field] < b[field]) return -1;
    if (a[field] > b[field]) return 1;
  }

  return 0;
};

const expectedFilename = (channel, version) => {
  const policy = channels[channel];
  return policy ? policy.filename(version) : "";
};

const validHttpsUrl = (value, allowEmpty = false) => {
  if (allowEmpty && value === "") {
    return true;
  }

  if (typeof value !== "string" || value.length > 2048) {
    return false;
  }

  try {
    return new URL(value).protocol === "https:";
  } catch {
    return false;
  }
};

const releaseManifestKey = (channel, version) =>
  `releases/${channel}/${version}/${PLATFORM}/manifest.json`;

const liveManifestKey = channel =>
  `channels/${channel}/${PLATFORM}/manifest.json`;

const installerKey = (channel, version, sha256, filename) =>
  `releases/${channel}/${version}/${PLATFORM}/${sha256}/${filename}`;

const legacyIdentity = (channel, version) =>
  legacyReleases[`${channel}:${version}`] || null;

const expectedMetadata = (channel, version, sha256, size) => ({
  product: PRODUCT,
  channel,
  version,
  sha256,
  expected_size: String(size)
});

const metadataMatches = (object, expected) => {
  if (!object) {
    return false;
  }

  const actual = object.customMetadata || {};

  return Object.entries(expected).every(
    ([key, value]) => actual[key] === value
  );
};

const isConditionalFailure = error => {
  const status = Number(error?.status || error?.statusCode || 0);
  const message = String(error?.message || "").toLowerCase();

  return status === 412 ||
    message.includes("precondition") ||
    message.includes("condition not met");
};

const uploadErrorResponse = error => {
  const message = String(error?.message || "").toLowerCase();

  if (
    error?.message === "request-too-large" ||
    error?.message === "invalid-json"
  ) {
    return adminJson({ error: "Invalid request body" }, 400);
  }

  if (
    message.includes("multipart") &&
    (
      message.includes("not found") ||
      message.includes("invalid") ||
      message.includes("complete") ||
      message.includes("abort")
    )
  ) {
    return adminJson(
      { error: "Upload session is no longer usable; begin the upload again" },
      409
    );
  }

  return adminJson(
    { error: "Release upload operation failed" },
    500
  );
};

const uploadCoordinates = (
  channel,
  version,
  filename,
  sha256,
  size
) => {
  if (
    !channels[channel] ||
    !parseVersion(version, channel) ||
    !validSha256(sha256) ||
    !Number.isSafeInteger(size) ||
    size < 1 ||
    size > MAX_INSTALLER_BYTES ||
    filename !== expectedFilename(channel, version) ||
    legacyIdentity(channel, version)
  ) {
    return null;
  }

  return {
    channel,
    version,
    filename,
    sha256,
    size,
    key: installerKey(channel, version, sha256, filename),
    metadata: expectedMetadata(channel, version, sha256, size)
  };
};

const coordinatesFromUrl = url =>
  uploadCoordinates(
    url.searchParams.get("channel"),
    url.searchParams.get("version"),
    url.searchParams.get("filename"),
    url.searchParams.get("sha256"),
    Number(url.searchParams.get("size"))
  );

const validUploadId = value =>
  typeof value === "string" &&
  /^[\x21-\x7e]{1,512}$/.test(value);

const parseJsonRequest = async request => {
  const length = Number(request.headers.get("content-length") || "0");

  if (length > 1024 * 1024) {
    throw new Error("request-too-large");
  }

  try {
    return await request.json();
  } catch {
    throw new Error("invalid-json");
  }
};

const hasUploadAccess = (request, env) => {
  const expected = env.RELEASE_UPLOAD_TOKEN;

  return Boolean(expected) &&
    (request.headers.get("authorization") || "") ===
      `Bearer ${expected}`;
};

const hasDownloadAccess = (request, env, channel) => {
  const binding = channels[channel]?.downloadTokenBinding;
  const expected = binding ? env[binding] : null;

  return Boolean(expected) &&
    (request.headers.get("authorization") || "") ===
      `Bearer ${expected}`;
};

const validReleaseNotes = notes =>
  Array.isArray(notes) &&
  notes.length <= 6 &&
  notes.every(
    note =>
      typeof note === "string" &&
      note.length > 0 &&
      note.length <= 180
  );

const expectedDownloadUrl = (
  requestOrigin,
  channel,
  version
) =>
  new URL(
    `/v1/download/${channel}/${version}/${PLATFORM}`,
    requestOrigin
  ).href;

const validateManifest = (
  manifest,
  expectedChannel,
  requestOrigin
) => {
  if (!plainObject(manifest) || !channels[expectedChannel]) {
    return null;
  }

  const version = parseVersion(
    manifest.version,
    expectedChannel
  );

  const minimum = parseVersion(
    manifest.minimum_supported_version,
    expectedChannel
  );

  if (
    manifest.schema !== 1 ||
    manifest.product !== PRODUCT ||
    manifest.channel !== expectedChannel ||
    manifest.platform !== PLATFORM ||
    manifest.minimum_updater_schema !== 1 ||
    !version ||
    !minimum ||
    compareVersions(
      manifest.minimum_supported_version,
      manifest.version,
      expectedChannel
    ) > 0 ||
    typeof manifest.mandatory !== "boolean" ||
    typeof manifest.published_at !== "string" ||
    !Number.isFinite(Date.parse(manifest.published_at)) ||
    !validReleaseNotes(manifest.release_notes) ||
    !validHttpsUrl(manifest.release_notes_url, true) ||
    !plainObject(manifest.installer)
  ) {
    return null;
  }

  const installer = manifest.installer;

  if (
    installer.filename !== expectedFilename(
      expectedChannel,
      manifest.version
    ) ||
    !Number.isSafeInteger(installer.size) ||
    installer.size < 1 ||
    !validSha256(installer.sha256) ||
    installer.download_request_url !== expectedDownloadUrl(
      requestOrigin,
      expectedChannel,
      manifest.version
    )
  ) {
    return null;
  }

  return manifest;
};

const immutableManifestValue = manifest => ({
  schema: manifest.schema,
  product: manifest.product,
  channel: manifest.channel,
  platform: manifest.platform,
  version: manifest.version,
  minimum_supported_version:
    manifest.minimum_supported_version,
  minimum_updater_schema:
    manifest.minimum_updater_schema,
  mandatory: manifest.mandatory,
  installer: {
    filename: manifest.installer.filename,
    size: manifest.installer.size,
    sha256: manifest.installer.sha256,
    download_request_url:
      manifest.installer.download_request_url
  },
  release_notes: manifest.release_notes,
  release_notes_url: manifest.release_notes_url
});

const immutableManifestsMatch = (left, right) =>
  JSON.stringify(immutableManifestValue(left)) ===
  JSON.stringify(immutableManifestValue(right));

const completeManifestsMatch = (left, right) =>
  immutableManifestsMatch(left, right) &&
  left.published_at === right.published_at;

const readManifestObject = async (
  env,
  key,
  expectedChannel,
  requestOrigin
) => {
  const object = await env.RELEASES.get(key);

  if (!object) {
    return null;
  }

  if (object.size > MAX_MANIFEST_BYTES) {
    throw new Error("manifest-too-large");
  }

  const raw = await object.text();
  let parsed;

  try {
    parsed = JSON.parse(raw);
  } catch {
    throw new Error("invalid-manifest-json");
  }

  const manifest = validateManifest(
    parsed,
    expectedChannel,
    requestOrigin
  );

  if (!manifest) {
    throw new Error("invalid-manifest");
  }

  return { object, manifest, raw };
};

const configuredManifest = async (
  env,
  channel,
  requestOrigin
) => {
  if (env.RELEASES) {
    const stored = await readManifestObject(
      env,
      liveManifestKey(channel),
      channel,
      requestOrigin
    );

    if (stored) {
      return stored.manifest;
    }
  }

  const binding =
    channel === "owner-build"
      ? "OWNER_BUILD_MANIFEST"
      : "PRIVATE_BETA_MANIFEST";

  const raw = env[binding];

  if (!raw) {
    return null;
  }

  let parsed;

  try {
    parsed = JSON.parse(raw);
  } catch {
    throw new Error("invalid-fallback-manifest");
  }

  const manifest = validateManifest(
    parsed,
    channel,
    requestOrigin
  );

  if (!manifest) {
    throw new Error("invalid-fallback-manifest");
  }

  return manifest;
};

const createManifest = (
  body,
  requestOrigin,
  publishedAt
) => ({
  schema: 1,
  product: PRODUCT,
  channel: body.channel,
  platform: PLATFORM,
  version: body.version,
  minimum_supported_version:
    body.minimum_supported_version,
  minimum_updater_schema: 1,
  mandatory: body.mandatory,
  published_at: publishedAt,
  installer: {
    filename: body.filename,
    size: body.size,
    sha256: body.sha256,
    download_request_url: expectedDownloadUrl(
      requestOrigin,
      body.channel,
      body.version
    )
  },
  release_notes: body.release_notes,
  release_notes_url: body.release_notes_url
});

const validatePublicationBody = (
  body,
  requestOrigin
) => {
  if (!plainObject(body)) {
    return null;
  }

  const coordinates = uploadCoordinates(
    body.channel,
    body.version,
    body.filename,
    body.sha256,
    body.size
  );

  if (
    !coordinates ||
    !parseVersion(
      body.minimum_supported_version,
      body.channel
    ) ||
    compareVersions(
      body.minimum_supported_version,
      body.version,
      body.channel
    ) > 0 ||
    !Number.isSafeInteger(body.size) ||
    body.size < 1 ||
    body.size > MAX_INSTALLER_BYTES ||
    typeof body.mandatory !== "boolean" ||
    !validReleaseNotes(body.release_notes) ||
    !validHttpsUrl(body.release_notes_url, true)
  ) {
    return null;
  }

  const candidate = createManifest(
    body,
    requestOrigin,
    new Date().toISOString()
  );

  if (
    !validateManifest(
      candidate,
      body.channel,
      requestOrigin
    )
  ) {
    return null;
  }

  return { coordinates, candidate };
};

const putImmutableReleaseManifest = async (
  env,
  coordinates,
  candidate,
  requestOrigin
) => {
  const key = releaseManifestKey(
    coordinates.channel,
    coordinates.version
  );

  const serialized = JSON.stringify(candidate);

  let created = null;

  try {
    created = await env.RELEASES.put(
      key,
      serialized,
      {
        httpMetadata: {
          contentType: "application/json; charset=utf-8"
        },
        onlyIf: new Headers({
          "If-None-Match": "*"
        })
      }
    );
  } catch (error) {
    if (!isConditionalFailure(error)) {
      throw error;
    }
  }

  if (created) {
    return { manifest: candidate, serialized };
  }

  const existing = await readManifestObject(
    env,
    key,
    coordinates.channel,
    requestOrigin
  );

  if (
    !existing ||
    !immutableManifestsMatch(
      existing.manifest,
      candidate
    )
  ) {
    throw new Error("release-conflict");
  }

  return {
    manifest: existing.manifest,
    serialized: existing.raw
  };
};

const advanceLiveManifest = async (
  env,
  release,
  requestOrigin
) => {
  const channel = release.manifest.channel;
  const key = liveManifestKey(channel);

  for (let attempt = 0; attempt < 3; ++attempt) {
    const current = await readManifestObject(
      env,
      key,
      channel,
      requestOrigin
    );

    if (current) {
      const comparison = compareVersions(
        current.manifest.version,
        release.manifest.version,
        channel
      );

      if (comparison > 0) {
        throw new Error("stale-release");
      }

      if (comparison === 0) {
        if (
          completeManifestsMatch(
            current.manifest,
            release.manifest
          )
        ) {
          return current.manifest;
        }

        throw new Error("release-conflict");
      }
    }

    const condition = current
      ? new Headers({
          "If-Match": current.object.httpEtag
        })
      : new Headers({
          "If-None-Match": "*"
        });

    let updated = null;

    try {
      updated = await env.RELEASES.put(
        key,
        release.serialized,
        {
          httpMetadata: {
            contentType:
              "application/json; charset=utf-8"
          },
          onlyIf: condition
        }
      );
    } catch (error) {
      if (!isConditionalFailure(error)) {
        throw error;
      }
    }

    if (updated) {
      return release.manifest;
    }
  }

  throw new Error("publication-race");
};

const handleReleaseUpload = async (
  request,
  env,
  url
) => {
  if (!hasUploadAccess(request, env)) {
    return adminJson(
      { error: "Unauthorised" },
      401
    );
  }

  if (!env.RELEASES) {
    return adminJson(
      { error: "Release storage is not configured" },
      503
    );
  }

  const action =
    url.searchParams.get("action") || "create";

  try {
    if (
      request.method === "POST" &&
      action === "create"
    ) {
      const body = await parseJsonRequest(request);
      const size = Number(body?.size);

      const coordinates = uploadCoordinates(
        body?.channel,
        body?.version,
        body?.filename,
        body?.sha256,
        size
      );

      if (
        !coordinates ||
        !Number.isSafeInteger(size) ||
        size < 1 ||
        size > MAX_INSTALLER_BYTES
      ) {
        return adminJson(
          { error: "Invalid release details" },
          400
        );
      }

      const published = await env.RELEASES.head(
        releaseManifestKey(
          coordinates.channel,
          coordinates.version
        )
      );

      if (published) {
        return adminJson(
          {
            error:
              "This version is already published; use a new version"
          },
          409
        );
      }

      const existing = await env.RELEASES.head(
        coordinates.key
      );

      if (existing) {
        if (
          existing.size !== size ||
          !metadataMatches(
            existing,
            coordinates.metadata
          )
        ) {
          return adminJson(
            {
              error:
                "That immutable release path contains a different installer"
            },
            409
          );
        }

        return adminJson({
          key: coordinates.key,
          alreadyExists: true,
          size: existing.size,
          sha256: coordinates.sha256,
          partSize: PART_SIZE
        });
      }

      const upload =
        await env.RELEASES.createMultipartUpload(
          coordinates.key,
          {
            httpMetadata: {
              contentType:
                "application/vnd.microsoft.portable-executable",
              contentDisposition:
                `attachment; filename="${coordinates.filename}"`
            },
            customMetadata: coordinates.metadata
          }
        );

      return adminJson({
        key: coordinates.key,
        uploadId: upload.uploadId,
        sha256: coordinates.sha256,
        partSize: PART_SIZE
      });
    }

    const coordinates = coordinatesFromUrl(url);
    const uploadId =
      url.searchParams.get("uploadId");

    if (
      !coordinates ||
      !validUploadId(uploadId)
    ) {
      return adminJson(
        { error: "Invalid upload details" },
        400
      );
    }

    const upload =
      env.RELEASES.resumeMultipartUpload(
        coordinates.key,
        uploadId
      );

    if (
      request.method === "PUT" &&
      action === "part"
    ) {
      const partNumber = Number(
        url.searchParams.get("partNumber")
      );

      const contentLength =
        request.headers.get("content-length");

      const length =
        contentLength === null
          ? null
          : Number(contentLength);

      if (
        !Number.isInteger(partNumber) ||
        partNumber < 1 ||
        partNumber > 10000 ||
        !request.body ||
        (
          length !== null &&
          (
            !Number.isFinite(length) ||
            length < 1 ||
            length > PART_SIZE
          )
        )
      ) {
        return adminJson(
          { error: "Invalid upload part" },
          400
        );
      }

      const part = await upload.uploadPart(
        partNumber,
        request.body
      );

      return adminJson({
        partNumber: part.partNumber,
        etag: part.etag
      });
    }

    if (
      request.method === "POST" &&
      action === "complete"
    ) {
      const body = await parseJsonRequest(request);
      const size = Number(body?.size);
      const parts = Array.isArray(body?.parts)
        ? [...body.parts]
        : [];

      if (
        body?.sha256 !== coordinates.sha256 ||
        !Number.isSafeInteger(size) ||
        size < 1 ||
        size !== coordinates.size ||
        parts.length < 1 ||
        parts.length > 10000
      ) {
        return adminJson(
          { error: "Invalid completion details" },
          400
        );
      }

      parts.sort(
        (left, right) =>
          left?.partNumber - right?.partNumber
      );

      const validParts = parts.every(
        (part, index) =>
          part &&
          part.partNumber === index + 1 &&
          typeof part.etag === "string" &&
          part.etag.length > 0 &&
          part.etag.length <= 256
      );

      if (!validParts) {
        return adminJson(
          { error: "Invalid completion details" },
          400
        );
      }

      await upload.complete(
        parts.map(({ partNumber, etag }) => ({
          partNumber,
          etag
        }))
      );

      const completed = await env.RELEASES.head(
        coordinates.key
      );

      if (
        !completed ||
        completed.size !== size ||
        !metadataMatches(
          completed,
          coordinates.metadata
        )
      ) {
        return adminJson(
          {
            error:
              "Uploaded installer identity verification failed"
          },
          500
        );
      }

      return adminJson({
        key: coordinates.key,
        size: completed.size,
        sha256: coordinates.sha256,
        etag: completed.httpEtag
      });
    }

    if (
      request.method === "DELETE" &&
      action === "abort"
    ) {
      await upload.abort();

      return adminJson({ aborted: true });
    }

    return adminJson(
      { error: "Method not allowed" },
      405
    );
  } catch (error) {
    return uploadErrorResponse(error);
  }
};

const handleReleasePublish = async (
  request,
  env,
  url
) => {
  if (!hasUploadAccess(request, env)) {
    return adminJson(
      { error: "Unauthorised" },
      401
    );
  }

  if (!env.RELEASES) {
    return adminJson(
      { error: "Release storage is not configured" },
      503
    );
  }

  if (request.method !== "POST") {
    return adminJson(
      { error: "Method not allowed" },
      405
    );
  }

  try {
    const body = await parseJsonRequest(request);

    if (legacyIdentity(body?.channel, body?.version)) {
      return adminJson(
        {
          error:
            "Legacy releases cannot be republished; use a new version"
        },
        409
      );
    }

    const validated = validatePublicationBody(
      body,
      url.origin
    );

    if (!validated) {
      return adminJson(
        { error: "Invalid manifest details" },
        400
      );
    }

    const { coordinates, candidate } =
      validated;

    const installer = await env.RELEASES.head(
      coordinates.key
    );

    if (
      !installer ||
      installer.size !== candidate.installer.size ||
      !metadataMatches(
        installer,
        coordinates.metadata
      )
    ) {
      return adminJson(
        {
          error:
            "The uploaded installer could not be verified"
        },
        409
      );
    }

    const release =
      await putImmutableReleaseManifest(
        env,
        coordinates,
        candidate,
        url.origin
      );

    const published =
      await advanceLiveManifest(
        env,
        release,
        url.origin
      );

    return adminJson({
      published: true,
      manifest: published
    });
  } catch (error) {
    if (
      error?.message === "release-conflict"
    ) {
      return adminJson(
        {
          error:
            "This version is already published with different immutable release details"
        },
        409
      );
    }

    if (error?.message === "stale-release") {
      return adminJson(
        {
          error:
            "A newer version is already live on this channel"
        },
        409
      );
    }

    if (
      error?.message === "publication-race"
    ) {
      return adminJson(
        {
          error:
            "Another publication changed this channel; review the live release and try again"
        },
        409
      );
    }

    return adminJson(
      { error: "Release publication failed" },
      500
    );
  }
};

const uploadPageResponse = () =>
  new Response(releaseUploadPage, {
    headers: {
      "content-type": "text/html; charset=utf-8",
      "cache-control": "no-store",
      "content-security-policy":
        "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; form-action 'none'; frame-ancestors 'none'; base-uri 'none'",
      "referrer-policy": "no-referrer",
      "x-content-type-options": "nosniff",
      "x-frame-options": "DENY"
    }
  });

const validStoredInstaller = (
  object,
  manifest,
  legacy
) => {
  if (
    !object ||
    object.size !== manifest.installer.size
  ) {
    return false;
  }

  if (legacy) {
    return true;
  }

  return metadataMatches(
    object,
    expectedMetadata(
      manifest.channel,
      manifest.version,
      manifest.installer.sha256,
      manifest.installer.size
    )
  );
};

const requestedRange = (request, size) => {
  const value = request.headers.get("range");

  if (!value) {
    return { ok: true, range: null };
  }

  const match = value.match(/^bytes=(\d*)-(\d*)$/i);

  if (!match || (match[1] === "" && match[2] === "")) {
    return { ok: false, range: null };
  }

  if (match[1] === "") {
    const suffix = Number(match[2]);

    if (!Number.isSafeInteger(suffix) || suffix < 1) {
      return { ok: false, range: null };
    }

    const length = Math.min(suffix, size);
    return {
      ok: true,
      range: { offset: size - length, length }
    };
  }

  const start = Number(match[1]);
  const requestedEnd = match[2] === ""
    ? size - 1
    : Number(match[2]);

  if (
    !Number.isSafeInteger(start) ||
    !Number.isSafeInteger(requestedEnd) ||
    start < 0 ||
    start >= size ||
    requestedEnd < start
  ) {
    return { ok: false, range: null };
  }

  const end = Math.min(requestedEnd, size - 1);
  return {
    ok: true,
    range: { offset: start, length: end - start + 1 }
  };
};

const rangeNotSatisfiable = size =>
  new Response(null, {
    status: 416,
    headers: {
      "content-range": `bytes */${size}`,
      "accept-ranges": "bytes",
      "cache-control": "private, no-store",
      "x-content-type-options": "nosniff"
    }
  });

const serveR2Object = (
  object,
  filename,
  headOnly = false,
  range = null,
  totalSize = object?.size || 0
) => {
  if (!object) {
    return responseJson(
      { error: "Download not found" },
      404
    );
  }

  const headers = new Headers();
  object.writeHttpMetadata(headers);

  headers.set(
    "content-type",
    headers.get("content-type") ||
      "application/octet-stream"
  );

  const length = range ? range.length : totalSize;

  headers.set(
    "content-length",
    String(length)
  );

  headers.set(
    "content-disposition",
    `attachment; filename="${filename}"`
  );

  headers.set(
    "cache-control",
    "private, no-store"
  );

  headers.set("etag", object.httpEtag);
  headers.set("accept-ranges", "bytes");
  headers.set(
    "x-content-type-options",
    "nosniff"
  );

  let status = 200;

  if (range) {
    status = 206;

    headers.set(
      "content-range",
      `bytes ${range.offset}-${range.offset + range.length - 1}/${totalSize}`
    );
  }

  return new Response(
    headOnly ? null : object.body,
    { status, headers }
  );
};

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const pathname = url.pathname;

    if (
      pathname === "/admin/releases/upload"
    ) {
      if (request.method !== "GET") {
        return adminJson(
          { error: "Method not allowed" },
          405
        );
      }

      return uploadPageResponse();
    }

    if (
      pathname ===
      "/v1/admin/releases/upload"
    ) {
      return handleReleaseUpload(
        request,
        env,
        url
      );
    }

    if (
      pathname ===
      "/v1/admin/releases/publish"
    ) {
      return handleReleasePublish(
        request,
        env,
        url
      );
    }

    if (
      request.method !== "GET" &&
      request.method !== "HEAD"
    ) {
      return responseJson(
        { error: "Method not allowed" },
        405
      );
    }

    if (
      pathname ===
      "/v1/download/owner-build/test"
    ) {
      if (
        !hasDownloadAccess(
          request,
          env,
          "owner-build"
        )
      ) {
        return unauthorised();
      }

      if (!env.RELEASES) {
        return responseJson(
          {
            error:
              "Release storage is not configured"
          },
          503
        );
      }

      try {
        const stored = await env.RELEASES.head(
          ownerTestObjectKey
        );

        if (!stored) {
          return responseJson(
            { error: "Download not found" },
            404
          );
        }

        const parsedRange = request.method === "GET"
          ? requestedRange(request, stored.size)
          : { ok: true, range: null };

        if (!parsedRange.ok) {
          return rangeNotSatisfiable(stored.size);
        }

        const object = request.method === "HEAD"
          ? stored
          : await env.RELEASES.get(
              ownerTestObjectKey,
              parsedRange.range
                ? { range: parsedRange.range }
                : undefined
            );

        return serveR2Object(
          object,
          "RearSilver-Avatar-Update-Service-Test.txt",
          request.method === "HEAD",
          parsedRange.range,
          stored.size
        );
      } catch {
        return responseJson(
          { error: "Release download failed" },
          500
        );
      }
    }

    const download = pathname.match(
      /^\/v1\/download\/(owner-build|private-beta)\/([^/]+)\/windows-x64\/?$/
    );

    if (download) {
      const channel = download[1];
      const version = download[2];

      if (
        !hasDownloadAccess(
          request,
          env,
          channel
        )
      ) {
        return unauthorised();
      }

      if (!env.RELEASES) {
        return responseJson(
          {
            error:
              "Release storage is not configured"
          },
          503
        );
      }

      try {
        const manifest = await configuredManifest(
          env,
          channel,
          url.origin
        );

        if (
          !manifest ||
          !parseVersion(version, channel) ||
          version !== manifest.version
        ) {
          return responseJson(
            { error: "Download not found" },
            404
          );
        }

        const legacy = legacyIdentity(
          channel,
          version
        );

        const key = legacy
          ? legacy.key
          : installerKey(
              channel,
              version,
              manifest.installer.sha256,
              manifest.installer.filename
            );

        const stored = await env.RELEASES.head(key);

        if (
          !validStoredInstaller(
            stored,
            manifest,
            Boolean(legacy)
          )
        ) {
          return responseJson(
            { error: "Download not found" },
            404
          );
        }

        const parsedRange = request.method === "GET"
          ? requestedRange(request, stored.size)
          : { ok: true, range: null };

        if (!parsedRange.ok) {
          return rangeNotSatisfiable(stored.size);
        }

        const object = request.method === "HEAD"
          ? stored
          : await env.RELEASES.get(
              key,
              parsedRange.range
                ? { range: parsedRange.range }
                : undefined
            );

        if (
          !validStoredInstaller(
            object,
            manifest,
            Boolean(legacy)
          )
        ) {
          return responseJson(
            { error: "Download not found" },
            404
          );
        }

        return serveR2Object(
          object,
          manifest.installer.filename,
          request.method === "HEAD",
          parsedRange.range,
          stored.size
        );
      } catch {
        return responseJson(
          { error: "Release download failed" },
          500
        );
      }
    }

    const update = pathname.match(
      /^\/v1\/updates\/(owner-build|private-beta)\/windows-x64\/?$/
    );

    if (!update) {
      return responseJson(
        { error: "Not found" },
        404
      );
    }

    const channel = update[1];

    try {
      const manifest = await configuredManifest(
        env,
        channel,
        url.origin
      );

      if (!manifest) {
        return responseJson(
          {
            error:
              "This update channel is not configured"
          },
          503
        );
      }

      if (request.method === "HEAD") {
        return new Response(null, {
          status: 200,
          headers: {
            "content-type":
              "application/json; charset=utf-8",
            "cache-control":
              "public, max-age=300",
            "x-content-type-options":
              "nosniff"
          }
        });
      }

      return responseJson(
        manifest,
        200,
        true
      );
    } catch {
      return responseJson(
        {
          error:
            "Configured manifest is invalid"
        },
        500
      );
    }
  }
};
