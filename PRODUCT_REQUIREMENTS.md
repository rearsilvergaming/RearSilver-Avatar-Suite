# RearSilver Avatar Suite reconstruction requirements

This document is the authoritative product and reconstruction specification. The first reconstructed renderer is intentionally limited to the responsive overlay, image loading, basic motion, background modes, and OBS compatibility described below, but validating that renderer does not waive the stable-output requirement.

## Stable OBS output is the product goal

- The OBS-facing composition has a stable configured width and height independent of the RearSilver Avatar Suite application window.
- A user may freely position, scale, crop, align, and optionally lock the RearSilver Avatar Suite source in OBS. RearSilver Avatar Suite must continue supplying the same intrinsic canvas underneath that OBS-owned transform.
- Resizing, maximising, restoring, snapping, minimising, or repositioning the application window must not change the OBS source's native dimensions, effective scene scale, position, crop, alignment, or bounding geometry.
- Locking a source in OBS does not protect its scene geometry when the source's native dimensions change. Therefore a dynamically client-sized source does not satisfy this requirement merely because the OBS item is locked.
- A maximised application client area is not necessarily the monitor's full resolution. Window chrome and the taskbar can produce dimensions such as 1920×1009 on a 1920×1080 display; this must not become the stream source resolution.
- The application window remains a freely resizable local preview and control surface. Its dimensions must not be authoritative for the stream composition.
- Veadotube's window-following source behaviour is a compatibility reference only and is not RearSilver Avatar Suite's target user experience.
- Direct Game Capture of a client-sized application swap chain remains a worst-case fallback only. Adopting it as product behaviour requires an explicit product decision; it must not silently replace the stable-output requirement.

## Application and rendering architecture

- Use one resizable, user-facing top-level window for the application preview and controls.
- Game Capture uses one fixed-size D3D11 composition swap chain created with `CreateSwapChainForComposition`. The current validated baseline is 1920×1080; treat this as the configured output resolution rather than a permanent restriction on future output settings.
- Attach that swap chain through one DirectComposition target and visual to the one top-level application window. DirectComposition applies only the local aspect-fit scale and centring transform; it must not alter the fixed OBS-facing render canvas.
- A newly created OBS Game Capture source has been verified to start with 1920×1080 intrinsic dimensions. An existing OBS source retains its user-defined scene transform when the application window changes size or state.
- Preserve the working renderer and responsive UI on the fixed composition canvas.
- Preserve the avatar's intended proportions and keep the composition centred without stretching, distortion, or unintended cropping.
- The avatar and background form the base composition layer.
- The custom, branded interface forms a rendered D3D overlay above the base composition. It does not reserve layout space.
- Opening or closing any menu, settings page, wizard step, dropdown, or other overlay must not push, shrink, rescale, crop, or reposition the avatar composition.
- Do not replace the rendered interface with generic native Win32-looking controls merely to obtain automatic coordinate handling.
- Do not reintroduce the rejected fixed capture child, GDI preview, child capture HWND, or second user-facing output window. Any new OBS output transport or GPU-sharing mechanism requires a contained design and validation step before production integration.

Avatar and UI layout must remain independent in the implementation. Both are evaluated in configured output-canvas coordinates, while DirectComposition separately maps that canvas into the local client area:

```text
avatarTransform = calculateAvatarTransform(outputWidth, outputHeight, logicalCanvas)
uiLayout = calculateUiLayout(outputWidth, outputHeight, outputScale, overlayState)
localPreviewTransform = calculateAspectFit(clientWidth, clientHeight, outputWidth, outputHeight)
```

Overlay state, panel dimensions, and other interface geometry must never be inputs to the avatar transform. In particular, do not calculate the avatar transform from the client area minus interface dimensions.

- The Avatar page provides a persistent whole-avatar scale from 25% to 250%, defaulting to 100%. It scales primary, reaction, and blink states uniformly before effects without changing the fixed output canvas or OBS source geometry.
- Show the exact avatar-scale percentage alongside descriptive clickable markers: Pocket-sized, Compact, Just right, Screen hog, and Absolute unit. Future layers inherit this avatar-root scale so their alignment remains intact.
- The Avatar page provides a preset-scoped horizontal mirror control for users whose artwork faces the wrong direction. It mirrors the complete assembled avatar around the shared avatar root, including primary, reaction, blink, layer placement, pivots, and effects.

## Authoritative UI direction

