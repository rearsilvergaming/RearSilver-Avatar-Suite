# RearSilver Avatar reconstruction requirements

This document is the authoritative specification for the first reconstructed application baseline. The initial baseline is intentionally limited to the renderer, responsive overlay, image loading, basic motion, background modes, and OBS compatibility described below.

## Application and rendering architecture

- Use one resizable, user-facing top-level window and one D3D11 swap chain attached directly to that window.
- Resize the swap chain to follow each valid, non-zero client size. Under this accepted fallback architecture, OBS Game Capture source dimensions may change when the application is resized, maximised, restored, or snapped.
- Preserve the avatar's intended proportions and keep the composition centred without stretching, distortion, or unintended cropping.
- The avatar and background form the base composition layer.
- The custom, branded sidebar and menu form a rendered D3D overlay above the base composition. They do not reserve layout space.
- Opening or closing the sidebar must not push, shrink, rescale, crop, or reposition the avatar composition.
- Do not replace the rendered interface with generic native Win32-looking controls merely to obtain automatic coordinate handling.
- Do not introduce a fixed capture child, GDI preview, child capture HWND, second output window, or second swap chain into the production baseline.

Avatar and UI layout must remain independent in the implementation:

```text
avatarTransform = calculateAvatarTransform(clientWidth, clientHeight, logicalCanvas)
uiLayout = calculateUiLayout(clientWidth, clientHeight, dpiScale, sidebarOpen)
```

`sidebarOpen`, `sidebarWidth`, and other overlay geometry must never be inputs to the avatar transform. In particular, do not calculate the avatar transform from `clientWidth - sidebarWidth`.

## Overlay visibility and focus

- While RearSilver Avatar is the foreground application, the complete overlay UI is visible locally and is intentionally included in OBS Game Capture.
- When RearSilver Avatar loses foreground application status, hide the entire overlay and leave only the avatar/background composition visible locally and in OBS.
- Treat interaction with UI owned by the RearSilver Avatar top-level window as part of the foreground application rather than requiring the foreground HWND to equal the top-level HWND exactly.
- An owned file picker takes foreground status away from the avatar window, so the overlay must disappear while the picker is open.
- Hiding the overlay must not change the avatar transform, animation, background, swap-chain dimensions, or OBS capture.

## Responsive controls and input

- Calculate one responsive `UiLayout` from the current client dimensions and DPI scale.
- Use the exact bounds from that `UiLayout` for rendering, hover state, pressed state, and pointer hit testing.
- Do not maintain separate drawing and input rectangles.
- Do not use stale hard-coded hit rectangles based on the launch resolution.
- Controls must remain correctly positioned and clickable after resizing, maximising, restoring, snapping, or changing DPI. A visible control must never invoke a neighbouring action.
- The behavioural reference is the Stream Suite sidebar: the buttons are the buttons regardless of window size.

## Resize ownership and minimisation

- The UI thread publishes client-size changes; the render thread owns the D3D device context, swap-chain resizing, GPU resources, and presentation.
- Coalesce pending resize requests so the render thread applies the latest available dimensions.
- A minimised or zero-sized client state must not call `ResizeBuffers` with a width or height of zero.
- Defer resizing while either dimension is zero and apply the latest valid, non-zero size after restoration.

## OBS Game Capture and adapter selection

- Diagnostic 18 is authoritative for adapter enumeration, legacy shared-resource interoperability scoring, D3D11 device creation, and the validated swap-chain creation lifecycle.
- Prefer the ordinary Windows default adapter on ties. Select another related hardware-adapter alias only when it has strictly broader interoperability.
- Never hard-code an adapter index, GPU name, or LUID. LUID values may change between Windows sessions.
- Normal OBS Game Capture must work with SLI/Crossfire Capture Mode disabled.
- Keep RivaTuner Statistics Server compatibility separate from renderer selection. If RTSS or similar graphics-hook software is detected, show this notice:

  > RivaTuner Statistics Server is running. If OBS Game Capture is blank or frozen, open RTSS Setup and enable “Use Microsoft Detours API hooking”.

- Do not automatically change OBS or RTSS settings.

## Image loading and file picker

- Opening the file picker must not pause the render loop, animation, or OBS output.
- Decode a selected image into validated CPU-side RGBA data without making the active GPU texture unavailable.
- Queue the decoded image for the render thread, create the replacement GPU texture there, and atomically swap it into use only after creation succeeds.
- Loading, cancelling, or failing to load a replacement image must not reset the animation phase or produce a frozen interval in OBS.
- Cancelling or rejecting a file must leave the current image unchanged.

## First reconstructed baseline acceptance checks

The first baseline must demonstrate all of the following together:

- PNG loading and successful atomic replacement.
- Basic avatar motion.
- Supported background modes.
- Normal OBS Game Capture using the Diagnostic 18 adapter-selection behaviour.
- Proportional rendering at launch size and after resizing, maximising, restoring, and snapping.
- An overlay UI that never changes avatar geometry.
- Complete focus-driven hiding and restoration of the overlay.
- Correct control hit testing at launch size and at resized, maximised, restored, and snapped sizes.
- Continuous rendering and animation while the file picker is open, including successful selection and cancellation.
- Safe minimisation and restoration without an invalid zero-sized swap-chain resize.

Profiles, microphone-driven states, blinking, and effects are outside the first reconstructed baseline unless separately approved.

## Rejected architecture record

The fixed-child capture experiment established that OBS could capture a fixed 960×720 child swap chain while the parent changed size. When the child became fully clipped, explicitly hidden, or minimised with its parent, presentation and OBS animation throttled significantly. That approach did not satisfy the requirement that ordinary window management leave stream animation unaffected.

The fixed child swap chain, GDI preview, second output window, second swap chain, and child capture HWND are rejected for the production baseline and must not be reintroduced without a separate product decision.

