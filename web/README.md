# ZMK Custom Settings Web UI

React + TypeScript UI for the `zmk__custom_settings` custom Studio RPC
subsystem.

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
    └── zmk/custom_settings/custom_settings.ts

test/
├── App.spec.tsx
└── RPCTestSection.spec.tsx
```

The protobuf schema lives at
`../proto/zmk/custom_settings/custom_settings.proto`.