- RearSilver Avatar Suite is a standalone companion product in the RearSilver Stream Suite family. Use the same core navigation and control language so users encounter familiar tabs, panels, buttons, typography, spacing, states, terminology, and cyan-accented dark visual treatment across both products.
- The Stream Suite settings pages and guided setup are the direct visual and behavioural references. Avatar-specific previews, meters, thumbnails, and icons may provide product identity without creating a separate interface language.
- The floating collapsible sidebar and the temporary large D3D settings panel have been retired and removed. The renderer now contains only a small vertical quick-navigation rail; heavyweight settings belong to the owned WebView2 interface.
- The normal focused renderer view uses a small vertical quick-navigation icon rail inside the fixed D3D composition. It may briefly appear in Game Capture while the user interacts directly with the renderer.
- The quick-navigation rail hides whenever the main renderer window is not the active interaction surface, including while the owned Settings window has focus. This returns Game Capture to the clean avatar/background composition during configuration.
- Full Settings, Tools, and Guided Setup are not rendered into the fixed OBS canvas. They open in the same owned native window containing WebView2. The interface uses the Stream Suite Control Hub pattern: compact product branding at the top of a persistent left sidebar, grouped vertical navigation, a contextual page heading, short explanatory copy, and grouped cards or control sections in the remaining content area.
- The sidebar has separate Settings and Tools groups. Settings contains Presets, Avatar, Layers, Microphone, Blink, Effects, Backgrounds, Output, and General. Tools contains Feedback & Diagnostics, Updates, and Help. On narrow windows the sidebar collapses to recognisable icons with tooltips and never to unexplained abbreviations or a completely hidden state.
- The Avatar preview remains available beside configuration pages where it provides useful immediate feedback. Tools pages use the full content width so diagnostics, updater state, and help content are not compressed by an unrelated preview.
- The cog quick-rail button opens the last visited Settings page and must never strand the user on a Tools destination. The Tools quick-rail button opens Feedback & Diagnostics. Both routes reuse the same owned WebView2 window and navigation state.
- The owned Settings window is application UI rather than an OBS output surface. It must not modify, resize, suspend, re-parent, or otherwise disturb the validated DirectComposition renderer, its animation, its fixed canvas, or the user's OBS transform.
- The Settings window supports ordinary Windows maximise and restore behaviour and remembers a maximised state when hidden and reopened. Responsive content must remain inside its assigned grid column; long filenames and button groups must never extend underneath the preview card.
- Native-to-HTML communication uses WebView2 messages following the Stream Suite hosting pattern. Avatar may reuse Stream Suite styling and assets where appropriate while retaining its own product identity and content.
- First run uses a guided setup in the owned WebView2 interface based directly on the Stream Suite pattern: visible progress, a page title and explanation, grouped settings, automatic progress saving, and Back, Skip for now, and Continue actions.
- Guided setup and normal Settings reuse the same controls, layout components, validation, and stored settings. Do not implement separate copies of the same configuration workflow.
- The initial candidate setup areas are Output, Avatar Images, Microphone, Voice Detection, Blink and Motion, and Review and Finish. Their exact names, grouping, order, and page count remain provisional until real control density and workflow testing justify the final structure.
- The quick-navigation rail is overlay content and must never alter the avatar transform or configured OBS canvas. The owned Settings window and its WebView content remain outside that composition entirely.
- Closing Settings returns focus to the Avatar renderer without changing the avatar, animation phase, output canvas, background, or OBS transform.
- Do not introduce a second renderer/output window, native Win32 settings dialog, or generic native control styling. The owned WebView2 Settings window is the authorised application-interface exception to the single-renderer-window rule.

## General, tools, and companion integration

