import { releaseUploadPage } from "./release-upload-page.js";

const json = (body, status = 200) => new Response(JSON.stringify(body), {
  status,
  headers: {
    "content-type": "application/json; charset=utf-8",
    "cache-control": status === 200 ? "public, max-age=300" : "no-store",
    "x-content-type-options": "nosniff"
  }
});

const channels = {
  "owner-build": {
    manifestBinding: "OWNER_BUILD_MANIFEST",
    downloadTokenBinding: "OWNER_DOWNLOAD_TOKEN",
    installerFilename: /^RearSilver-Avatar-Suite-Owner-[0-9A-Za-z.-]+-Setup\.exe$/
  },
  "private-beta": {
    manifestBinding: "PRIVATE_BETA_MANIFEST",
    downloadTokenBinding: "PRIVATE_BETA_DOWNLOAD_TOKEN",
    installerFilename: /^RearSilver-Avatar-Suite-Private-Beta-[0-9A-Za-z.-]+-Setup\.exe$/
  }
};

const ownerTestObjectKey = "tests/owner-build/download-route.txt";

const unauthorized = () => json({ error: "Unauthorised" }, 401);

const adminJson = (body, status = 200) => new Response(JSON.stringify(body), {
  status,
  headers: {
    "content-type": "application/json; charset=utf-8",
    "cache-control": "no-store",
    "x-content-type-options": "nosniff"
  }
});

const hasUploadAccess = (request, env) => {
  const expected = env.RELEASE_UPLOAD_TOKEN;
  if (!expected) return false;
  return (request.headers.get("authorization") || "") === `Bearer ${expected}`;
};

const hasDownloadAccess = (request, env, channel) => {
  const binding = channels[channel]?.downloadTokenBinding;
  const expected = binding ? env[binding] : null;
  if (!expected) return false;
  const authorization = request.headers.get("authorization") || "";
  return authorization === `Bearer ${expected}`;
};

const liveManifestKey = channel => `channels/${channel}/windows-x64/manifest.json`;

const configuredManifest = async (env, channel) => {
  const binding = channels[channel]?.manifestBinding;
  let raw = null;
  if (env.RELEASES) {
    const object = await env.RELEASES.get(liveManifestKey(channel));
    if (object) raw = await object.text();
  }
  if (!raw) raw = binding ? env[binding] : null;
  if (!raw) return null;
  try {
    const manifest = JSON.parse(raw);
    if (manifest?.schema !== 1 || manifest?.product !== "rearsilver-avatar-suite" ||
        manifest?.channel !== channel || manifest?.platform !== "windows-x64") return null;
    return manifest;
  } catch {
    return null;
  }
};

const safeReleasePart = value => typeof value === "string" && /^[0-9A-Za-z][0-9A-Za-z.-]{0,95}$/.test(value);
const safeFilename = (value, channel) => typeof value === "string" &&
  channels[channel]?.installerFilename.test(value) && !value.includes("..");

const expectedInstallerFilename = (channel, version) => channel === "owner-build"
  ? `RearSilver-Avatar-Suite-Owner-${version}-Setup.exe`
  : `RearSilver-Avatar-Suite-Private-Beta-${version}-Setup.exe`;

const uploadCoordinates = (channel, version, filename) => {
  if (!channels[channel] || !safeReleasePart(version) || !safeFilename(filename, channel) ||
      filename !== expectedInstallerFilename(channel, version)) return null;
  return {
    channel,
    version,
    filename,
    key: `releases/${channel}/${version}/windows-x64/${filename}`
  };
};

const requestUploadCoordinates = url => uploadCoordinates(
  url.searchParams.get("channel"),
  url.searchParams.get("version"),
  url.searchParams.get("filename")
);

const validUploadId = value => typeof value === "string" && /^[\x21-\x7E]{1,512}$/.test(value);

const parseJson = async request => {
  const length = Number(request.headers.get("content-length") || "0");
  if (length > 1024 * 1024) throw new Error("request-too-large");
  return request.json();
};

const validHttpsUrl = value => {
  if (value === "") return true;
  if (typeof value !== "string" || value.length > 2048) return false;
  try { return new URL(value).protocol === "https:"; } catch { return false; }
};

