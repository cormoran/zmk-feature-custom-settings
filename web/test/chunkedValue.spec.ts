import {
  readValueChunked,
  writeValueChunked,
  CHUNK_DATA_MAX,
} from "../src/chunkedValue";
import {
  Request,
  Response,
  SettingRef,
  SettingWriteMode,
} from "../src/proto/cormoran/zmk/custom_settings/custom_settings";

const ref: SettingRef = { key: "large_bytes" };

function makeValue(size: number): Uint8Array {
  const out = new Uint8Array(size);
  for (let i = 0; i < size; i++) {
    out[i] = (i * 7 + 3) & 0xff;
  }
  return out;
}

describe("chunked large-value transfer", () => {
  it("reassembles a value read across multiple ReadValueChunk frames", async () => {
    const value = makeValue(200);
    const callRPC = jest.fn(async (request: Request): Promise<Response> => {
      const offset = request.readValueChunk?.offset ?? 0;
      const end = Math.min(offset + CHUNK_DATA_MAX, value.length);
      return Response.create({
        valueChunk: {
          totalSize: value.length,
          offset,
          data: value.slice(offset, end),
          last: end >= value.length,
        },
      });
    });

    const result = await readValueChunked(callRPC, ref);
    expect(result.length).toBe(value.length);
    expect(Array.from(result)).toEqual(Array.from(value));
    // 200 bytes over 128-byte chunks = 2 reads.
    expect(callRPC).toHaveBeenCalledTimes(2);
  });

  it("splits a value into ordered WriteValueChunk frames and commits the last", async () => {
    const value = makeValue(200);
    const assembled: number[] = [];
    let declaredTotal = -1;
    let committed = false;

    const callRPC = jest.fn(async (request: Request): Promise<Response> => {
      const chunk = request.writeValueChunk!;
      if (chunk.offset === 0) {
        declaredTotal = chunk.totalSize;
      }
      expect(chunk.offset).toBe(assembled.length);
      assembled.push(...Array.from(chunk.data));
      if (chunk.commit) {
        committed = true;
      }
      return Response.create({
        status: { affectedCount: assembled.length, message: "ok" },
      });
    });

    await writeValueChunked(
      callRPC,
      ref,
      value,
      SettingWriteMode.SETTING_WRITE_MODE_PERSIST
    );

    expect(declaredTotal).toBe(value.length);
    expect(committed).toBe(true);
    expect(assembled).toEqual(Array.from(value));
    expect(callRPC).toHaveBeenCalledTimes(2);
  });

  it("throws when a chunk RPC returns an error", async () => {
    const callRPC = jest.fn(async (): Promise<Response> => {
      return Response.create({ error: { message: "Unlock required" } });
    });

    await expect(readValueChunked(callRPC, ref)).rejects.toThrow(
      "Unlock required"
    );
  });
});
