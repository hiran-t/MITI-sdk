/**
 * The extensions (docs/protocol.md, "Extensions"), for servers that select miti.bridge.v1.
 * Skipped against servers that do not, such as the reference bridge.
 */
import assert from 'node:assert/strict';
import { after, before, describe, it } from 'node:test';
import {
  readCompressedImage,
  readImage,
  readPointCloud2,
  readString,
  writeSetBoolRequest,
  writeString,
} from '../src/cdr.ts';
import { FIXTURE, SERVER_URL, connectWhenReady } from '../src/fixture.ts';
import {
  Connection,
  MITI_SUBPROTOCOL,
  type Received,
  isMessageData,
  type JsonMessage,
  type MessageData,
  type ServerInfo,
} from '../src/protocol.ts';

/** The fixture's string topic is published at 10 Hz. */
const STRING_HZ = 10;

describe('extensions', () => {
  let connection: Connection;
  let info: ServerInfo;
  let skip = '';

  before(async () => {
    connection = await connectWhenReady();
    info = await connection.serverInfo();
    if (connection.protocol !== MITI_SUBPROTOCOL)
      skip = `the server selected ${connection.protocol}, not ${MITI_SUBPROTOCOL}`;
  });

  after(() => connection?.close());

  /** The next status message after `since`. */
  const nextStatus = (since: number) =>
    connection.find((message): message is JsonMessage => message.op === 'status', { since });

  const isOp =
    (op: string) =>
    (message: { op: string }): message is JsonMessage =>
      message.op === op;

  const setOptions = (subscriptionId: number, options: Record<string, unknown>) =>
    connection.send({ op: 'miti.subscriptionOptions', subscriptionId, ...options });

  /** The next miti.subscriptionFormat of a subscription after `since`, and where it is. */
  const nextFormat = async (subscriptionId: number, since: number) => {
    const format = await connection.find(
      (message): message is JsonMessage =>
        message.op === 'miti.subscriptionFormat' &&
        (message as JsonMessage).subscriptionId === subscriptionId,
      { since }
    );
    return { format, index: connection.received.indexOf(format) };
  };

  const pause = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

  it('refuses a handshake that does not offer miti.bridge.v1', async (t) => {
    if (skip) return t.skip(skip);
    await assert.rejects(Connection.open(SERVER_URL, ['foxglove.websocket.v1']));
  });

  describe('miti.subscriptionOptions', () => {
    it('maxRate caps the messages sent', async (t) => {
      if (skip) return t.skip(skip);
      const channel = await connection.channel(FIXTURE.string.topic);
      const subscription = connection.subscribe(channel.id);
      try {
        const maxRate = 2;
        connection.send({ op: 'miti.subscriptionOptions', subscriptionId: subscription, maxRate });
        // Messages already queued when the options arrived may still come.
        await new Promise((resolve) => setTimeout(resolve, 300));
        const seconds = 2;
        const received = await connection.collect(isMessageData(subscription), {
          since: connection.mark(),
          forMs: seconds * 1000,
        });
        assert.ok(
          received.length <= maxRate * seconds + 1,
          `${received.length} messages in ${seconds} s at maxRate ${maxRate}`
        );
        assert.ok(received.length >= maxRate * seconds - 1, `only ${received.length} messages`);
      } finally {
        connection.unsubscribe(subscription);
      }
    });

    it('null maxRate removes the limit', async (t) => {
      if (skip) return t.skip(skip);
      const channel = await connection.channel(FIXTURE.string.topic);
      const subscription = connection.subscribe(channel.id);
      try {
        connection.send({
          op: 'miti.subscriptionOptions',
          subscriptionId: subscription,
          maxRate: 1,
        });
        connection.send({
          op: 'miti.subscriptionOptions',
          subscriptionId: subscription,
          maxRate: null,
        });
        await new Promise((resolve) => setTimeout(resolve, 300));
        const received = await connection.collect(isMessageData(subscription), {
          since: connection.mark(),
          forMs: 1000,
        });
        assert.ok(received.length >= STRING_HZ - 2, `${received.length} messages in 1 s`);
      } finally {
        connection.unsubscribe(subscription);
      }
    });

    it('answers an unknown subscription with a status error', async (t) => {
      if (skip) return t.skip(skip);
      const since = connection.mark();
      connection.send({ op: 'miti.subscriptionOptions', subscriptionId: 0x7fffffff, maxRate: 1 });
      assert.equal((await nextStatus(since)).level, 2);
    });

    it('answers an invalid maxRate with a status error', async (t) => {
      if (skip) return t.skip(skip);
      const channel = await connection.channel(FIXTURE.string.topic);
      const subscription = connection.subscribe(channel.id);
      try {
        const since = connection.mark();
        connection.send({
          op: 'miti.subscriptionOptions',
          subscriptionId: subscription,
          maxRate: 0,
        });
        assert.equal((await nextStatus(since)).level, 2);
      } finally {
        connection.unsubscribe(subscription);
      }
    });

    it('answers a transform with a status error without miti.transforms', async (t) => {
      if (skip) return t.skip(skip);
      if (info.capabilities.includes('miti.transforms'))
        return t.skip('the server has miti.transforms');
      const channel = await connection.channel(FIXTURE.image.topic);
      const subscription = connection.subscribe(channel.id);
      try {
        const since = connection.mark();
        connection.send({
          op: 'miti.subscriptionOptions',
          subscriptionId: subscription,
          transform: { kind: 'jpeg' },
        });
        assert.equal((await nextStatus(since)).level, 2);
      } finally {
        connection.unsubscribe(subscription);
      }
    });

    describe('keepNewest', () => {
      let reason = '';
      before(() => {
        if (skip) reason = skip;
        else if (!info.capabilities.includes('miti.keepNewest'))
          reason = 'no miti.keepNewest capability';
        else if (!info.capabilities.includes('clientPublish'))
          reason = 'no clientPublish capability, which the check publishes its burst with';
      });

      it('sends the last message of a burst once maxRate allows it', async (t) => {
        if (reason) return t.skip(reason);
        const echo = await connection.channel(FIXTURE.echo.out, FIXTURE.echo.type);
        const subscription = connection.subscribe(echo.id);
        const isEcho = isMessageData(subscription);
        const channelId = connection.id();
        connection.send({
          op: 'advertise',
          channels: [
            {
              id: channelId,
              topic: FIXTURE.echo.in,
              encoding: 'cdr',
              schemaName: FIXTURE.echo.type,
            },
          ],
        });
        try {
          // A new publisher needs a moment to be discovered: send until an echo comes back.
          const ready = connection.mark();
          const timer = setInterval(
            () => connection.sendClientMessage(channelId, writeString('ready')),
            200
          );
          try {
            await connection.find(isEcho, { since: ready, timeoutMs: 10000 });
          } finally {
            clearInterval(timer);
          }
          setOptions(subscription, { maxRate: 1, keepNewest: true });
          // Let the echoes still on their way arrive, and the rate's interval pass.
          await pause(2500);

          const since = connection.mark();
          const last = `last ${Date.now()}`;
          for (let i = 0; i < 5; i++) connection.sendClientMessage(channelId, writeString(`${i}`));
          connection.sendClientMessage(channelId, writeString(last));
          // The burst's first message goes at once; the last waits for the next second.
          const message = await connection.find(
            (received): received is MessageData =>
              isEcho(received) && readString(received.data) === last,
            { since, timeoutMs: 3000 }
          );
          const sent = connection.received.slice(since).filter(isEcho);
          assert.ok(sent.length <= 2, `${sent.length} messages at maxRate 1`);
          assert.equal(sent.at(-1), message, 'the last message of the burst came last');
        } finally {
          connection.unsubscribe(subscription);
          connection.send({ op: 'unadvertise', channelIds: [channelId] });
        }
      });

      it('answers a keepNewest that is not a boolean with a status error', async (t) => {
        if (reason) return t.skip(reason);
        const channel = await connection.channel(FIXTURE.string.topic);
        const subscription = connection.subscribe(channel.id);
        try {
          const since = connection.mark();
          setOptions(subscription, { maxRate: 1, keepNewest: 'yes' });
          assert.equal((await nextStatus(since)).level, 2);
        } finally {
          connection.unsubscribe(subscription);
        }
      });
    });

    // Over a fast local connection nothing waits long enough to be replaced or reordered;
    // libmiti's scheduler tests check these, and bench/link checks them over a slowed link.
    it.todo('latest delivery keeps only the newest waiting message');
    it.todo('high priority is sent before low');
  });

  // Base behavior (protocol.md, Transient-local topics), checked where a ROS graph can start a
  // publisher late: servers on the fixture graph that offer its service.
  describe('transient-local topics', () => {
    /**
     * Subscribes `on` to every channel of `topic`, also those offered later, as a client that
     * follows advertise does: a server may offer a topic again as a new channel.
     */
    const follow = (on: Connection, topic: string) => {
      const subscriptions = new Map<number, number>(); // subscription id by channel id
      const update = () => {
        for (const offered of on.channels().values()) {
          if (offered.topic !== topic || subscriptions.has(offered.id)) continue;
          subscriptions.set(offered.id, on.subscribe(offered.id));
        }
      };
      update();
      const timer = setInterval(update, 50);
      return {
        /** Whether a message with this text arrived on any of them since `since`. */
        received: (text: string, since: number) =>
          on.received
            .slice(since)
            .some(
              (message: Received) =>
                Array.from(subscriptions.values()).some((id) => isMessageData(id)(message)) &&
                readString((message as MessageData).data) === text
            ),
        stop: () => {
          clearInterval(timer);
          // Subscriptions to a channel the server removed have ended already.
          const offered = on.channels();
          subscriptions.forEach((id, channelId) => {
            if (offered.has(channelId)) on.unsubscribe(id);
          });
        },
      };
    };

    const waitFor = async (condition: () => boolean, timeoutMs: number) => {
      const deadline = Date.now() + timeoutMs;
      while (!condition() && Date.now() < deadline) await pause(100);
      return condition();
    };

    it('delivers a message whose transient-local publisher came after the subscription', async (t) => {
      if (skip) return t.skip(skip);
      const late = FIXTURE.lateLatched;
      const service = info.capabilities.includes('services')
        ? connection.services().get(late.service)
        : undefined;
      if (!service) return t.skip(`the server does not offer ${late.service}`);

      await connection.channel(late.topic, late.type);
      const first = follow(connection, late.topic);
      try {
        // The server subscribes while the topic has no publisher.
        await pause(1500);
        const since = connection.mark();
        connection.sendServiceCall(service.id, connection.id(), 'cdr', writeSetBoolRequest(true));
        assert.ok(
          await waitFor(() => first.received(late.text, since), 10000),
          `${late.topic} never delivered its message`
        );

        // And to a client that subscribes while the first one still is.
        const later = await Connection.open(SERVER_URL);
        const second = follow(later, late.topic);
        try {
          await later.channel(late.topic, late.type);
          assert.ok(
            await waitFor(() => second.received(late.text, 0), 10000),
            `${late.topic} did not deliver its message to a later subscriber`
          );
        } finally {
          second.stop();
          later.close();
        }
      } finally {
        first.stop();
      }
    });
  });

  describe('transforms', () => {
    const needsTransforms = () =>
      skip ||
      (info.capabilities.includes('miti.transforms') ? '' : 'no miti.transforms capability');

    it('jpeg sends miti.subscriptionFormat, then CompressedImages, until it is removed', async (t) => {
      const reason = needsTransforms();
      if (reason) return t.skip(reason);
      const channel = await connection.channel(FIXTURE.image.topic);
      const subscription = connection.subscribe(channel.id);
      try {
        const since = connection.mark();
        setOptions(subscription, { transform: { kind: 'jpeg', quality: 50 } });
        const jpeg = await nextFormat(subscription, since);
        assert.equal(jpeg.format.encoding, 'cdr');
        assert.equal(jpeg.format.schemaName, 'sensor_msgs/msg/CompressedImage');
        assert.equal(jpeg.format.schemaEncoding, 'ros2msg');
        assert.match(String(jpeg.format.schema), /^string format$/m);

        // Every message after the format is in it.
        const compressed = readCompressedImage(
          (await connection.messageData(subscription, { since: jpeg.index + 1 })).data
        );
        assert.equal(compressed.header.frameId, 'camera');
        assert.equal(compressed.format, 'jpeg');
        assert.deepEqual(Array.from(compressed.data.subarray(0, 2)), [0xff, 0xd8]);

        const removed = connection.mark();
        setOptions(subscription, { transform: null });
        const raw = await nextFormat(subscription, removed);
        assert.match(String(raw.format.schemaName), /^sensor_msgs\/(msg\/)?Image$/);
        const image = readImage(
          (await connection.messageData(subscription, { since: raw.index + 1 })).data
        );
        assert.equal(image.encoding, 'rgb8');
        assert.equal(image.width, FIXTURE.image.width);
      } finally {
        connection.unsubscribe(subscription);
      }
    });

    it('jpeg drops images it cannot encode, with one status warning', async (t) => {
      const reason = needsTransforms();
      if (reason) return t.skip(reason);
      const channel = await connection.channel(FIXTURE.depth.topic);
      const subscription = connection.subscribe(channel.id);
      try {
        const since = connection.mark();
        setOptions(subscription, { transform: { kind: 'jpeg' } });
        const { index } = await nextFormat(subscription, since);
        const warning = await nextStatus(since);
        assert.equal(warning.level, 1);
        assert.match(String(warning.message), new RegExp(FIXTURE.depth.encoding));
        // The fixture publishes an image every 200 ms.
        await pause(1000);
        const warnings = connection.received
          .slice(since)
          .filter((message) => message.op === 'status' && (message as JsonMessage).level === 1);
        assert.equal(warnings.length, 1);
        assert.equal(
          connection.received.slice(index + 1).filter(isMessageData(subscription)).length,
          0
        );
      } finally {
        connection.unsubscribe(subscription);
      }
    });

    it('reduce keeps x, y, z and the named fields, and at most maxPoints points', async (t) => {
      const reason = needsTransforms();
      if (reason) return t.skip(reason);
      const channel = await connection.channel(FIXTURE.points.topic);
      const subscription = connection.subscribe(channel.id);
      try {
        setOptions(subscription, { transform: { kind: 'reduce', fields: ['x'], maxPoints: 2 } });
        // The output has the input's type, so no format message tells where it starts: the first
        // message without intensity is reduced.
        const reduced = await connection.find(
          (message): message is MessageData =>
            isMessageData(subscription)(message) &&
            !readPointCloud2(message.data).fields.some((field) => field.name === 'intensity')
        );
        const cloud = readPointCloud2(reduced.data);
        assert.deepEqual(
          cloud.fields.map(({ name, offset }) => [name, offset]),
          [
            ['x', 0],
            ['y', 4],
            ['z', 8],
          ]
        );
        assert.equal(cloud.height, 1);
        assert.equal(cloud.width, 2);
        assert.equal(cloud.pointStep, 12);
        assert.equal(cloud.isDense, true);
        const view = new DataView(cloud.data.buffer, cloud.data.byteOffset, cloud.data.byteLength);
        // Evenly spaced: points 0 and 1 of 3.
        assert.equal(view.getFloat32(12 + 4, true), 2);
      } finally {
        connection.unsubscribe(subscription);
      }
    });

    it('answers a transform for another message type with a status error', async (t) => {
      const reason = needsTransforms();
      if (reason) return t.skip(reason);
      const channel = await connection.channel(FIXTURE.string.topic);
      const subscription = connection.subscribe(channel.id);
      try {
        const since = connection.mark();
        setOptions(subscription, { transform: { kind: 'jpeg' } });
        assert.equal((await nextStatus(since)).level, 2);
      } finally {
        connection.unsubscribe(subscription);
      }
    });

    it('answers invalid transform parameters with a status error', async (t) => {
      const reason = needsTransforms();
      if (reason) return t.skip(reason);
      const channel = await connection.channel(FIXTURE.image.topic);
      const subscription = connection.subscribe(channel.id);
      try {
        const since = connection.mark();
        setOptions(subscription, { transform: { kind: 'jpeg', quality: 0 } });
        assert.equal((await nextStatus(since)).level, 2);
        // Nothing changed: no format message, and raw images still arrive.
        await pause(500);
        const received = connection.received.slice(since);
        assert.equal(received.filter(isOp('miti.subscriptionFormat')).length, 0);
        assert.ok(received.some(isMessageData(subscription)), 'no images after the error');
      } finally {
        connection.unsubscribe(subscription);
      }
    });
  });
  describe('miti.getSchema', () => {
    const getSchema = async (type: string) => {
      const id = `schema-${connection.id()}`;
      const since = connection.mark();
      connection.send({ op: 'miti.getSchema', id, type });
      return connection.find(
        (message): message is JsonMessage =>
          message.op === 'miti.schema' && (message as JsonMessage).id === id,
        { since }
      );
    };

    it('returns the ros2msg definition of a type', async (t) => {
      if (skip) return t.skip(skip);
      if (!info.capabilities.includes('miti.schemas')) return t.skip('no miti.schemas capability');
      const reply = await getSchema(FIXTURE.image.type);
      assert.equal(reply.type, FIXTURE.image.type);
      assert.equal(reply.schemaName, FIXTURE.image.type);
      assert.equal(reply.schemaEncoding, 'ros2msg');
      assert.match(String(reply.schema), /^std_msgs\/Header header/m);
      assert.match(String(reply.schema), /^MSG: std_msgs\/Header$/m);
    });

    it('answers an unknown type with an error', async (t) => {
      if (skip) return t.skip(skip);
      if (!info.capabilities.includes('miti.schemas')) return t.skip('no miti.schemas capability');
      const reply = await getSchema('no_such_msgs/msg/Nothing');
      assert.equal(typeof reply.error, 'string');
      assert.equal(reply.schema, undefined);
    });
  });
  describe('miti.stats', () => {
    it('reports what each subscription sent and dropped, until unsubscribed', async (t) => {
      if (skip) return t.skip(skip);
      if (!info.capabilities.includes('miti.stats')) return t.skip('no miti.stats capability');
      const channel = await connection.channel(FIXTURE.string.topic);
      const subscription = connection.subscribe(channel.id);
      try {
        // 10 Hz at maxRate 2: most messages are dropped.
        setOptions(subscription, { maxRate: 2 });
        const since = connection.mark();
        connection.send({ op: 'miti.subscribeStats', intervalMs: 500 });
        await pause(2200);
        const reports = connection.received.slice(since).filter(isOp('miti.stats'));
        assert.ok(reports.length >= 3, `${reports.length} reports in 2.2 s every 500 ms`);
        for (const report of reports) {
          assert.ok(Math.abs(Number(report.timestamp) - Date.now()) < 60_000, 'Unix milliseconds');
          assert.equal(typeof report.bufferedBytes, 'number');
        }
        const ours = reports
          .slice(1)
          .map((report) =>
            (report.subscriptions as JsonMessage[]).find(
              (entry) => entry.subscriptionId === subscription
            )
          );
        const sum = (field: string) =>
          ours.reduce((total, entry) => total + Number(entry?.[field] ?? 0), 0);
        assert.ok(sum('sent') >= 1, `sent ${sum('sent')}`);
        assert.ok(sum('dropped') > sum('sent'), `sent ${sum('sent')}, dropped ${sum('dropped')}`);
        assert.ok(sum('bytesPerSecond') > 0);
        for (const entry of ours) assert.equal(typeof entry?.oldestQueuedMs, 'number');

        connection.send({ op: 'miti.unsubscribeStats' });
        await pause(300);
        const after = connection.mark();
        await pause(1200);
        assert.equal(connection.received.slice(after).filter(isOp('miti.stats')).length, 0);
      } finally {
        connection.send({ op: 'miti.unsubscribeStats' });
        connection.unsubscribe(subscription);
      }
    });

    it('answers an interval under 250 ms with a status error', async (t) => {
      if (skip) return t.skip(skip);
      if (!info.capabilities.includes('miti.stats')) return t.skip('no miti.stats capability');
      const since = connection.mark();
      connection.send({ op: 'miti.subscribeStats', intervalMs: 100 });
      assert.equal((await nextStatus(since)).level, 2);
    });
  });
});
