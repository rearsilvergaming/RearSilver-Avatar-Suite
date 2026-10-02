# RearSilver Avatar Suite WebSocket protocol

Protocol version: 1  
Status: integration draft

This protocol lets local automation applications discover and control RearSilver Avatar Suite. It is platform-neutral and is the same interface used by the SAMMI integration and the Streamer.bot C# fallback generated in Avatar Suite Settings.

## Connection

- URL: `ws://127.0.0.1:17891/`
- Transport: unencrypted WebSocket on the local computer only
- Messages: UTF-8 JSON text frames
- Maximum message payload: 65,535 bytes
- Fragmented messages and binary messages are not supported
- More than one local client may connect at once

Avatar Suite may start before or after an integration. Clients should reconnect with a short backoff and perform the handshake and catalogue request after every connection.

Browser-origin connections are accepted only from `localhost`, `127.0.0.1`, the Avatar Suite WebView origin, or an absent/null origin. Native WebSocket clients normally send no Origin header.

## Message model

Client requests contain an `action`. An optional string `requestId` is echoed in the corresponding response and should be used when a client may have more than one request in flight.

```json
{
  "action": "server.status",
  "requestId": "status-1"
}
```

```json
{
  "ok": true,
  "action": "server.status",
  "version": 1,
  "protocolVersion": 1,
  "requestId": "status-1"
}
```

Messages containing an `event` are unsolicited server notifications. They are not responses and do not contain a `requestId`. Clients must distinguish `event` messages from `action` responses.

## Handshake

Send `client.hello` after connecting:

```json
{
  "action": "client.hello",
  "requestId": "hello-1"
}
```

The response identifies Avatar Suite, the protocol version, and available capabilities:

```json
{
  "ok": true,
  "action": "client.hello",
  "server": "RearSilver Avatar Suite",
  "protocolVersion": 1,
  "capabilities": [
    "catalog.get",
    "catalog.changed",
    "preset.activate",
    "sequence.run"
  ],
  "requestId": "hello-1"
}
```

An integration should reject unsupported protocol versions clearly rather than guessing at message fields.

## Status

`server.status` is a lightweight availability check:

```json
{"action":"server.status","requestId":"status-1"}
```

## Dynamic catalogue

Send `catalog.get` after the handshake and whenever the catalogue may be stale:

```json
{"action":"catalog.get","requestId":"catalog-1"}
```

The response contains all friendly names and stable IDs needed to populate integration controls:

```json
{
  "ok": true,
  "action": "catalog.get",
  "protocolVersion": 1,
  "catalogRevision": 7,
  "catalog": {
    "activePreset": {"id":"preset-id","name":"My avatar"},
    "presets": [
      {"id":"preset-id","name":"My avatar","active":true}
    ],
    "groups": [
      {"id":"group-id","name":"Outfit","visible":true}
    ],
    "layers": [
      {"id":"layer-id","name":"Hat","groupId":"group-id","visible":true}
    ],
    "effects": [
      {
        "id":"effect-id",
        "name":"Sway",
        "type":"sway",
        "ownerType":"layer",
        "ownerId":"layer-id",
        "ownerName":"Hat",
        "enabled":true
      }
    ],
    "savedActions": []
  },
  "requestId": "catalog-1"
}
```

`presets` contains every saved preset. The remaining collections describe the active preset. IDs are automation identifiers and remain stable when the user renames an item. Integrations should display names but store and send IDs.

`ownerType` is `primary`, `layer`, or `group`. The primary avatar uses the owner ID `primary`. `savedActions` is reserved for a future saved-action editor and is currently empty.

### Catalogue changes

When a user changes selectable catalogue content through Avatar Suite Settings, connected clients receive:

```json
{
  "event": "catalog.changed",
  "protocolVersion": 1,
  "catalogRevision": 8,
  "reason": "layer.changed"
}
```

The notification deliberately contains no catalogue payload. Clients should debounce repeated notifications if necessary, then call `catalog.get`. A client that reconnects must always request a fresh catalogue rather than relying on its previous revision.

Current reasons include `preset.changed`, `preset.reverted`, `layer.added`, `layer.changed`, `group.added`, `group.changed`, `template.added`, and `effects.changed`. Clients must treat the reason as informational and refresh for any `catalog.changed` event.

