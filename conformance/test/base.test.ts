/**
 * The base protocol (docs/protocol.md, "Base protocol"), against the fixture graph. Every server
 * MITI talks to must pass these: SDK servers, and foxglove_bridge until MITI drops it.
 */
import assert from 'node:assert/strict';
import { after, before, describe, it } from 'node:test';
import {
  readImage,
  readPointCloud2,
  readSetBoolResponse,
  readString,
  readTFMessage,
  writeSetBoolRequest,
  writeString,
} from '../src/cdr.ts';
import { FIXTURE, connectWhenReady } from '../src/fixture.ts';
import {
  Connection,
  SUBPROTOCOLS,
  isMessageData,
  type FetchAssetResponse,
  type JsonMessage,
  type MessageData,
  type ServerInfo,
  type ServiceCallResponse,
  type TimeMessage,
} from '../src/protocol.ts';

const RECORD_SEPARATOR = '='.repeat(80);

describe('base protocol', () => {
  let connection: Connection;
  let info: ServerInfo;
  const can = (capability: string) => info.capabilities.includes(capability);

  before(async () => {
    connection = await connectWhenReady();
    info = await connection.serverInfo();
  });

  after(() => connection?.close());

  describe('connection', () => {
    it('selects one of the offered subprotocols', () => {
      assert.ok(SUBPROTOCOLS.includes(connection.protocol), `selected "${connection.protocol}"`);
    });

    it('sends serverInfo first, with a name and capabilities', () => {
      assert.equal(connection.received[0]?.op, 'serverInfo');
      assert.equal(typeof info.name, 'string');
      assert.ok(Array.isArray(info.capabilities));
    });
  });

  describe('topics', () => {
    it('advertises a CDR channel with its ros2msg schema', async () => {
      const channel = await connection.channel(FIXTURE.string.topic);
      assert.equal(channel.encoding, 'cdr');
      assert.equal(channel.schemaName, FIXTURE.string.type);
      assert.equal(channel.schemaEncoding ?? 'ros2msg', 'ros2msg');
      assert.match(channel.schema, /^string data/m);
    });

    it('includes the types a schema uses, each after a separator and MSG line', async () => {
      const channel = await connection.channel(FIXTURE.image.topic);
      assert.ok(
        channel.schema.includes(`${RECORD_SEPARATOR}\nMSG: std_msgs/Header\n`),
        channel.schema
      );
      assert.ok(channel.schema.includes('MSG: builtin_interfaces/Time'), channel.schema);
    });

    it('sends message data for a subscription, with a timestamp', async () => {
      const channel = await connection.channel(FIXTURE.string.topic);
      const message = await connection.firstMessage(channel.id);
      assert.match(readString(message.data), /^tick \d+$/);
      assert.ok(message.timestamp > 0n, `timestamp ${message.timestamp}`);
    });

    it('stops sending a subscription after unsubscribe', async () => {
      const channel = await connection.channel(FIXTURE.string.topic);
      const since = connection.mark();
      const subscription = connection.subscribe(channel.id);
      try {
        await connection.messageData(subscription, { since });
      } finally {
        connection.unsubscribe(subscription);
      }
      // Messages already on the way when the server read the unsubscribe may still arrive.
      await new Promise((resolve) => setTimeout(resolve, 300));
      const late = await connection.collect(isMessageData(subscription), {
        since: connection.mark(),
        forMs: 1000,
      });
      assert.equal(late.length, 0, `${late.length} messages after unsubscribe`);
    });

    it('delivers an image as the fixture published it', async () => {
      const channel = await connection.channel(FIXTURE.image.topic, FIXTURE.image.type);
      const image = readImage((await connection.firstMessage(channel.id)).data);
      assert.equal(image.header.frameId, 'camera');
      assert.equal(image.width, FIXTURE.image.width);
      assert.equal(image.height, FIXTURE.image.height);
      assert.equal(image.encoding, 'rgb8');
      assert.equal(image.data.length, image.step * image.height);
      assert.deepEqual(Array.from(image.data.subarray(0, 4)), [0, 1, 2, 3]);
    });

    it('delivers a point cloud as the fixture published it', async () => {
      const channel = await connection.channel(FIXTURE.points.topic, FIXTURE.points.type);
      const cloud = readPointCloud2((await connection.firstMessage(channel.id)).data);
      assert.equal(cloud.width, FIXTURE.points.width);
      assert.deepEqual(
        cloud.fields.map((field) => field.name),
        ['x', 'y', 'z', 'intensity']
      );
      const view = new DataView(cloud.data.buffer, cloud.data.byteOffset, cloud.data.byteLength);
      assert.equal(view.getFloat32(cloud.pointStep + 4, true), 2);
    });

    it('delivers TF', async () => {
      const channel = await connection.channel(FIXTURE.tf.topic, FIXTURE.tf.type);
      const [transform] = readTFMessage((await connection.firstMessage(channel.id)).data);
      assert.equal(transform?.header.frameId, FIXTURE.tf.parent);
      assert.equal(transform?.childFrameId, FIXTURE.tf.child);
      assert.equal(transform?.translation.x, FIXTURE.tf.x);
      assert.equal(transform?.rotation.w, 1);
    });

    it('delivers a transient-local topic to a client that subscribes later', async () => {
      const { topic, type, parent, child } = FIXTURE.tfStatic;
      const channel = await connection.channel(topic, type);
      // This client keeps the topic subscribed, so the server's own subscription already took
      // the one message when the second client asks for it.
      const subscription = connection.subscribe(channel.id);
      try {
        await connection.messageData(subscription);
        const later = await connectWhenReady();
        try {
          const laterChannel = await later.channel(topic, type);
          const [transform] = readTFMessage((await later.firstMessage(laterChannel.id)).data);
          assert.equal(transform?.header.frameId, parent);
          assert.equal(transform?.childFrameId, child);
        } finally {
          later.close();
        }
      } finally {
        connection.unsubscribe(subscription);
      }
    });

    it('answers a subscription to an unknown channel with a status error', async () => {
      const since = connection.mark();
      connection.subscribe(0x7fffffff);
      const status = await connection.find(
        (message): message is JsonMessage => message.op === 'status',
        { since }
      );
      assert.equal(status.level, 2, JSON.stringify(status));
    });
  });

  describe('client publishing', () => {
    it('publishes CDR from a client channel', async (t) => {
      if (!can('clientPublish')) return t.skip('no clientPublish capability');
      if (!info.supportedEncodings?.includes('cdr')) return t.skip('cdr not in supportedEncodings');
      const echo = await connection.channel(FIXTURE.echo.out, FIXTURE.echo.type);
      const subscription = connection.subscribe(echo.id);
      const channelId = connection.id();
      connection.send({
        op: 'advertise',
        channels: [
          { id: channelId, topic: FIXTURE.echo.in, encoding: 'cdr', schemaName: FIXTURE.echo.type },
        ],
      });
      // A new publisher needs a moment to be discovered: send until the echo comes back.
      const text = `hello ${Date.now()}`;
      const since = connection.mark();
      const timer = setInterval(
        () => connection.sendClientMessage(channelId, writeString(text)),
        200
      );
      try {
        const isEcho = isMessageData(subscription);
        const message = await connection.find(
          (received): received is MessageData =>
            isEcho(received) && readString(received.data) === text,
          { since, timeoutMs: 10000 }
        );
        assert.equal(readString(message.data), text);
      } finally {
        clearInterval(timer);
        connection.unsubscribe(subscription);
        connection.send({ op: 'unadvertise', channelIds: [channelId] });
      }
    });
  });

  describe('services', () => {
    it('advertises a service with request and response schemas', async (t) => {
      if (!can('services')) return t.skip('no services capability');
      const service = await connection.service(FIXTURE.service.name);
      assert.equal(service.type, FIXTURE.service.type);
      const request = service.request?.schema ?? service.requestSchema;
      const response = service.response?.schema ?? service.responseSchema;
      assert.match(request ?? '', /^bool data/m);
      assert.match(response ?? '', /^bool success/m);
      assert.match(response ?? '', /^string message/m);
    });

    it('calls a service with a CDR request', async (t) => {
      if (!can('services')) return t.skip('no services capability');
      const service = await connection.service(FIXTURE.service.name);
      const callId = connection.id();
      const since = connection.mark();
      connection.sendServiceCall(service.id, callId, 'cdr', writeSetBoolRequest(true));
      const response = await connection.find(
        (message): message is ServiceCallResponse =>
          message.op === 'serviceCallResponse' && message.callId === callId,
        { since }
      );
      assert.equal(response.serviceId, service.id);
      assert.equal(response.encoding, 'cdr');
      assert.deepEqual(readSetBoolResponse(response.data), { success: true, message: 'set True' });
    });

    it('answers a call to an unknown service with serviceCallFailure', async (t) => {
      if (!can('services')) return t.skip('no services capability');
      const callId = connection.id();
      const since = connection.mark();
      connection.sendServiceCall(0x7fffffff, callId, 'cdr', writeSetBoolRequest(true));
      const failure = await connection.find(
        (message): message is JsonMessage =>
          (message.op === 'serviceCallFailure' && (message as JsonMessage).callId === callId) ||
          message.op === 'status',
        { since }
      );
      assert.equal(failure.op, 'serviceCallFailure', JSON.stringify(failure));
    });
  });

  describe('parameters', () => {
    it('reads a parameter, replying with the request id', async (t) => {
      if (!can('parameters')) return t.skip('no parameters capability');
      const id = `conformance-${connection.id()}`;
      const since = connection.mark();
      connection.send({ op: 'getParameters', parameterNames: [FIXTURE.parameter], id });
      const reply = await connection.find(
        (message): message is JsonMessage =>
          message.op === 'parameterValues' && (message as JsonMessage).id === id,
        { since }
      );
      const [parameter] = reply.parameters as { name: string; value: unknown }[];
      assert.equal(parameter?.name, FIXTURE.parameter);
      assert.match(String(parameter?.value), /<robot name="conformance">/);
    });
  });

  describe('assets', () => {
    const fetchAsset = async (uri: string) => {
      const requestId = connection.id();
      const since = connection.mark();
      connection.send({ op: 'fetchAsset', uri, requestId });
      return connection.find(
        (message): message is FetchAssetResponse =>
          message.op === 'fetchAssetResponse' && message.requestId === requestId,
        { since }
      );
    };

    it('serves a package:// mesh', async (t) => {
      if (!can('assets')) return t.skip('no assets capability');
      const response = await fetchAsset(FIXTURE.mesh);
      assert.equal(response.status, 0, response.error);
      assert.equal(response.error, '');
      assert.match(new TextDecoder().decode(response.data), /^solid cube/);
    });

    it('refuses a file outside its roots with a failed response', async (t) => {
      if (!can('assets')) return t.skip('no assets capability');
      const response = await fetchAsset(FIXTURE.forbiddenAsset);
      assert.equal(response.status, 1);
      assert.notEqual(response.error, '');
      assert.equal(response.data.length, 0);
    });
  });

  describe('time', () => {
    it('sends the robot clock', async (t) => {
      if (!can('time')) return t.skip('no time capability');
      const time = await connection.find(
        (message): message is TimeMessage => message.op === 'time',
        { since: connection.mark() }
      );
      assert.ok(time.timestamp > 0n);
    });
  });
});
