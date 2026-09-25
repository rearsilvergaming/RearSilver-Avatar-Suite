# RearSilver Avatar reconstruction requirements

This document is the authoritative product and reconstruction specification. The first reconstructed renderer is intentionally limited to the responsive overlay, image loading, basic motion, background modes, and OBS compatibility described below, but validating that renderer does not waive the stable-output requirement.

## Stable OBS output is the product goal

- The OBS-facing composition has a stable configured width and height independent of the RearSilver Avatar application window.
- A user may freely position, scale, crop, align, and optionally lock the RearSilver Avatar source in OBS. RearSilver must continue supplying the same intrinsic canvas underneath that OBS-owned transform.
- Resizing, maximising, restoring, snapping, minimising, or repositioning the application window must not change the OBS source's native dimensions, effective scene scale, position, crop, alignment, or bounding geometry.
- Locking a source in OBS does not protect its scene geometry when the source's native dimensions change. Therefore a dynamically client-sized source does not satisfy this requirement merely because the OBS item is locked.
- A maximised application client area is not necessarily the monitor's full resolution. Window chrome and the taskbar can produce dimensions such as 1920×1009 on a 1920×1080 display; this must not become the stream source resolution.
- The application window remains a freely resizable local preview and control surface. Its dimensions must not be authoritative for the stream composition.
- Veadotube's window-following source behaviour is a compatibility reference only and is not RearSilver Avatar's target user experience.
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

## Authoritative UI direction

- RearSilver Avatar is a standalone companion product in the RearSilver Stream Suite family. Use the same core navigation and control language so users encounter familiar tabs, panels, buttons, typography, spacing, states, terminology, and cyan-accented dark visual treatment across both products.
- The Stream Suite settings pages and guided setup are the direct visual and behavioural references. Avatar-specific previews, meters, thumbnails, and icons may provide product identity without creating a separate interface language.
- The floating collapsible sidebar and the temporary large D3D settings panel have been retired and removed. The renderer now contains only a small vertical quick-navigation rail; heavyweight settings belong to the owned WebView2 interface.
- The normal focused renderer view uses a small vertical quick-navigation icon rail inside the fixed D3D composition. It may briefly appear in Game Capture while the user interacts directly with the renderer.
- The quick-navigation rail hides whenever the main renderer window is not the active interaction surface, including while the owned Settings window has focus. This returns Game Capture to the clean avatar/background composition during configuration.
- Full Settings and Guided Setup are not rendered into the fixed OBS canvas. Settings opens in an owned native window containing WebView2, with Stream Suite-style horizontal tab navigation, clear page headings, short explanatory copy, and grouped cards or control sections.
- The owned Settings window is application UI rather than an OBS output surface. It must not modify, resize, suspend, re-parent, or otherwise disturb the validated DirectComposition renderer, its animation, its fixed canvas, or the user's OBS transform.
- Native-to-HTML communication uses WebView2 messages following the Stream Suite hosting pattern. Avatar may reuse Stream Suite styling and assets where appropriate while retaining its own product identity and content.
- First run uses a guided setup in the owned WebView2 interface based directly on the Stream Suite pattern: visible progress, a page title and explanation, grouped settings, automatic progress saving, and Back, Skip for now, and Continue actions.
- Guided setup and normal Settings reuse the same controls, layout components, validation, and stored settings. Do not implement separate copies of the same configuration workflow.
- The initial candidate setup areas are Output, Avatar Images, Microphone, Voice Detection, Blink and Motion, and Review and Finish. Their exact names, grouping, order, and page count remain provisional until real control density and workflow testing justify the final structure.
- The quick-navigation rail is overlay content and must never alter the avatar transform or configured OBS canvas. The owned Settings window and its WebView content remain outside that composition entirely.
- Closing Settings returns focus to the Avatar renderer without changing the avatar, animation phase, output canvas, background, or OBS transform.
- Do not introduce a second renderer/output window, native Win32 settings dialog, or generic native control styling. The owned WebView2 Settings window is the authorised application-interface exception to the single-renderer-window rule.

## Overlay visibility and focus

- While the main RearSilver Avatar renderer window is active, its small quick-navigation overlay is visible locally and may be included in OBS Game Capture.
- When the main renderer window loses active-window status, including when the owned Settings window receives focus, hide the entire D3D overlay and leave only the avatar/background composition visible locally and in OBS.
- Base quick-navigation visibility on activation of the main renderer window itself. Owned Settings and picker windows deliberately deactivate the renderer overlay even though they remain part of the RearSilver Avatar application.
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

## Rejected architecture record

The fixed-child capture experiment established that OBS could capture a fixed 960×720 child swap chain while the parent changed size. When the child became fully clipped, explicitly hidden, or minimised with its parent, presentation and OBS animation throttled significantly. That approach did not satisfy the requirement that ordinary window management leave stream animation unaffected.

The fixed child swap chain, GDI preview, second output window, and child capture HWND are rejected for production and must not be reintroduced without a separate product decision. This rejects that implementation, not the stable-output requirement itself.

