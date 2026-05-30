# ZMK Custom Settings Web UI

React + TypeScript UI for the `zmk__custom_settings` custom Studio RPC
subsystem.

The UI runs in a browser with Web Serial support. Connect a keyboard whose
firmware enables `CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC`, then use the Settings
view to list values, edit one selected setting, save/discard/reset matching
settings, or export/import visible RPC-readable values as JSON.

On split keyboards the list view requests all sources. Editing a selected row
targets that row's source, while Save, Discard, Reset, and JSON import apply to
all sources that match the selected scope.

## Commands

```bash
npm install
npm run generate
npm run dev
npm test
npm run build
```

`npm run generate` runs `buf generate` and writes protobuf TypeScript types under
`src/proto/`.

## Project Structure

```text
src/
├── main.tsx
├── App.tsx
├── App.css
└── proto/
    └── zmk/cormoran/custom_settings/custom_settings.ts

test/
├── App.spec.tsx
└── RPCTestSection.spec.tsx
```

The protobuf schema lives at
`../proto/zmk/cormoran/custom_settings/custom_settings.proto`.
