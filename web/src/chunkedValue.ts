// Chunked large-value write helper (issue #16 / simplification P2).
//
// The RX path buffers a whole frame before nanopb decodes it, so a BYTES /
// STRING value bigger than one frame still has to be written over several
// WriteValueChunk RPCs. Reading no longer needs chunking: GetSetting/
// ListSettings now stream a value of any size directly (the RPC transport's
// TX path streams its response incrementally, unlike RX), so
// ReadValueChunk/readValueChunked is gone - see App.tsx's getSetting, which
// just uses the plain GetSetting response.
//
// This helper takes a plain `callRPC(request) => Promise<Response>` so it can
// be unit tested without a live connection.

import {
  Request,
  Response,
  SettingRef,
  SettingWriteMode,
} from "./proto/cormoran/zmk/custom_settings/custom_settings";

// Largest value that still fits a single non-chunked SettingValue frame. A
// value larger than this is written over the chunked RPC instead (reads of
// any size go through the plain GetSetting response).
export const SINGLE_FRAME_VALUE_MAX = 64;
// Chunk payload size (matches WriteValueChunkRequest.data max_size:128 in the
// proto options).
export const CHUNK_DATA_MAX = 128;

export type CallRPC = (request: Request) => Promise<Response>;

// Write a large BYTES/STRING value by splitting it into CHUNK_DATA_MAX frames.
// The first frame (offset 0) opens the transfer and declares total_size; the
// final frame sets commit so the device validates and applies the assembled
// value atomically.
export async function writeValueChunked(
  callRPC: CallRPC,
  setting: SettingRef,
  bytes: Uint8Array,
  mode: SettingWriteMode
): Promise<void> {
  const total = bytes.length;
  let offset = 0;

  do {
    const end = Math.min(offset + CHUNK_DATA_MAX, total);
    const data = bytes.slice(offset, end);
    const commit = end >= total;
    const resp = await callRPC(
      Request.create({
        writeValueChunk: {
          setting,
          totalSize: total,
          offset,
          data,
          commit,
          mode,
        },
      })
    );
    if (resp.error) {
      throw new Error(resp.error.message || "Write chunk failed");
    }
    offset = end;
  } while (offset < total);
}
