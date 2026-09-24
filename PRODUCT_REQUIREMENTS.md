# RearSilver Avatar product requirements

## OBS Game Capture compatibility

- Select the D3D11 render adapter by testing legacy shared-resource interoperability between related hardware adapter aliases. Prefer the ordinary Windows default adapter on ties and select another related alias only when it has strictly broader interoperability.
- Never hard-code an adapter index, GPU name, or LUID. LUID values can change between Windows sessions.
- Normal OBS Game Capture must work with SLI/Crossfire Capture Mode disabled.
- Keep RivaTuner Statistics Server compatibility separate from renderer selection. If RTSS or similar graphics-hook software is detected, show this notice:

  > RivaTuner Statistics Server is running. If OBS Game Capture is blank or frozen, open RTSS Setup and enable “Use Microsoft Detours API hooking”.

- Do not automatically change OBS or RTSS settings.

## Capture canvas and window sizing

- RearSilver Avatar uses one user-facing application window. Do not introduce a separate capture/output window without explicit product approval.
- The OBS/Game Capture output has its own configurable width and height.
- Resizing, maximizing, restoring, or rearranging the RearSilver Avatar desktop window must not change the capture source's native dimensions.
- Lay out native controls and the preview inside the same window. Scale the logical capture canvas proportionally into the available preview area and letterbox it when the aspect ratios differ.
- Preserve OBS transforms, crops, and scene layouts when the application window size changes.
- Keep application controls and menus outside the captured render output. OBS should receive only the configured background and avatar composition.
- Give the avatar independent scale and position controls inside the capture canvas. Do not reserve permanent canvas space for the application menu.

## Image loading

- Continue rendering and animating the current avatar while the file picker is open.
- Loading, cancelling, or failing to load a replacement image must not reset the current animation phase or produce a frozen interval in OBS.
- Replace the active image atomically after decoding succeeds. Cancelling or rejecting a file leaves the current image unchanged.

