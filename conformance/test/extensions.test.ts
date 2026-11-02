/**
 * The extensions (docs/protocol.md, "Extensions"), for servers that select miti.bridge.v1.
 * Skipped against servers that do not, such as foxglove_bridge.
 */
import assert from 'node:assert/strict';
import { after, before, describe, it } from 'node:test';
import { FIXTURE, SERVER_URL, connectWhenReady } from '../src/fixture.ts';
import {
  Connection,
  MITI_SUBPROTOCOL,
  isMessageData,
  type JsonMessage,
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

    // Over a fast local connection nothing waits long enough to be replaced or reordered;
    // libmiti's scheduler tests check these, and phase 3 checks them over a slowed link.
    it.todo('latest delivery keeps only the newest waiting message');
    it.todo('high priority is sent before low');
  });

  it.todo('miti.subscriptionFormat precedes the first transformed message');
  it.todo('jpeg turns an rgb8 Image into a CompressedImage');
  it.todo('reduce keeps the named fields and at most maxPoints points');
  it.todo('miti.getSchema returns a ros2msg definition, or an error for an unknown type');
  it.todo('miti.stats reports what was sent and dropped');
});