- Avatar Suite is a companion to RearSilver Stream Suite. Shared capabilities must use the same placement, labels, visual hierarchy, and interaction patterns so Stream Suite users can find them without learning a second information architecture.
- Feedback & Diagnostics is a first-class Tools destination rather than a card hidden inside General. It follows Stream Suite's feedback-first, privacy-explicit workflow: collect descriptive feedback, refresh and preview a redacted report, copy it, optionally export one diagnostic package, and open the local logs folder. Nothing is submitted automatically.
- Avatar diagnostics should report relevant application state such as version and build, renderer and output state, selected capture method, Spout2 sender state, microphone availability, WebView2/runtime information, and local logs while excluding credentials and unrelated personal content.
- Updates is a first-class Tools destination. Reuse Stream Suite's updater behaviour and presentation for installed version, update status, release changes, download progress, cancellation, installer verification, release notes, and installer handoff instead of creating a separate update model.
- General contains persistent application and companion preferences. When Stream Suite is installed, it may offer an explicit `Open Avatar Suite with Stream Suite` integration. Stream Suite owns that launch relationship; Avatar Suite must not register itself for Windows startup.
- Companion support requires the 64-bit Stream Suite uninstall registration, an absolute registered `InstallLocation`, the exact `Control Hub\\RearSilver-Stream-Suite-Control-Hub.exe` executable, and `AvatarCompanionSchema >= 1`. Do not use `DisplayIcon`, Program Files, PATH, development checkout, or other fallback discovery.
- The saved companion preference is `%LOCALAPPDATA%\\RearSilver Avatar\\settings.ini`, section `[Avatar]`, key `OpenWithStreamSuite`. Persist `1` for enabled and `0` for disabled while preserving the Unicode INI format; only `1` is enabled.
- The Avatar installer registers the 64-bit uninstall key `HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RearSilver Avatar Suite`, with an absolute `InstallLocation` containing exactly `RearSilver Avatar Suite.exe`. Stream Suite uses only that registration to discover Avatar.
- Stream Suite launches Avatar with `CREATE_BREAKAWAY_FROM_JOB` and never owns its shutdown. Avatar enforces a definitive named-mutex single-instance guard so concurrent or racing launches cannot create multiple renderers.
- The validated startup chain is OBS → Stream Suite Control Hub → Avatar Suite. Neither companion should steal focus. Closing OBS or Stream Suite must not close Avatar, and either product's manual launch button must use its registered companion path. Re-launching Avatar while it is running must retain one process and one renderer.
- Installer artifacts follow the Stream Suite channel layout: Owner builds under `artifacts/owner/<owner-version>` and time-gated beta builds under `artifacts/private-beta/<beta-version>`. Owner is the unrestricted development and system-test build. Owner and Private Beta use separate build caches and artifact roots.
- The `windows-owner` and `windows-private-beta` configure/build presets create clean application payloads under their profile-specific `artifacts/<profile>/<version>/app` directories. `scripts/build-installer.ps1 -Profile <profile>` validates that staged payload and creates its NSIS package under the matching `installer` directory. Generated artifacts and downloaded prerequisites remain untracked.
- `scripts/build-installer.ps1 -Profile private-beta` creates an independently labelled, time-limited Private Beta payload and installer under `artifacts/private-beta/<beta-version>`. Owner and beta artifacts must never share an output directory.
- The Owner installer installs to `%ProgramFiles%\\RearSilver Avatar Suite`, bundles verified Microsoft Visual C++ and WebView2 prerequisites, creates Start Menu launch and uninstall shortcuts, and writes standard 64-bit uninstall metadata. Uninstalling the application must not delete the user's `%LOCALAPPDATA%\\RearSilver Avatar` configuration.
- Installer upgrades replace only known Avatar Suite files and owned subdirectories. They must never recursively delete a user-selected installation root or unrelated files stored there.
- The installer presents Avatar Suite's own software licence and branded header/welcome artwork. Microsoft prerequisites are installed only when required, and WebView2 must be verified after installation before the product files are committed.
- A future silent updater handoff must preserve the installed instance and coordinate application shutdown without relying on the current interactive close/retry prompt. Update-handoff behavior is not added until the updater is implemented and tested.
- After a successful application update, the updater offers a one-time `Run guided setup` action independently of the permanent Settings button. Declining it preserves the existing completed-setup flag and settings; accepting it intentionally restarts the guide without resetting saved configuration.
- Do not add an always-on-top option. Avatar Suite must not claim scarce screen space over a streamer's game, OBS, chat, or automation tools.
- Do not add a control that permanently hides the renderer quick-navigation rail unless an equally discoverable recovery mechanism is designed and approved first. The rail must not be made effectively unrecoverable by a saved preference.
- Help is a first-class Tools destination for capture setup, avatar configuration, troubleshooting, documentation, version/build information, third-party licences, and relevant application-data locations.

## Motion and reaction effects

