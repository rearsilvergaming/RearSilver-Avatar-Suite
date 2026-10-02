# SAMMI integration

`RearSilver Avatar Suite.sef` connects SAMMI Bridge 9.02 or newer to Avatar Suite's local automation server.

## Current scope

- Verifies the connection with a protocol handshake and reconnects automatically.
- Retrieves the live catalogue for the active Avatar Suite setup.
- Refreshes automatically when Avatar Suite reports a catalogue change.
- Populates friendly SAMMI selectors for presets, layers, groups, and configured effects.
- Translates each friendly selection to its stable Avatar Suite ID before sending the command.
- Supports primary-avatar effects and layer/group effects.
- Keeps an advanced ordered-sequence JSON command available for testing.

The catalogue contains a `savedActions` collection reserved for a future saved-action editor. It is currently empty.

## Install and test

1. Start RearSilver Avatar Suite.
2. Start SAMMI Core and its Bridge.
3. In SAMMI, choose **Bridge → Install an Extension** and select `RearSilver Avatar Suite.sef`.
4. Open the **RearSilver Avatar Suite** tab in Bridge. Confirm that it says **Connected and verified** and lists the active preset and catalogue totals.
5. Create or edit a SAMMI button and open **Extension Commands → SAMMI Bridge**.
6. Add an Avatar Suite command. Its selector should contain the friendly names from the currently active setup.
7. Run the button and confirm the requested change in Avatar Suite.

The extension refreshes automatically after catalogue changes and preset activation. **Refresh Catalogue** remains available as a manual recovery action.

During development, open `bridge.html` in a browser and use its developer console if the extension installs but does not run. SAMMI inserts SEF contents into that file and does not reject every JavaScript syntax error during installation.
