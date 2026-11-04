/**
 * The client side of the link benchmark: subscribes to /tf and the camera, and reports how late
 * they arrive (arrival time minus the header stamp; the robot stamps with the same clock).
 *   node bench/link/measure.mts <url> raw|jpeg [camera fps] [seconds]
 * The camera is asked for at most `camera fps` (15, as MITI's Image panel by default; 0 for every
 * frame). The reference bridge sends every frame.
 */
import { SUBPROTOCOLS, parseBinary, type MessageData } from '../../conformance/src/protocol.ts';
import { readCompressedImage, readImage, readTFMessage } from '../../conformance/src/cdr.ts';

const [url = 'ws://127.0.0.1:8765', camera = 'raw', fps = '15', seconds = '20'] =
  process.argv.slice(2);
const WARMUP_MS = 5000;
const READY_MS = 180000;

interface Sample {
  at: number;
  lateMs: number;
  bytes: number;
}

const stampMs = (header: { sec: number; nanosec: number }) =>
  header.sec * 1000 + header.nanosec / 1e6;

function percentile(values: number[], p: number): number {
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.min(sorted.length - 1, Math.floor((p / 100) * sorted.length))] ?? NaN;
}

function summary(samples: Sample[], durationMs: number) {
  const late = samples.map((sample) => sample.lateMs);
  const round = (value: number) => Math.round(value * 10) / 10;
  return {
    messages: samples.length,
    perSecond: round(samples.length / (durationMs / 1000)),
    lateP50Ms: round(percentile(late, 50)),
    lateP95Ms: round(percentile(late, 95)),
    lateMaxMs: round(Math.max(...late)),
    meanKB: round(samples.reduce((sum, s) => sum + s.bytes, 0) / samples.length / 1000),
  };
}

function open(): Promise<WebSocket> {
  const deadline = Date.now() + READY_MS;
  return new Promise((resolve, reject) => {
    const attempt = () => {
      const ws = new WebSocket(url, SUBPROTOCOLS);
      ws.binaryType = 'arraybuffer';
      ws.onopen = () => resolve(ws);
      ws.onerror = () => {
        if (Date.now() > deadline) reject(new Error(`No bridge at ${url}`));
        else setTimeout(attempt, 1000);
      };
    };
    attempt();
  });
}

const ws = await open();
const subscriptions = new Map<number, 'tf' | 'camera'>();
const samples = { tf: [] as Sample[], camera: [] as Sample[] };
let measuring = false;
let ids = 0;

ws.onmessage = (event) => {
  const at = Date.now();
  if (typeof event.data === 'string') {
    const message = JSON.parse(event.data);
    if (message.op === 'advertise') {
      for (const channel of message.channels) {
        const kind =
          channel.topic === '/tf' ? 'tf' : channel.topic === '/camera/image_raw' ? 'camera' : null;
        if (!kind || Array.from(subscriptions.values()).includes(kind)) continue;
        const id = ids++;
        subscriptions.set(id, kind);
        ws.send(JSON.stringify({ op: 'subscribe', subscriptions: [{ id, channelId: channel.id }] }));
        if (kind === 'camera' && ws.protocol === 'miti.bridge.v1')
          ws.send(
            JSON.stringify({
              op: 'miti.subscriptionOptions',
              subscriptionId: id,
              maxRate: Number(fps) || null,
              ...(camera === 'jpeg' ? { transform: { kind: 'jpeg', quality: 80 } } : {}),
            })
          );
      }
    } else if (message.op === 'status' && message.level > 0) {
      console.error('status:', message.message);
    } else if (message.op === 'miti.subscriptionFormat') {
      console.error('format:', message.schemaName);
    }
    return;
  }
  const parsed = parseBinary(event.data as ArrayBuffer);
  if (parsed.op !== 'messageData' || !measuring) return;
  const { subscriptionId, data } = parsed as MessageData;
  const kind = subscriptions.get(subscriptionId);
  if (kind === 'tf') {
    const [transform] = readTFMessage(data);
    if (transform) samples.tf.push({ at, lateMs: at - stampMs(transform.header), bytes: data.length });
  } else if (kind === 'camera') {
    // The jpeg transform changes the type; a raw Image has height and width before its data.
    const header = (() => {
      try {
        return readCompressedImage(data).header;
      } catch {
        return readImage(data).header;
      }
    })();
    samples.camera.push({ at, lateMs: at - stampMs(header), bytes: data.length });
  }
};

// Measures once both topics are flowing, after the link warmed up.
while (subscriptions.size < 2) await new Promise((resolve) => setTimeout(resolve, 200));
await new Promise((resolve) => setTimeout(resolve, WARMUP_MS));
measuring = true;
const durationMs = Number(seconds) * 1000;
await new Promise((resolve) => setTimeout(resolve, durationMs));
measuring = false;
ws.close();
console.log(
  JSON.stringify(
    {
      protocol: ws.protocol,
      camera,
      cameraMaxFps: ws.protocol === 'miti.bridge.v1' ? Number(fps) || 'every frame' : 'every frame',
      tf: summary(samples.tf, durationMs),
      cameraFrames: summary(samples.camera, durationMs),
    },
    null,
    2
  )
);