- Motion and effects are composited onto the same avatar transform and must apply consistently to primary, reaction, and blink image states without changing the fixed output canvas or OBS-owned geometry.
- Idle effects planned for the product are float, breathing, and an optional darker idle appearance.
- Microphone-reaction effects planned for the product are float, bounce, squash and stretch, shake, tilt, breathing, and an optional lighter reaction appearance.
- The darker-idle/lighter-reaction option is a visual treatment for users who want an obvious speaking state without supplying a separate reaction image. It must work with one source image and must not require duplicated image assets.
- Effect controls belong to the shared WebView2 Settings components so the guided setup can reuse them later.
- Present whole-avatar effects as an ordered stack. A single Add Effect control opens a plain-language effect picker; selecting an effect adds and expands its settings card and leaves Add Effect available beneath the stack.
- Keep the complete effect catalogue visible in the picker. Effects already present in the stack are greyed out, disabled, and labelled `Already added` so users can discover available effect types without accidentally adding duplicate instances.
- Each effect card supports enable or bypass without losing settings, expand or collapse, removal, and reordering. The stored list is the authoritative processing order. Initially allow one instance of each whole-avatar effect type; layer-specific effects may later reuse the same card pattern independently.
- Show an informative empty state when no effects are configured. Removing one effect must not alter the configuration or phase of any remaining effect.
- Adding or changing an effect must not reset blink phase, interrupt microphone detection, create transform jumps between image states, or disturb DirectComposition and OBS output stability.
- When an effect has a meaningful intensity range, pair its exact numeric value with plain-language range names and clickable markers. Wording may vary by effect so the labels describe the visible result. Squash and stretch uses Subtle, Animated, Cartoon, and Absurd across a 0–300% range; existing and future effects should use equivalent descriptive scales where they improve configuration.
- Squash and stretch is a one-shot, bottom-centre-anchored reaction animation that squashes, stretches, and eases back to the neutral transform when speech begins. It composes with continuous effects without resetting their phase and permits deliberately extreme cartoon deformation while clamping scale above inversion or collapse.
- Shake runs continuously while microphone reaction is active and settles smoothly back to the current composed position when reaction ends. It provides direction, intensity, speed, and optional rotation-wobble controls; uses continuous deterministic motion so image-state changes do not restart its phase; and composes independently with bounce, breathing, and squash and stretch.
- Idle/Reaction Brightness is one combined whole-avatar effect with separate idle and reaction brightness values plus a bidirectional transition time. Reaction brightness remains active for the microphone reaction state, using the detector's existing release timing rather than a second effect duration. Brightness transitions continue smoothly through reaction, blink, and image changes and apply after the avatar is assembled so inherited layers receive the same treatment.
- Float is a continuous whole-avatar effect available during idle, reaction, or both states. It provides one travel height, cycle duration, and optional left or right horizontal drift with a configurable distance. `Idle and reaction` means one uninterrupted path: microphone state changes must not change its transform, amplitude, phase, or position. When the effect starts or stops, or its height, drift, or direction changes, preserve the exact currently rendered offset and blend the path correction away so the avatar cannot jump to a different point on the updated path.
- Tilt is a reaction-held, bottom-centre-anchored whole-avatar effect. It eases to a configurable angle when microphone reaction begins, holds that angle for the reaction state, and eases back upright when reaction ends. Direction may be fixed left, fixed right, or alternate on each new reaction; an alternating reaction keeps one direction throughout. Tilt rotation composes additively with Shake wobble and must not reset or alter any continuous effect phase.

## Microphone reaction detection

- Microphone capture exists only to drive avatar state. RearSilver Avatar Suite must not play, monitor, record, route, or expose captured microphone audio to OBS.
- Measure reaction input with a smoothed RMS-style loudness envelope rather than raw sample peaks so short clicks and background fluctuations do not dominate the detector or meter.
- Provide a three-second background-noise calibration. The user remains silent while their normal room noise is present; Avatar stores the measured noise floor and positions the reaction threshold by a configurable sensitivity offset above it.
- Preserve manual reaction-threshold and release-delay controls. Manual threshold changes and the displayed sensitivity offset must remain consistent with one another.
- Open the reaction state immediately when the threshold is reached. Use a short release delay, approximately 100 ms by default, and small hysteresis to close naturally between words without flickering around the threshold.
- Noise calibration and gating affect only avatar detection. They must not alter the selected Windows device, the audio available to other applications, or OBS audio configuration.

## Layered avatar composition

- Each preset owns one visible, ordered composition stack modelled after familiar illustration and image-editing applications. Items nearer the top of the displayed stack render in front of items below them; changing the displayed order changes the real compositing order.
- The fixed Primary avatar appears exactly once in the root stack as a clearly labelled locked item. It cannot be removed, renamed, hidden, transformed from the Layers page, placed inside a group, or dragged; its artwork and avatar-wide scale remain editable through the Avatar page. User layers and groups may move above or below it, making their relationship to Primary explicit without a separate `Move behind` or `Move in front` model.
- Each preset supports up to 16 user-created image layers. Groups organise those layers and do not consume image-layer slots. The interface creates only the layers and groups the user requests and does not show permanently empty placeholders.
- Root items may be ungrouped image layers or one-level groups. Initial group support does not require nested groups. A group has an editable name, expand or collapse state, visibility, position, pivot, scale, rotation, opacity, horizontal mirror state, whole-avatar inheritance state, and its own ordered local-effect stack.
- A group contains an ordered list of image layers. The top child renders in front of lower children. A user may move layers into or out of groups and reorder root items or group children without re-importing artwork.
- A group occupies one position in the root composition and therefore sits wholly above or wholly below Primary. Artwork that must straddle Primary uses clearly named groups such as `Hair Front` and `Hair Back`; do not duplicate Primary inside groups or render it more than once.
- Every image layer has a name, visibility state, local position, pivot or anchor, uniform scale, optional independent horizontal and vertical scale, rotation, opacity, horizontal mirror state, optional descriptive purpose, whole-avatar inheritance state, and an ordered layer-effect stack.
- Intended uses include front and back hair, eye housings, pupils, mouths, clothing, hats, glasses, held props, redeem overlays, tails, background elements, and foreground visual effects. Purpose labels help organise layers and recommend useful effects but never restrict the layer-effect catalogue.
- Artwork exported on the same canvas as the base avatar defaults to position `0,0`, scale `100%`, and matching alignment. Cropped artwork supports explicit local positioning and pivot adjustment.
- Apply transforms hierarchically in this order: image pixels, layer transform, ordered layer effects, group transform, ordered group effects, then the shared avatar-root transform and inherited whole-avatar effects. Moving a group moves every child while preserving each child's local placement and animation.
- Layers and groups inherit whole-avatar effects by default. Each provides a clearly worded inheritance control for exceptional content that must remain stationary or visually independent. Inheritance is separate from layer effects and does not copy the whole-avatar Effects catalogue into a layer.
- Whole-avatar brightness and other root appearance effects affect all inheriting visible content uniformly. Layer and group opacity remain local multipliers and must compose predictably with inherited appearance.
- Existing presets migrate without changing appearance: current behind-Primary layers retain their relative order below the locked Primary item, current front layers retain their relative order above it, and all existing layers begin ungrouped.

