# Mix It Up integration

RearSilver Avatar Suite installs a small local command helper for Mix It Up because Mix It Up's C# Script Action does not reference the .NET WebSocket assembly.

## Configure an action

1. Keep RearSilver Avatar Suite running.
2. In Avatar Suite, open **Settings → WebSocket → Mix It Up**.
3. Build and test the ordered action sequence.
4. In Mix It Up, edit the command or event that should control the avatar.
5. Add an **External Program Action**.
6. Select `RearSilver Avatar Suite Automation.exe` from this folder.
7. Paste the generated arguments from Avatar Suite into the action's arguments field.
8. Enable **Wait Until Complete**. Optionally enable **Save Output** to store Avatar Suite's JSON response.
9. Test the action while Avatar Suite is running.

The helper only connects to Avatar Suite's local WebSocket server at `ws://127.0.0.1:17891/`. It does not connect to the internet or accept connections.
