# App icon

The repository root file **`icon.png`** is the single canonical app icon source.
Do not keep extra duplicate PNGs in the repo.

To regenerate the per-OS icon files used by packaging, run:

```sh
./src/icons/make-icons.sh
```

This derives the following files from `/icon.png`:

- `src/icons/icon.png`  — normalized 1024×1024 PNG
- `src/icons/icon.icns` — macOS app icon embedded in the `.app`
- `src/icons/icon.ico`  — Windows app icon embedded in the `.exe`

Linux/AppImage packaging uses `/icon.png` directly, and the Qt GUI embeds the
same `/icon.png` via `resources.qrc` for the runtime window icon.