### Layer and group effects

- Layer effects are a separate catalogue of local component behaviours, not duplicates of the Primary whole-avatar effects. They modify one image layer or one group before optional whole-avatar inheritance is applied.
- Every layer and group may browse, add, remove, enable or bypass, expand or collapse, reorder, and stack every compatible entry in the layer-effect catalogue. Purpose labels may place recommendations first but must not prevent unconventional combinations.
- The initial layer-effect catalogue includes Sway, Bounded Movement, Dangle or Spring, Flutter, Orbit, Local Pulse, Local Spin, State Visibility, Reaction Nudge, and Artwork State Change. Names and final grouping may be refined through runtime testing, but these remain local component behaviours.
- Sway rotates around a configurable local pivot with angle, cycle, easing, state applicability, and optional reaction intensity. `Tail Wag` is a recommended Sway configuration rather than a renderer-exclusive Tail effect.
- Bounded Movement provides horizontal and vertical ranges, speed or cycle, motion style, state applicability, smooth return-to-origin, and optional elliptical constraint. `Pupil Movement` is a recommended Bounded Movement configuration applied to a pupil component, not movement of a complete eye housing.
- Dangle or Spring follows parent movement with delayed settling; Flutter introduces restrained irregular local position or rotation; Orbit moves around a configurable local centre; Local Pulse changes local scale or opacity; and Local Spin rotates locally around the configured pivot.
- State Visibility controls whether a component appears during idle, reaction, both, or an automation-driven state. Reaction Nudge performs a one-shot local movement when reaction starts. Artwork State Change selects layer-specific idle, reaction, or named image variants when a transform cannot represent the desired change.
- Each effect card exposes only the properties relevant to that local behaviour and uses the established ordered-card interaction from the main Effects page. A new layer or group starts with an empty local stack.
- Multiple local effects must compose rather than overwrite one another: position offsets add, rotations add in stack order, scale factors multiply, and opacity factors multiply. Enabling, disabling, adding, removing, or reordering one effect must not reset another effect's phase or produce a transform jump.
- Local effects remain attached when the group or assembled avatar moves. Changing idle, reaction, blink, or layer artwork must not reset local effect phase unless the selected effect explicitly represents a one-shot state transition.
- Layer and group automation should expose visibility, position, scale, rotation, opacity, enabled effects, and named artwork variants while preserving the same composition hierarchy.
- Layer texture loading must respect GPU-memory limits. PNG file size is not a measure of decoded texture memory; the interface should report useful estimated decoded memory and avoid loading unused variants until required.

### Built-in layer templates and delivery sequence

- Avatar Suite will include an optional catalogue of bundled layer templates for users who do not have suitable artwork or image-editing experience. Initial candidates include googly eyes, blush, tears, a sweat drop, hearts or stars, reaction symbols, simple glasses, and seasonal accessories.
- A built-in template creates ordinary groups and image layers using the same renderer, hierarchy, transforms, visibility, effects, whole-avatar inheritance, preset ownership, and portable-package rules as user-created content. Do not create a template-only composition or persistence path.
- The initial googly-eyes template creates separate Left Eye and Right Eye groups. Each contains a stationary eye-housing layer and a pupil layer above it. Housing and pupil share a 512×512 transparent coordinate space; the pupil receives a recommended Bounded Movement configuration with an artwork-safe elliptical limit.
- Paired-eye template controls support left-group position, right-group position, linked group size, pupil movement range and speed, and idle or reaction applicability. Users may expand the groups and edit, reorder, transform, or replace the individual components.
- Built-in artwork remains clearly identified as bundled content. Users can reorder, hide, group, ungroup, transform, mirror, save, duplicate, import, and export configured templates through the same interactions used by other composition items.
- Delivery order is: implement and validate the unified root stack and group hierarchy; migrate existing presets; complete the generic layer and group effect stack; then add paired-eye groups and googly eyes as the first bundled template before expanding the catalogue.

## Preset ownership and editing