const handleReleasePublish = async (request, env) => {
  if (!hasUploadAccess(request, env)) return adminJson({ error: "Unauthorised" }, 401);
  if (!env.RELEASES) return adminJson({ error: "Release storage is not configured" }, 503);
  if (request.method !== "POST") return adminJson({ error: "Method not allowed" }, 405);

  try {
    const body = await parseJson(request);
    const channel = body?.channel;
    const version = body?.version;
    const filename = body?.filename;
    const size = Number(body?.size);
    const sha256 = typeof body?.sha256 === "string" ? body.sha256.toLowerCase() : "";
    const minimumSupportedVersion = body?.minimum_supported_version;
    const releaseNotesUrl = body?.release_notes_url;
    const releaseNotes = Array.isArray(body?.release_notes) ? body.release_notes : [];
    const coordinates = uploadCoordinates(channel, version, filename);
    const validNotes = releaseNotes.length <= 6 && releaseNotes.every(note =>
      typeof note === "string" && note.length > 0 && note.length <= 180);
    if (!coordinates || !Number.isSafeInteger(size) || size < 1 ||
        !/^[0-9a-f]{64}$/.test(sha256) || !safeReleasePart(minimumSupportedVersion) ||
        !validHttpsUrl(releaseNotesUrl) || !validNotes || typeof body?.mandatory !== "boolean") {
      return adminJson({ error: "Invalid manifest details" }, 400);
    }

    const installer = await env.RELEASES.head(coordinates.key);
    if (!installer || installer.size !== size) {
      return adminJson({ error: "The uploaded installer could not be verified" }, 409);
    }

    const manifest = {
      schema: 1,
      product: "rearsilver-avatar-suite",
      channel,
      platform: "windows-x64",
      version,
      minimum_supported_version: minimumSupportedVersion,
      minimum_updater_schema: 1,
      mandatory: body.mandatory,
      published_at: new Date().toISOString(),
      installer: {
        filename,
        size,
        sha256,
        download_request_url: new URL(`/v1/download/${channel}/${version}/windows-x64`, request.url).href
      },
      release_notes: releaseNotes,
      release_notes_url: releaseNotesUrl
    };
    const serialized = JSON.stringify(manifest);
    const metadata = { httpMetadata: { contentType: "application/json; charset=utf-8" } };
    await env.RELEASES.put(`releases/${channel}/${version}/windows-x64/manifest.json`, serialized, metadata);
    await env.RELEASES.put(liveManifestKey(channel), serialized, metadata);
    return adminJson({ published: true, manifest });
  } catch {
    return adminJson({ error: "Release publication failed" }, 500);
  }
};

const handleReleaseUpload = async (request, env, url) => {
  if (!hasUploadAccess(request, env)) return adminJson({ error: "Unauthorised" }, 401);
  if (!env.RELEASES) return adminJson({ error: "Release storage is not configured" }, 503);

  const action = url.searchParams.get("action") || "create";
  try {
    if (request.method === "POST" && action === "create") {
      const body = await parseJson(request);
      const coordinates = uploadCoordinates(body?.channel, body?.version, body?.filename);
      const size = Number(body?.size);
      if (!coordinates || !Number.isSafeInteger(size) || size < 1 || size > 5 * 1024 * 1024 * 1024) {
        return adminJson({ error: "Invalid release details" }, 400);
      }
      const existing = await env.RELEASES.head(coordinates.key);
      if (existing) {
        if (existing.size !== size) return adminJson({ error: "That release path already contains a different installer" }, 409);
        return adminJson({ key: coordinates.key, alreadyExists: true, size: existing.size, partSize: 10 * 1024 * 1024 });
      }
      const upload = await env.RELEASES.createMultipartUpload(coordinates.key, {
        httpMetadata: {
          contentType: "application/vnd.microsoft.portable-executable",
          contentDisposition: `attachment; filename="${coordinates.filename}"`
        }
      });
      return adminJson({ key: coordinates.key, uploadId: upload.uploadId, partSize: 10 * 1024 * 1024 });
    }

    const coordinates = requestUploadCoordinates(url);
    const uploadId = url.searchParams.get("uploadId");
    if (!coordinates || !validUploadId(uploadId)) return adminJson({ error: "Invalid upload details" }, 400);
    const upload = env.RELEASES.resumeMultipartUpload(coordinates.key, uploadId);

    if (request.method === "PUT" && action === "part") {
      const partNumber = Number(url.searchParams.get("partNumber"));
      const contentLength = request.headers.get("content-length");
      const length = contentLength === null ? null : Number(contentLength);
      if (!Number.isInteger(partNumber) || partNumber < 1 || partNumber > 10000 ||
          !request.body || (length !== null && (!Number.isFinite(length) || length < 1 || length > 10 * 1024 * 1024))) {
        return adminJson({ error: "Invalid upload part" }, 400);
      }
      const part = await upload.uploadPart(partNumber, request.body);
      return adminJson({ partNumber: part.partNumber, etag: part.etag });
    }

    if (request.method === "POST" && action === "complete") {
      const body = await parseJson(request);
      const size = Number(body?.size);
      const parts = Array.isArray(body?.parts) ? [...body.parts] : [];
      if (!Number.isSafeInteger(size) || size < 1 || parts.length < 1 || parts.length > 10000) {
        return adminJson({ error: "Invalid completion details" }, 400);
      }
      parts.sort((a, b) => a?.partNumber - b?.partNumber);
      const validParts = parts.every((part, index) => part && part.partNumber === index + 1 &&
        typeof part.etag === "string" && part.etag.length > 0 && part.etag.length <= 256);
      if (!validParts) return adminJson({ error: "Invalid completion details" }, 400);
      const object = await upload.complete(parts.map(({ partNumber, etag }) => ({ partNumber, etag })));
      if (object.size !== size) return adminJson({ error: "Uploaded object size verification failed" }, 500);
      return adminJson({ key: coordinates.key, size: object.size, etag: object.httpEtag });
    }

    if (request.method === "DELETE" && action === "abort") {
      await upload.abort();
      return adminJson({ aborted: true });
    }

    return adminJson({ error: "Method not allowed" }, 405);
  } catch {
    return adminJson({ error: "Release upload operation failed" }, 500);
  }
};

