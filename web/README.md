# ZMK Custom Settings Web UI

React + TypeScript UI for the `cormoran_custom_settings` custom Studio RPC
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
    ├── cormoran/zmk/custom_settings/custom_settings.ts
    └── cormoran/zmk/custom_settings/custom_settings_relay.ts

test/
├── App.spec.tsx
└── RPCTestSection.spec.tsx
```

The protobuf schemas live under `../proto/cormoran/zmk/custom_settings/`.

## Code and memory guide

Open `architecture.html` from the console header, or visit
`http://localhost:5173/zmk-feature-custom-settings/architecture.html` with the
default `npm run dev` configuration. With `VITE_BASE=/`, use `/architecture.html`.
This Japanese guide works without a keyboard connection. It has a separate
Vite entry point, so it does not mount Studio connection or RPC hooks.

`src/architecture/ArchitecturePage.tsx` contains the reading sequence, with
`ValueLayoutSection`, `LifetimeSection`, `StorageSection` and `ResultsSection`
keeping each chapter and its local interaction together;
`Diagrams.tsx` contains the diagram primitives; `content.ts` contains the file
map, source revision, measurements and review findings. `architecture.css` is
loaded only by this page. Update the revision, measurements and explanations
together when firmware changes; source links intentionally pin the measured
implementation instead of following moving line numbers.

The guide covers descriptor/state/value ownership, compatibility ON/OFF,
borrowed versus copied reads, array and pool layout, keyspace generations,
RPC lifetime and the limits of static RAM measurements. Both HTML entries are
included in root-hosted previews and GitHub Pages project builds.