- A preset is the authoritative saved snapshot of one complete avatar configuration. It owns the idle, reaction, idle-blink, and reaction-blink images; the ordered root composition containing locked Primary, groups, and ungrouped layers; group membership and child order; every group and layer transform, visibility state, purpose, mirror state, inheritance choice, artwork variant, and local-effect stack; avatar scale and whole-avatar mirror state; blink configuration; and the ordered whole-avatar effect stack with every effect value.
- Imported PNGs are copied into an application-managed, content-addressed asset library. Presets reference stable asset identifiers so moving or deleting an original user file cannot break the saved avatar, and presets may share identical assets without unnecessary duplication.
- Managed asset paths and content hashes are private implementation details. Presets separately retain the original PNG filename for display and portable export without retaining or exposing the user's source directory. Legacy packages without filename metadata display a neutral `Imported PNG` label rather than a hash.
- Each preset has a stable internal identifier independent of its editable display name. Automation targets the identifier so renaming a preset cannot break a saved WebSocket action.
- The Presets page manages creation, selection, duplication, renaming, deletion, import, and export. Avatar, Blink, Effects, and Layers edit the currently active preset rather than maintaining separate disconnected configurations.
- A new preset begins with the bundled primary and reaction mascots, no optional blink images, no user layers, and the coded default settings. Asynchronous image uploads are bound to the preset generation that initiated them and must never write a late result into a newly selected preset.
- Export creates one `.rasavatar` package from the selected preset's last saved snapshot. It includes the complete composition hierarchy, group and layer effect stacks, original display filenames, and only the referenced managed PNG assets; unfinished drafts are excluded. Import validates the package structure, entry names, hierarchy, size limits, and asset SHA-256 hashes, installs assets into the local content-addressed library, assigns a new stable preset ID, and selects the imported preset through the normal dirty-switch workflow.
- Every preset-scoped settings page visibly names the preset being edited and exposes the same `Update preset` and `Revert changes` controls. Controls update the renderer and preview immediately, but the complete preset remains an unsaved draft until `Update preset` atomically saves the full snapshot.
- `Revert changes` restores the last saved snapshot. Switching presets with unsaved changes must offer Update, Discard, and Remain choices. Closing the application preserves a recoverable draft rather than silently losing work.
- Global settings do not mark the preset dirty. These include microphone device and noise calibration, capture and output method, Stream Suite integration, WebSocket server configuration, and general application preferences.
- WebSocket preset activation always uses the last saved snapshot, never an unfinished draft. Switching a preset must apply its complete visual state atomically so images, layers, blink settings, and effects from different presets are never briefly mixed.
- The local WebSocket protocol provides dynamic discovery for presets and the active preset's layers, groups, and configured effects. Integrations display friendly names while retaining stable IDs, request a fresh catalogue after connecting or activating a preset, and refresh when Avatar Suite reports `catalog.changed` after catalogue-affecting Settings edits.
- Catalogue-change notifications support integration configuration such as native sub-action dropdowns. They are transport-level refresh signals and are not required to become user-facing automation triggers.
- Primary, reaction, and blink images may have different source resolutions. Layer size is anchored to the Primary composition scale and whole-avatar effects are applied as separate root factors, so changing state never resizes an aligned layer merely because the active PNG has different pixel dimensions.

## Future automation and event integration

- RearSilver Avatar Suite should support temporary preset or avatar-state changes triggered by external events, then restore the prior state automatically after a configured duration.
- Candidate triggers include Twitch channel-point redemptions, chat commands, subscriptions and other stream events routed through the user's automation tool; Stream Deck actions; and tools such as Streamer.bot, Mix It Up, and SAMMI.
- Use a documented localhost WebSocket control interface as the core integration boundary so external tools can select a preset or avatar state; show, hide, transform, or select a variant for a layer; trigger an effect; specify timing; and request restoration without controlling the UI.
- RearSilver Avatar Suite does not require direct Twitch authentication for this feature. Twitch and other platform events remain the responsibility of the user's existing stream bot or automation tool, which sends the corresponding local WebSocket command to Avatar Suite.
- A temporary command may specify a transition duration, active duration, and restoration-transition duration. Avatar Suite owns those timers and restores the permanent state itself; external automation must not need to pause or schedule a later revert command.
- Temporary changes need deterministic restoration and conflict handling. A newer command for the same property on the same layer replaces the earlier temporary override. Different properties and different layers may run concurrently. Expiry restores the latest permanent value rather than an obsolete captured value, and an expired earlier command must never overwrite a newer user or automation choice.
- Commands may omit the active duration to remain in effect until changed or cancelled. A dedicated cancellation command restores the relevant permanent value immediately or with a requested transition.
- Settings must provide a no-code WebSocket Action Builder. Users select the intended preset, state, effect, layer, property or variant; enter the value, transition, active duration, and restoration transition; test the action locally; and copy valid JSON without needing to understand or write code.
- The Action Builder must present a plain-language summary of the generated behaviour, validation errors in ordinary language, the local WebSocket address, raw JSON for advanced use, and connection examples suitable for supported automation tools.
- One saved automation action may contain an ordered list of steps. Avatar Suite executes those steps in list order so a single redeem can, for example, hide one layer, show another, disable one configured local effect, and enable another without replacing the complete preset.
- Sequence steps target stable preset, group, layer, and effect identifiers while the builder displays their editable names. Initial step types include preset activation, layer or group visibility, configured local-effect enable/disable, artwork variant selection, transforms, effect triggers, waits, and restoration.
- Automation layer and effect changes are runtime overrides rather than preset edits. They must never mark the preset dirty or become part of the saved snapshot unless the user explicitly makes the equivalent edit in Settings.
- Tool-specific connection preferences, including Streamer.bot's zero-based WebSocket client number, persist globally between Avatar Suite sessions and remain outside preset data.
- Users may save a tested action under a friendly name. External tools can then invoke that saved action with a short command while Avatar Suite owns its full behaviour, duration, transitions, restoration, and conflict handling.
- External automation must not resize, suspend, re-parent, or otherwise disturb the renderer, fixed output canvas, animation continuity, or OBS-owned transform.