const uploadPageResponse = () => new Response(releaseUploadPage, {
  headers: {
    "content-type": "text/html; charset=utf-8",
    "cache-control": "no-store",
    "content-security-policy": "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; form-action 'none'; frame-ancestors 'none'; base-uri 'none'",
    "referrer-policy": "no-referrer",
    "x-content-type-options": "nosniff",
    "x-frame-options": "DENY"
  }
});

const serveR2Object = async (object, filename, headOnly = false) => {
  if (!object) return json({ error: "Download not found" }, 404);
  const headers = new Headers();
  object.writeHttpMetadata(headers);
  headers.set("content-type", headers.get("content-type") || "application/octet-stream");
  const range = object.range;
  const length = range && "length" in range ? range.length : object.size;
  headers.set("content-length", String(length));
  headers.set("content-disposition", `attachment; filename="${filename}"`);
  headers.set("cache-control", "private, no-store");
  headers.set("etag", object.httpEtag);
  headers.set("accept-ranges", "bytes");
  headers.set("x-content-type-options", "nosniff");
  let status = 200;
  if (range && "offset" in range && "length" in range) {
    status = 206;
    headers.set("content-range", `bytes ${range.offset}-${range.offset + range.length - 1}/${object.size}`);
  }
  return new Response(headOnly ? null : object.body, { status, headers });
};

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const pathname = url.pathname;

    if (pathname === "/admin/releases/upload") {
      if (request.method !== "GET") return adminJson({ error: "Method not allowed" }, 405);
      return uploadPageResponse();
    }

    if (pathname === "/v1/admin/releases/upload") return handleReleaseUpload(request, env, url);

    if (pathname === "/v1/admin/releases/publish") return handleReleasePublish(request, env);

    if (request.method !== "GET" && request.method !== "HEAD") return json({ error: "Method not allowed" }, 405);

    if (pathname === "/v1/download/owner-build/test") {
      if (!hasDownloadAccess(request, env, "owner-build")) return unauthorized();
      if (!env.RELEASES) return json({ error: "Release storage is not configured" }, 503);
      const object = request.method === "HEAD" ? await env.RELEASES.head(ownerTestObjectKey) :
        await env.RELEASES.get(ownerTestObjectKey, { range: request.headers });
      return serveR2Object(object, "RearSilver-Avatar-Update-Service-Test.txt", request.method === "HEAD");
    }

    const download = pathname.match(/^\/v1\/download\/(owner-build|private-beta)\/([^/]+)\/windows-x64\/?$/);
    if (download) {
      const channel = download[1];
      const version = download[2];
      if (!hasDownloadAccess(request, env, channel)) return unauthorized();
      if (!env.RELEASES) return json({ error: "Release storage is not configured" }, 503);
      const manifest = await configuredManifest(env, channel);
      const installer = manifest?.installer;
      if (!manifest || !safeReleasePart(version) || version !== manifest.version ||
          !safeFilename(installer?.filename, channel)) return json({ error: "Download not found" }, 404);
      const key = `releases/${channel}/${version}/windows-x64/${installer.filename}`;
      const object = request.method === "HEAD" ? await env.RELEASES.head(key) :
        await env.RELEASES.get(key, { range: request.headers });
      return serveR2Object(object, installer.filename, request.method === "HEAD");
    }

    const update = pathname.match(/^\/v1\/updates\/(owner-build|private-beta)\/windows-x64\/?$/);
    if (!update) return json({ error: "Not found" }, 404);
    const channel = update[1];
    try {
      const manifest = await configuredManifest(env, channel);
      if (!manifest) return json({ error: "This update channel is not configured" }, 503);
      return json(manifest);
    } catch {
      return json({ error: "Configured manifest is invalid" }, 500);
    }
  }
};
