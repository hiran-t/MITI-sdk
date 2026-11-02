/**
 * A client for the MITI protocol (docs/protocol.md), written from the document and small enough
 * to check servers with. It keeps every message it receives, so a check can look for one that
 * already arrived (serverInfo, advertise) as well as wait for the next.
 */

/** What MITI offers, most preferred first. */
export const SUBPROTOCOLS = ['miti.bridge.v1', 'foxglove.sdk.v1', 'foxglove.websocket.v1'];
export const MITI_SUBPROTOCOL = 'miti.bridge.v1';

const DEFAULT_TIMEOUT_MS = 5000;
const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();

export interface Channel {
  id: number;
  topic: string;
  encoding: string;
  schemaName: string;
  schema: string;
  schemaEncoding?: string;
}

export interface MessageDefinition {
  encoding: string;
  schemaName: string;
  schemaEncoding: string;
  schema: string;
}

export interface Service {
  id: number;
  name: string;
  type: string;
  request?: MessageDefinition;
  response?: MessageDefinition;
  requestSchema?: string;
  responseSchema?: string;
}

export interface ServerInfo extends JsonMessage {
  op: 'serverInfo';
  name: string;
  capabilities: string[];
  supportedEncodings?: string[];
  metadata?: Record<string, string>;
  sessionId?: string;
}

/** A text frame: one JSON object with an `op`. */
export interface JsonMessage {
  op: string;
  [field: string]: unknown;
}

export interface MessageData {
  op: 'messageData';
  subscriptionId: number;
  timestamp: bigint;
  data: Uint8Array;
}

export interface TimeMessage {
  op: 'time';
  timestamp: bigint;
}

export interface ServiceCallResponse {
  op: 'serviceCallResponse';
  serviceId: number;
  callId: number;
  encoding: string;
  data: Uint8Array;
}

export interface FetchAssetResponse {
  op: 'fetchAssetResponse';
  requestId: number;
  status: number;
  error: string;
  data: Uint8Array;
}

export type BinaryMessage = MessageData | TimeMessage | ServiceCallResponse | FetchAssetResponse;
export type Received = JsonMessage | BinaryMessage;

/** Parses a server → client binary frame (protocol.md, Binary frames). */
export function parseBinary(buffer: ArrayBuffer): BinaryMessage {
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  const opcode = view.getUint8(0);
  let offset = 1;
  const uint32 = () => {
    const value = view.getUint32(offset, true);
    offset += 4;
    return value;
  };
  const string = () => {
    const length = uint32();
    const value = textDecoder.decode(bytes.subarray(offset, offset + length));
    offset += length;
    return value;
  };
  switch (opcode) {
    case 0x01: {
      const subscriptionId = uint32();
      const timestamp = view.getBigUint64(offset, true);
      return { op: 'messageData', subscriptionId, timestamp, data: bytes.subarray(offset + 8) };
    }
    case 0x02:
      return { op: 'time', timestamp: view.getBigUint64(offset, true) };
    case 0x03: {
      const serviceId = uint32();
      const callId = uint32();
      const encoding = string();
      return {
        op: 'serviceCallResponse',
        serviceId,
        callId,
        encoding,
        data: bytes.subarray(offset),
      };
    }
    case 0x04: {
      const requestId = uint32();
      const status = view.getUint8(offset);
      offset += 1;
      const error = string();
      return { op: 'fetchAssetResponse', requestId, status, error, data: bytes.subarray(offset) };
    }
    default:
      throw new Error(`Unknown server binary opcode 0x${opcode.toString(16)}`);
  }
}

/** Opcode, then each part: numbers as uint32, strings length-prefixed, bytes as they are. */
function encodeBinary(opcode: number, ...parts: (number | string | Uint8Array)[]): Uint8Array {
  const chunks = parts.map((part) => {
    if (typeof part === 'number') {
      const chunk = new Uint8Array(4);
      new DataView(chunk.buffer).setUint32(0, part, true);
      return chunk;
    }
    if (typeof part === 'string') {
      const text = textEncoder.encode(part);
      const chunk = new Uint8Array(4 + text.length);
      new DataView(chunk.buffer).setUint32(0, text.length, true);
      chunk.set(text, 4);
      return chunk;
    }
    return part;
  });
  const out = new Uint8Array(1 + chunks.reduce((sum, chunk) => sum + chunk.length, 0));
  out[0] = opcode;
  let offset = 1;
  for (const chunk of chunks) {
    out.set(chunk, offset);
    offset += chunk.length;
  }
  return out;
}

/** Matches the message data of one subscription. */
export function isMessageData(subscriptionId: number) {
  return (message: Received): message is MessageData =>
    message.op === 'messageData' && (message as MessageData).subscriptionId === subscriptionId;
}

export interface FindOptions {
  /** Only messages received at or after this mark (see `mark()`); by default all of them. */
  since?: number;
  timeoutMs?: number;
}

export class Connection {
  readonly received: Received[] = [];
  private ws: WebSocket;
  private wakers = new Set<() => void>();
  private nextId = 1;
  private closed = false;

  private constructor(ws: WebSocket) {
    this.ws = ws;
    ws.binaryType = 'arraybuffer';
    ws.addEventListener('message', (event) => {
      const data = event.data as string | ArrayBuffer;
      this.received.push(
        typeof data === 'string' ? (JSON.parse(data) as JsonMessage) : parseBinary(data)
      );
      this.wakers.forEach((wake) => wake());
    });
    ws.addEventListener('close', () => {
      this.closed = true;
      this.wakers.forEach((wake) => wake());
    });
  }