## Overlay visibility and focus

- While the main RearSilver Avatar Suite renderer window is active, its small quick-navigation overlay is visible locally and may be included in OBS Game Capture.
- When the main renderer window loses active-window status, including when the owned Settings window receives focus, hide the entire D3D overlay and leave only the avatar/background composition visible locally and in OBS.
- Base quick-navigation visibility on activation of the main renderer window itself. Owned Settings and picker windows deliberately deactivate the renderer overlay even though they remain part of the RearSilver Avatar Suite application.
- An owned file picker takes foreground status away from the avatar window, so the overlay must disappear while the picker is open.
- Hiding the overlay must not change the avatar transform, animation, background, swap-chain dimensions, or OBS capture.

## Responsive controls and input

- Calculate one responsive `UiLayout` in the configured output-canvas coordinate space.
- Use the exact bounds from that `UiLayout` for rendering, hover state, pressed state, and pointer hit testing.
- Inverse-map local pointer coordinates through the exact DirectComposition aspect-fit scale and offset before testing those shared bounds. Ignore clicks in local letterbox or pillarbox margins.
- Do not maintain separate drawing and input rectangles.
- Do not use stale hard-coded hit rectangles based on the launch resolution.
- Controls must remain correctly positioned and clickable after resizing, maximising, restoring, snapping, or changing DPI. A visible control must never invoke a neighbouring action.
- The behavioural reference is Stream Suite: controls retain their identity, state, and correct hit area regardless of window size.

## Local presentation, resize ownership, and minimisation

- The UI thread publishes client-size changes; the render thread owns the D3D device context, DirectComposition transform, GPU resources, and presentation.
- Coalesce pending client-size changes so the render thread applies the latest available local preview transform.
- Do not call `ResizeBuffers` in response to application-window resizing, snapping, maximising, restoring, DPI changes, or minimisation. The composition swap chain remains at the configured output resolution.
- Ignore zero-sized client dimensions while minimised and apply the latest valid, non-zero DirectComposition transform after restoration.
- Continue rendering and calling `Present` while the application is obscured or minimised so OBS frame delivery and animation do not pause or throttle.

## OBS output, Game Capture, and adapter selection

- Diagnostic 18 is authoritative for adapter enumeration, legacy shared-resource interoperability scoring, D3D11 device creation, and the validated swap-chain creation lifecycle.
- Prefer the ordinary Windows default adapter on ties. Select another related hardware-adapter alias only when it has strictly broader interoperability.
- Never hard-code an adapter index, GPU name, or LUID. LUID values may change between Windows sessions.
- The validated DirectComposition Game Capture path works with SLI/Crossfire Capture Mode disabled and satisfies the stable-geometry requirement on the tested system.
- Diagnostic 19 is authoritative for the fixed composition swap-chain creation, DirectComposition visual attachment, aspect-fit local transform, inverse pointer mapping, minimised presentation, and premultiplied-alpha behavior.
- Keep RivaTuner Statistics Server compatibility separate from renderer selection. If RTSS or similar graphics-hook software is detected, show this notice:

  > RivaTuner Statistics Server is running. If OBS Game Capture is blank or frozen, open RTSS Setup and enable “Use Microsoft Detours API hooking”.

- Do not automatically change OBS or RTSS settings.

## Image loading and file picker

- Opening the file picker must not pause the render loop, animation, or OBS output.
- Decode a selected image into validated CPU-side RGBA data without making the active GPU texture unavailable.
- Queue the decoded image for the render thread, create the replacement GPU texture there, and atomically swap it into use only after creation succeeds.
- Loading, cancelling, or failing to load a replacement image must not reset the animation phase or produce a frozen interval in OBS.
- Cancelling or rejecting a file must leave the current image unchanged.

