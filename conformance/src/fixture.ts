import { Connection, type TimeMessage } from './protocol.ts';

/** What the fixture graph (fixture/fixture_node.py) offers; change both together. */
export const FIXTURE = {
  string: { topic: '/conformance/string', type: 'std_msgs/msg/String' },
  image: { topic: '/conformance/image', type: 'sensor_msgs/msg/Image', width: 4, height: 2 },
  /** An encoding the jpeg transform cannot encode. */
  depth: { topic: '/conformance/depth', type: 'sensor_msgs/msg/Image', encoding: '16UC1' },
  /** Fields x, y, z and intensity; point i is (i, 2i, 3i), with intensity 10i. */
  points: { topic: '/conformance/points', type: 'sensor_msgs/msg/PointCloud2', width: 3 },
  tf: { topic: '/tf', type: 'tf2_msgs/msg/TFMessage', parent: 'odom', child: 'base_link', x: 1.5 },
  /** Published once, transient-local. */
  tfStatic: { topic: '/tf_static', type: 'tf2_msgs/msg/TFMessage', parent: 'map', child: 'odom' },
  /** Client publishing: what is published on `in` comes back on `echo`. */
  echo: { in: '/conformance/in', out: '/conformance/echo', type: 'std_msgs/msg/String' },
  service: { name: '/conformance/set_bool', type: 'std_srvs/srv/SetBool' },
  parameter: '/conformance_fixture.robot_description',
  mesh: 'package://miti_conformance_fixture/meshes/cube.stl',
  /** Outside every asset root a server may have. */
  forbiddenAsset: 'file:///etc/hosts',
} as const;

/** The address of the server under test. */
export const SERVER_URL = process.env.MITI_SERVER_URL ?? 'ws://127.0.0.1:8765';

/** How long a server just started may take to listen and to discover the fixture graph. */
const STARTUP_MS = 30000;

/**
 * A connection to the server under test once it is ready: it accepts connections and offers the
 * fixture's topics (and service, if it has the capability) and, on simulation time, has a clock.
 * Servers and the fixture start together, and DDS discovery takes seconds, longer on some distros
 * than others.
 */
export async function connectWhenReady(): Promise<Connection> {
  const deadline = Date.now() + STARTUP_MS;
  let connection: Connection | undefined;
  while (!connection) {
    try {
      connection = await Connection.open(SERVER_URL);
    } catch (error) {
      if (Date.now() > deadline) throw error;
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
  }
  const left = () => Math.max(deadline - Date.now(), 1000);
  try {
    const info = await connection.serverInfo();
    await connection.channel(FIXTURE.string.topic, undefined, left());
    await connection.channel(FIXTURE.echo.out, undefined, left());
    if (info.capabilities.includes('services'))
      await connection.service(FIXTURE.service.name, left());
    // On simulation time a server has no time until its first clock message, and stamps what it
    // receives before then with zero.
    if (info.capabilities.includes('time'))
      await connection.find((message): message is TimeMessage => message.op === 'time', {
        timeoutMs: left(),
      });
  } catch (error) {
    // An open connection would keep the test run from ending.
    connection.close();
    throw error;
  }
  return connection;
}
