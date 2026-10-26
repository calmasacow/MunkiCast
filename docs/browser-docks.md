# Custom browser docks

Open **Docks > Custom Browser Docks…**, enter a dock name and a complete HTTP or HTTPS
URL, then select **Add dock**. For example:

```text
https://example.com/chat
```

Close the editor to interact with the page. Drag its tab to arrange it alongside the other
panels. **Docks > Browser Docks** toggles saved browser panels. Closing a panel keeps its
name and URL; **Remove** in the editor deletes it from every workspace. **Edit** changes its
name or URL. Back, Reload/Stop and Home act on the embedded page; Home returns to the saved URL.

Browser pages retain their state across tab changes, docking and workspace changes. Editing
the browser dock list recreates the browser views. Cookies and website storage persist between
normal application runs. User-initiated popup windows support website login flows. Web pages
receive no OBS scripting API or access to studio controls.

Definitions are saved atomically beside the layout file, with the suffix
`.browser-docks.json`; panel arrangements use the existing layout persistence. Qt WebEngine
Quick is required to build the UI.

The regular UI `--check` verifies the editor, URL validation, interactive embedded HTML,
reopening, persistence, removal and save failures without depending on an external website.
To test a real page separately:

```sh
./build/linux/apps/libertas-ui/libertas-ui --check \
  --check-browser-url 'https://example.com/' \
  --check-shots /tmp/libertas-browser-docks
```

The standard build uses local FFmpeg 9.1 by default; `./tools/run-ffmpeg-9.1.sh` also launches it.
Checks use temporary dock definitions and browser storage, leaving normal settings untouched.