## Renderer-baseline acceptance checks

The first renderer baseline must demonstrate all of the following together:

- PNG loading and successful atomic replacement.
- Basic avatar motion.
- Supported background modes.
- Normal OBS Game Capture using the Diagnostic 18 adapter-selection behaviour.
- Proportional local presentation at launch size and after resizing, maximising, restoring, and snapping, without changing the output canvas.
- An overlay UI that never changes avatar geometry.
- Complete focus-driven hiding and restoration of the overlay.
- Correct control hit testing at launch size and at resized, maximised, restored, and snapped sizes.
- Continuous rendering and animation while the file picker is open, including successful selection and cancellation.
- Safe minimisation and restoration without resizing the fixed swap chain or interrupting presentation.

The first renderer baseline validated these application-side behaviours. Diagnostic 19 then validated the fixed-output presentation architecture that is now the active production integration target.

## Stable-output acceptance checks

Before an output architecture can be considered production-ready, it must demonstrate all of the following together:

- OBS receives the configured composition width and height at launch.
- Resizing, maximising, restoring, snapping, minimising, and repositioning the application do not change those OBS source dimensions.
- A source positioned, scaled, cropped, aligned, and locked in OBS does not move or change effective scene geometry during any application-window transition.
- Avatar animation and output frame pacing continue while the application is obscured, minimised, or displaying an owned file picker.
- Alpha and all supported background modes remain correct.
- The working renderer, responsive overlay, input mapping, PNG replacement, motion, adapter selection, and RTSS handling remain intact.
- The implementation does not expose a second user-facing output window or require users to manage a hidden capture window.

Diagnostic 19 passed these checks on the target system with a fixed 1920×1080 composition swap chain: OBS acquisition, newly-created source dimensions, free resize and snap, maximise and restore, stable OBS scene geometry, aspect-fit local presentation, inverse pointer mapping, full-rate minimised animation, premultiplied alpha, and transparent/opaque backgrounds.

Profiles, microphone-driven states, blinking, and effects are outside the first reconstructed baseline unless separately approved.

## Background navigation

- Backgrounds have a dedicated top-level Settings page, separate from capture and output configuration.
- The quick rail order is Presets, Reactions, WebSocket, Backgrounds, Tools, then Settings.
- Clicking Backgrounds opens or focuses Settings directly on the Backgrounds page. It does not cycle or change the active background.
- Clicking Tools opens or focuses the owned interface directly on Feedback & Diagnostics. Clicking Settings opens or focuses the last visited Settings destination rather than the last visited Tools destination.
- Direct navigation must also work while the Settings WebView is still starting.
- Game Capture and Spout2 offer Transparent or Background image; Window Capture offers Solid colour, Chroma key, or Background image.
- Spout2 publishes the existing fixed 1920 × 1080 premultiplied-alpha D3D11 frame under the sender name `RearSilver Avatar Suite`. The sender exists only while Spout2 is selected and releases its resources when another capture method is chosen or the app closes.
- Selecting Spout2 clearly states that the separate OBS Spout2 plugin is required, links to its official release page, shows sender status, and instructs the user to add a Spout2 Capture source with Premultiplied Alpha compositing.
- Solid and chroma backgrounds support custom colours; chroma also offers common colour presets. Their controls appear only in their applicable mode.
- Background images support Contain, Cover, Stretch, and Tile fitting.
- In Window Capture mode, the selected background fills the complete application client area and the avatar renderer remains transparent above it, so resizing never exposes a hardcoded window colour.
- The restored renderer window starts with a 16:9 client area and preserves that client aspect ratio during manual edge or corner resizing. This prevents letterboxing in ordinary Window Capture use without changing the fixed Game Capture canvas.
- Maximising the renderer or pressing F11 enters borderless fullscreen across the selected monitor's complete bounds. F11, Escape, Restore, or a double-click outside the quick-navigation rail restores the prior framed window placement.
- Game Capture retains its fixed 1920 × 1080 output and uses transparency unless a background image is selected.
- Capture methods retain their own previous background choice when the user switches between them; the selected image and fitting are shared.
- A user can remove a previously selected background image without replacing it.

## Rejected architecture record

The fixed-child capture experiment established that OBS could capture a fixed 960×720 child swap chain while the parent changed size. When the child became fully clipped, explicitly hidden, or minimised with its parent, presentation and OBS animation throttled significantly. That approach did not satisfy the requirement that ordinary window management leave stream animation unaffected.

The fixed child swap chain, GDI preview, second output window, and child capture HWND are rejected for production and must not be reintroduced without a separate product decision. This rejects that implementation, not the stable-output requirement itself.

