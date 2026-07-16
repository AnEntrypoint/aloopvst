# Installing aloopvst

Grab `aloop.vst3` and/or `aloop.exe` (Standalone) from the latest green
[CI run](https://github.com/AnEntrypoint/aloopvst/actions) artifacts, or build
locally (see the root `CMakeLists.txt` — needs `faust` on `PATH` for local
builds; CI pre-generates the DSP C++ so it doesn't need faust installed).

## VST3 (Windows)

Copy the whole `aloop.vst3` folder (it's a bundle, not a single file) into a
folder Ableton actually scans. The default system-wide location is:

```
C:\Program Files\Common Files\VST3\
```

Ableton can also be configured (Preferences → Plug-ins) to scan additional
folders — some setups add `Documents\Ableton\User Library` itself as a custom
VST3 folder, alongside Live's own native content, and other plugins living
directly in that folder work fine there. Whichever folder(s) your install is
actually configured to scan, put `aloop.vst3` there.

After copying, tell Ableton to rescan (Preferences → Plug-ins → "Rescan" or
just restart Live) — it only scans configured folders on startup or on an
explicit rescan trigger, not automatically when a file appears.

## VST3 (macOS)

Copy `aloop.vst3` into `~/Library/Audio/Plug-Ins/VST3/` (per-user) or
`/Library/Audio/Plug-Ins/VST3/` (system-wide), then rescan in your host.

## VST3 (Linux)

Copy `aloop.vst3` into `~/.vst3/` or `/usr/lib/vst3/`, then rescan.

## Standalone

`aloop.exe` (Windows) / `aloop.app` (macOS) / the Linux standalone binary need
no installation — run directly. Useful for testing without a DAW, but you'll
need to pick an audio device and a MIDI input device from the app's own
audio/MIDI settings (usually an options button/menu in the title bar) before
you'll hear or control anything.
