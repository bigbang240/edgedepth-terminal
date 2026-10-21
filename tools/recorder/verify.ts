// Verify a recorded .edpack exactly the way the terminal's reader does:
// prefix → header → per-block zstd decompress → [u32 LE len | PackFrame] records.
// Usage: bun verify.ts <file.edpack>
import { readFileSync } from "node:fs";
import { load as protoLoad } from "protobufjs";
import { join } from "node:path";

const file = process.argv[2];
if (!file) {
  console.error("usage: bun verify.ts <file.edpack>");
  process.exit(1);
}
const buf = readFileSync(file);
const proto = await protoLoad(join(import.meta.dir, "protos", "messages.proto"));
const PackHeader = proto.lookupType("pb.PackHeader");
const PackFrame = proto.lookupType("pb.PackFrame");
const WSPayload = proto.lookupType("pb.WSPayload");

if (buf.subarray(0, 4).toString("ascii") !== "EDPK") throw new Error("bad magic");
const version = buf[4];
const headerLen = buf.readUInt32LE(5);
const header = PackHeader.decode(buf.subarray(9, 9 + headerLen));
console.log("version:", version);
console.log("event_id:", header.eventId);
console.log("exchange/symbol:", `${header.exchange}/${header.symbol}`);
console.log("window:", `${new Date(Number(header.startTsMs)).toISOString()} .. ${new Date(Number(header.endTsMs)).toISOString()}`);
console.log("total_frames:", header.totalFrames.toString());
console.log("blocks:", header.blocks.length);
console.log("ob_seed:", header.obSeed.length, "bytes @", header.obSeedSrcTsMs.toString());
console.log("candle_seeds:", header.candleSeeds.map((s) => `tf=${s.timeframeSec} ${s.candles.length}B`).join(", "));
const desc = header.description || "";
console.log("description:", desc.slice(0, 80));

const dataStart = 9 + headerLen;
let frames = 0;
let tsMin = Infinity, tsMax = -Infinity;
const streamCounts: Record<number, number> = {};
let badOrder = 0;
let prevTs = 0;
for (let i = 0; i < header.blocks.length; i++) {
  const b = header.blocks[i]!;
  const off = dataStart + Number(b.offset);
  const comp = buf.subarray(off, off + Number(b.length));
  const plain = Bun.zstdDecompressSync(comp);
  let p = 0;
  let count = 0;
  while (p < plain.length) {
    const len = plain.readUInt32LE(p);
    p += 4;
    const frame = PackFrame.decode(plain.subarray(p, p + len));
    p += len;
    const ts = Number(frame.tsMs);
    if (ts < prevTs) badOrder++;
    prevTs = ts;
    tsMin = Math.min(tsMin, ts);
    tsMax = Math.max(tsMax, ts);
    streamCounts[frame.stream] = (streamCounts[frame.stream] || 0) + 1;
    frames++;
    count++;
  }
  if (count !== Number(b.frameCount)) throw new Error(`block ${i}: count ${count} != ${b.frameCount}`);
  if (count === 0) throw new Error(`block ${i}: empty`);
}
console.log("frames walked:", frames, "(header says", header.totalFrames.toString() + ")");
console.log("ts range:", new Date(tsMin).toISOString(), "..", new Date(tsMax).toISOString());
console.log("monotonic violations (across block boundary):", badOrder);
console.log("streams:", JSON.stringify(streamCounts));
// ob seed sanity
if (header.obSeed.length) {
  const BookUpdate = proto.lookupType("pb.BookUpdate");
  const ob = BookUpdate.decode(header.obSeed);
  console.log(
    "ob_seed snapshot:", ob.snapshot,
    "asks:", ob.asks.length, "bids:", ob.bids.length,
    "@", new Date(Number(ob.timestampMs)).toISOString(),
  );
}
console.log("candle seeds:", header.candleSeeds.length);
console.log("VERIFY OK");
