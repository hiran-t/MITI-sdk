/**
 * The extensions (docs/protocol.md, "Extensions"), for servers that select miti.bridge.v1.
 * Skipped against servers that do not, such as foxglove_bridge.
 */
import assert from 'node:assert/strict';
import { after, before, describe, it } from 'node:test';
import { SERVER_URL, connectWhenReady } from '../src/fixture.ts';
import { Connection, MITI_SUBPROTOCOL } from '../src/protocol.ts';

describe('extensions', () => {
  let connection: Connection;
  let skip = '';

  before(async () => {
    connection = await connectWhenReady();
    if (connection.protocol !== MITI_SUBPROTOCOL)
      skip = `the server selected ${connection.protocol}, not ${MITI_SUBPROTOCOL}`;
  });

  after(() => connection?.close());

  it('refuses a handshake that does not offer miti.bridge.v1', async (t) => {
    if (skip) return t.skip(skip);
    await assert.rejects(Connection.open(SERVER_URL, ['foxglove.websocket.v1']));
  });

  it.todo('miti.subscriptionOptions: maxRate caps the messages sent');
  it.todo('miti.subscriptionOptions: latest delivery keeps only the newest waiting message');
  it.todo('miti.subscriptionOptions: high priority is sent before low');
  it.todo('miti.subscriptionFormat precedes the first transformed message');
  it.todo('jpeg turns an rgb8 Image into a CompressedImage');
  it.todo('reduce keeps the named fields and at most maxPoints points');
  it.todo('miti.getSchema returns a ros2msg definition, or an error for an unknown type');
  it.todo('miti.stats reports what was sent and dropped');
});
