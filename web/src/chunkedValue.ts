// Chunked large-value transfer helpers (issue #16).
//
// A single-frame SettingValue only carries up to
// CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE (64) bytes, so a larger BYTES /
// STRING value is transferred over the ReadValueChunk / WriteValueChunk RPCs.
// These helpers assemble / split such a value; they take a plain
// `callRPC(request) => Promise<Response>` so they can be unit tested without a
// live connection.

import {
  Request,
  Response,
  SettingRef,
  SettingWriteMode,
} from "./proto/cormoran/zmk/custom_settings/custom_settings";

// Largest value that still fits a single non-chunked SettingValue frame. A
// value larger than this is read/written over the chunked RPC instead.
export const SINGLE_FRAME_VALUE_MAX = 64;
// Chunk payload size (matches WriteValueChunkRequest.data / ValueChunkResponse
// max_size:128 in the proto options).
export const CHUNK_DATA_MAX = 128;

export type CallRPC = (request: Request) => Promise<Response>;

function concatChunks(parts: Uint8Array[], total: number): Uint8Array {
  const out = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) {
    out.set(part.subarray(0, Math.min(part.length, total - offset)), offset);
    offset += part.length;
    if (offset >= total) {
      break;
    }
  }
  return out;
}

// Read a large BYTES/STRING value in full by repeating ReadValueChunk until the
// device reports the last chunk. Stateless on the device side, so chunks are
// requested in order starting at offset 0.
export async function readValueChunked(
  callRPC: CallRPC,
  setting: SettingRef
): Promise<Uint8Array> {
  const parts: Uint8Array[] = [];
  let offset = 0;
  let total = 0;

  // Guard against a misbehaving device by bounding the loop to a generous
  // number of chunks.
  for (let guard = 0; guard < 4096; guard++) {
    const resp = await callRPC(
      Request.create({ readValueChunk: { setting, offset } })
    );
    if (resp.error) {
      throw new Error(resp.error.message || "Read chunk failed");
    }
    const chunk = resp.valueChunk;
    if (!chunk) {
      throw new Error("Read chunk response had no value chunk");
    }
    total = chunk.totalSize ?? 0;
    const data = chunk.data ?? new Uint8Array();
    parts.push(data);
    offset += data.length;
    if (chunk.last || (data.length === 0 && offset >= total)) {
      break;
    }
    if (data.length === 0) {
      // No progress and not marked last: bail rather than spin forever.
      break;
    }
  }

  return concatChunks(parts, total);
}

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