  /** Opens a connection offering `protocols`; rejects if the handshake fails or times out. */
  static open(
    url: string,
    protocols = SUBPROTOCOLS,
    timeoutMs = DEFAULT_TIMEOUT_MS
  ): Promise<Connection> {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(url, protocols);
      const connection = new Connection(ws);
      const timer = setTimeout(() => {
        ws.close();
        reject(new Error(`No handshake with ${url} within ${timeoutMs} ms`));
      }, timeoutMs);
      ws.addEventListener('open', () => {
        clearTimeout(timer);
        resolve(connection);
      });
      ws.addEventListener('error', () => {
        clearTimeout(timer);
        reject(new Error(`Cannot connect to ${url} offering ${protocols.join(', ')}`));
      });
    });
  }

  /** The subprotocol the server selected. */
  get protocol(): string {
    return this.ws.protocol;
  }

  get isClosed(): boolean {
    return this.closed;
  }

  /** A fresh id for a subscription, client channel, call or request. */
  id(): number {
    return this.nextId++;
  }

  /** The current end of `received`, to look only at what arrives after an action. */
  mark(): number {
    return this.received.length;
  }

  /** The first received message that matches, waiting for one if none has arrived yet. */
  async find<T extends Received>(
    match: (message: Received) => message is T,
    { since = 0, timeoutMs = DEFAULT_TIMEOUT_MS }: FindOptions = {}
  ): Promise<T> {
    const deadline = Date.now() + timeoutMs;
    let index = since;
    for (;;) {
      for (; index < this.received.length; index++) {
        const message = this.received[index]!;
        if (match(message)) return message;
      }
      if (this.closed) throw new Error('The server closed the connection');
      const left = deadline - Date.now();
      if (left <= 0) throw new Error(`Nothing matching arrived within ${timeoutMs} ms`);
      await this.wait(left);
    }
  }

  /** Every received message that matches, after waiting `forMs`. */
  async collect<T extends Received>(
    match: (message: Received) => message is T,
    { since = 0, forMs }: { since?: number; forMs: number }
  ): Promise<T[]> {
    await new Promise((resolve) => setTimeout(resolve, forMs));
    return this.received.slice(since).filter(match);
  }

  serverInfo(): Promise<ServerInfo> {
    return this.find((message): message is ServerInfo => message.op === 'serverInfo');
  }

  /** The channels advertised and not unadvertised, by id. */
  channels(): Map<number, Channel> {
    const channels = new Map<number, Channel>();
    for (const message of this.received) {
      if (message.op === 'advertise')
        for (const channel of (message as JsonMessage).channels as Channel[])
          channels.set(channel.id, channel);
      if (message.op === 'unadvertise')
        for (const id of (message as JsonMessage).channelIds as number[]) channels.delete(id);
    }
    return channels;
  }

  /** The channel of `topic` (and `schemaName`, if given), waiting for it to be advertised. */
  async channel(
    topic: string,
    schemaName?: string,
    timeoutMs = DEFAULT_TIMEOUT_MS
  ): Promise<Channel> {
    const matches = (channel: Channel) =>
      channel.topic === topic && (!schemaName || channel.schemaName === schemaName);
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const found = Array.from(this.channels().values()).find(matches);
      if (found) return found;
      const left = deadline - Date.now();
      if (left <= 0 || this.closed)
        throw new Error(`${topic} was not advertised within ${timeoutMs} ms`);
      await this.wait(left);
    }
  }

  /** The services advertised and not unadvertised, by name. */
  services(): Map<string, Service> {
    const byId = new Map<number, Service>();
    for (const message of this.received) {
      if (message.op === 'advertiseServices')
        for (const service of (message as JsonMessage).services as Service[])
          byId.set(service.id, service);
      if (message.op === 'unadvertiseServices')
        for (const id of (message as JsonMessage).serviceIds as number[]) byId.delete(id);
    }
    return new Map(Array.from(byId.values()).map((service) => [service.name, service]));
  }

  async service(name: string, timeoutMs = DEFAULT_TIMEOUT_MS): Promise<Service> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const found = this.services().get(name);
      if (found) return found;
      const left = deadline - Date.now();
      if (left <= 0 || this.closed)
        throw new Error(`${name} was not advertised within ${timeoutMs} ms`);
      await this.wait(left);
    }
  }

  send(message: { op: string; [field: string]: unknown }): void {
    this.ws.send(JSON.stringify(message));
  }

  /** Subscribes to a channel and returns the subscription id. */
  subscribe(channelId: number): number {
    const id = this.id();
    this.send({ op: 'subscribe', subscriptions: [{ id, channelId }] });
    return id;
  }

  unsubscribe(subscriptionId: number): void {
    this.send({ op: 'unsubscribe', subscriptionIds: [subscriptionId] });
  }

  /** The next message data of a subscription, received after `since`. */
  messageData(subscriptionId: number, options?: FindOptions): Promise<MessageData> {
    return this.find(isMessageData(subscriptionId), options);
  }

  sendClientMessage(channelId: number, data: Uint8Array): void {
    this.ws.send(encodeBinary(0x01, channelId, data));
  }

  sendServiceCall(serviceId: number, callId: number, encoding: string, data: Uint8Array): void {
    this.ws.send(encodeBinary(0x02, serviceId, callId, encoding, data));
  }

  close(): void {
    this.ws.close();
  }

  private wait(timeoutMs: number): Promise<void> {
    return new Promise((resolve) => {
      const wake = () => {
        clearTimeout(timer);
        this.wakers.delete(wake);
        resolve();
      };
      const timer = setTimeout(wake, timeoutMs);
      this.wakers.add(wake);
    });
  }
}
