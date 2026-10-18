[中文](README.zh.md) | **English**

# ui_sandbox Official Screenshot Matrix (52 Images)

Official design acceptance assets rendered offscreen by ui_sandbox: 13 views × light/dark themes × two sizes (1200/960). Components, layout parameters, and stylesheets are fully identical to production (no DownloadService is constructed; zero engine/network side effects).

## Matrix Composition

- `download_table_*` / `download_grid_*`: downloading view (table/grid)
- `download_completed_*` / `download_completed_grid_*`: completed view (seeding column / seeding card summary for seeding tasks in place)
- `cloud_*`: cloud storage space; `discovery_*`: resource discovery; `trash_*`: recycle bin
- `settings_*`: settings page full window + four close-up groups (`_seeding`/`_trash`/`_search`/`_about` — below the scroll area's fold line, not visible in full-window screenshots)
- `add_dialog_*`: add download dialog (the most frequently used interaction surface outside the main window)

Size suffix `_1200` = production default 1200×800, `_960` = minimum window 960×640 (squeeze scenario — overlap-type layout defects only show up under narrow windows).

## Regeneration

```bash
cmake --build build-desktop --target falcon-ui-sandbox
build-desktop/bin/falcon-ui-sandbox docs/design/ui-sandbox
```

The screenshot script and demo data are in `apps/desktop/dev/ui_sandbox.cpp`.