After a successful automation command that activates a preset, the calling client should request the catalogue again after receiving the command response. This preserves strict request-before-response ordering for simple clients.

## Activate a preset

`preset.activate` applies the selected preset's last saved snapshot. It never applies an unfinished Settings draft.

```json
{
  "action": "preset.activate",
  "presetId": "preset-id",
  "requestId": "activate-1"
}
```

```json
{
  "ok": true,
  "action": "preset.activate",
  "requestId": "activate-1"
}
```

Request a fresh catalogue after success because layers, groups, and effects are scoped to the newly active preset.

## Run an ordered sequence

`sequence.run` applies between 1 and 32 steps in array order:

```json
{
  "action": "sequence.run",
  "requestId": "sequence-1",
  "steps": [
    {"action":"preset.activate","presetId":"preset-id"},
    {"action":"group.visibility","targetId":"group-id","enabled":true},
    {"action":"layer.visibility","targetId":"layer-id","enabled":false},
    {"action":"effect.enabled","targetId":"layer-id","effectId":"effect-id","enabled":true}
  ]
}
```

Supported step actions:

| Step action | Required fields | Purpose |
| --- | --- | --- |
| `preset.activate` | `presetId` | Activate a saved preset snapshot |
| `group.visibility` | `targetId`, `enabled` | Show or hide a group at runtime |
| `layer.visibility` | `targetId`, `enabled` | Show or hide a layer at runtime |
| `effect.enabled` | `targetId`, `effectId`, `enabled` | Enable or disable a configured effect at runtime |

For a primary-avatar effect, use `targetId: "primary"` and the effect ID returned by the catalogue. Runtime visibility and effect overrides do not edit the saved preset.

A successful response reports the applied step count:

```json
{
  "ok": true,
  "action": "sequence.run",
  "steps": 4,
  "requestId": "sequence-1"
}
```

Sequences are ordered but are not currently transactional. A `preset.activate` step is applied immediately; if a later step is invalid, the preset activation is not rolled back. Integrations should build steps from the current catalogue and avoid stale IDs.

## Errors

Errors currently use this shape:

```json
{
  "ok": false,
  "code": "TARGET_NOT_FOUND",
  "error": "Preset ID was not found",
  "requestId": "activate-1"
}
```

Error text is intended for people and may be refined. Integrations should use `ok` to determine success and `code` for machine behavior.

| Code | Meaning |
| --- | --- |
| `BUILD_EXPIRED` | A time-limited build no longer accepts mutating commands |
| `COMMAND_FAILED` | A valid command could not be completed |
| `INVALID_ARGUMENT` | A required value is missing or has the wrong type |
| `INVALID_STEPS` | A sequence has no steps or exceeds the 32-step limit |
| `SERVER_SHUTTING_DOWN` | Avatar Suite is closing |
| `SERVER_UNAVAILABLE` | The command handler is unavailable |
| `TARGET_NOT_FOUND` | A stable ID no longer identifies an available item |
| `UNKNOWN_ACTION` | The top-level action is not supported |
| `UNSUPPORTED_ACTION` | A sequence step action is not supported |

Mutating commands are rejected after a time-limited Private Beta expires. Handshake, status, and catalogue discovery remain available so an integration can diagnose the installed build.

## Integration UI guidance

A native automation integration should:

1. Connect and send `client.hello`.
2. Send `catalog.get` and populate friendly sub-action controls.
3. Store stable IDs behind the displayed names.
4. Refresh after `catalog.changed`, reconnection, or successful preset activation.
5. Revalidate a selected ID at execution time and report when its item was removed.
6. Keep WebSocket, JSON, and internal identifiers hidden from ordinary users.

Typical sub-actions are Activate preset, Set layer visibility, Set group visibility, Set configured effect state, and Run ordered sequence. Existing platform triggers such as redeems, follows, subscriptions, raids, chat commands, timers, and manual actions decide when those sub-actions run.

The current integration goal is command execution with automatically maintained controls. Avatar Suite state changes are not currently proposed as user-facing triggers in automation platforms.
