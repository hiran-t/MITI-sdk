/**
 * Just enough CDR (little-endian, as ROS 2 serializes it) to read and write the fixture's
 * messages. Alignment counts from the end of the 4-byte encapsulation header.
 */

const HEADER = 4;
const CDR_LE = 0x01;
const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();

export class CdrReader {
  private bytes: Uint8Array;
  private view: DataView;
  private offset = HEADER;

  constructor(bytes: Uint8Array) {
    if (bytes.length < HEADER || bytes[1] !== CDR_LE)
      throw new Error(`Not little-endian CDR (header ${Array.from(bytes.subarray(0, HEADER))})`);
    this.bytes = bytes;
    this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  uint8(): number {
    return this.view.getUint8(this.offset++);
  }

  bool(): boolean {
    return this.uint8() !== 0;
  }

  int32(): number {
    this.align(4);
    const value = this.view.getInt32(this.offset, true);
    this.offset += 4;
    return value;
  }

  uint32(): number {
    this.align(4);
    const value = this.view.getUint32(this.offset, true);
    this.offset += 4;
    return value;
  }

  float64(): number {
    this.align(8);
    const value = this.view.getFloat64(this.offset, true);
    this.offset += 8;
    return value;
  }

  /** A string: its length including the terminating NUL, then the bytes. */
  string(): string {
    const length = this.uint32();
    const value = textDecoder.decode(this.bytes.subarray(this.offset, this.offset + length - 1));
    this.offset += length;
    return value;
  }

  /** A `uint8[]`: its length, then the bytes. */
  uint8Array(): Uint8Array {
    const length = this.uint32();
    const value = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return value;
  }

  private align(size: number): void {
    this.offset += (size - ((this.offset - HEADER) % size)) % size;
  }
}

export class CdrWriter {
  private bytes: number[] = [0x00, CDR_LE, 0x00, 0x00];

  bool(value: boolean): this {
    this.bytes.push(value ? 1 : 0);
    return this;
  }

  uint32(value: number): this {
    this.align(4);
    const chunk = new Uint8Array(4);
    new DataView(chunk.buffer).setUint32(0, value, true);
    this.bytes.push(...chunk);
    return this;
  }

  string(value: string): this {
    const text = textEncoder.encode(value);
    this.uint32(text.length + 1);
    this.bytes.push(...text, 0);
    return this;
  }

  toBytes(): Uint8Array {
    return Uint8Array.from(this.bytes);
  }

  private align(size: number): void {
    while ((this.bytes.length - HEADER) % size) this.bytes.push(0);
  }
}

// ── The fixture's message types ──

export interface Header {
  sec: number;
  nanosec: number;
  frameId: string;
}

function header(reader: CdrReader): Header {
  return { sec: reader.int32(), nanosec: reader.uint32(), frameId: reader.string() };
}

export function readString(data: Uint8Array): string {
  return new CdrReader(data).string();
}

export function writeString(value: string): Uint8Array {
  return new CdrWriter().string(value).toBytes();
}

export function readImage(data: Uint8Array) {
  const reader = new CdrReader(data);
  return {
    header: header(reader),
    height: reader.uint32(),
    width: reader.uint32(),
    encoding: reader.string(),
    isBigendian: reader.uint8(),
    step: reader.uint32(),
    data: reader.uint8Array(),
  };
}

export function readCompressedImage(data: Uint8Array) {
  const reader = new CdrReader(data);
  return { header: header(reader), format: reader.string(), data: reader.uint8Array() };
}

export function readPointCloud2(data: Uint8Array) {
  const reader = new CdrReader(data);
  const result = { header: header(reader), height: reader.uint32(), width: reader.uint32() };
  const fields = Array.from({ length: reader.uint32() }, () => ({
    name: reader.string(),
    offset: reader.uint32(),
    datatype: reader.uint8(),
    count: reader.uint32(),
  }));
  return {
    ...result,
    fields,
    isBigendian: reader.bool(),
    pointStep: reader.uint32(),
    rowStep: reader.uint32(),
    data: reader.uint8Array(),
    isDense: reader.bool(),
  };
}

export function readTFMessage(data: Uint8Array) {
  const reader = new CdrReader(data);
  return Array.from({ length: reader.uint32() }, () => ({
    header: header(reader),
    childFrameId: reader.string(),
    translation: { x: reader.float64(), y: reader.float64(), z: reader.float64() },
    rotation: {
      x: reader.float64(),
      y: reader.float64(),
      z: reader.float64(),
      w: reader.float64(),
    },
  }));
}

export function writeSetBoolRequest(data: boolean): Uint8Array {
  return new CdrWriter().bool(data).toBytes();
}

export function readSetBoolResponse(data: Uint8Array) {
  const reader = new CdrReader(data);
  return { success: reader.bool(), message: reader.string() };
}
