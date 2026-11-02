"""The fixture graph the conformance suite runs against: one node with a known topic of each
kind MITI draws, an echo for client publishing, a service, a parameter and a mesh.

conformance/src/fixture.ts holds the same names and values; change both together.
"""

import struct
import time

import rclpy
from builtin_interfaces.msg import Time
from geometry_msgs.msg import TransformStamped
from rclpy.executors import ExternalShutdownException
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile
from rosgraph_msgs.msg import Clock
from sensor_msgs.msg import Image, PointCloud2, PointField
from std_msgs.msg import String
from std_srvs.srv import SetBool
from tf2_msgs.msg import TFMessage

ROBOT_DESCRIPTION = """<?xml version="1.0"?>
<robot name="conformance">
  <link name="base_link">
    <visual>
      <geometry><mesh filename="package://miti_conformance_fixture/meshes/cube.stl"/></geometry>
    </visual>
  </link>
</robot>
"""


class Fixture(Node):
    def __init__(self):
        super().__init__("conformance_fixture")
        self.declare_parameter("robot_description", ROBOT_DESCRIPTION)
        self.ticks = 0

        self.string_pub = self.create_publisher(String, "/conformance/string", 10)
        self.image_pub = self.create_publisher(Image, "/conformance/image", 10)
        self.points_pub = self.create_publisher(PointCloud2, "/conformance/points", 10)
        self.tf_pub = self.create_publisher(TFMessage, "/tf", 10)
        self.clock_pub = self.create_publisher(Clock, "/clock", 10)
        latched = QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL)
        self.tf_static_pub = self.create_publisher(TFMessage, "/tf_static", latched)

        self.echo_pub = self.create_publisher(String, "/conformance/echo", 10)
        self.create_subscription(String, "/conformance/in", self.echo_pub.publish, 10)
        self.create_service(SetBool, "/conformance/set_bool", self.set_bool)

        self.tf_static_pub.publish(TFMessage(transforms=[self.transform("map", "odom", 0.0)]))
        self.create_timer(0.1, self.tick)

    def set_bool(self, request, response):
        response.success = True
        response.message = f"set {request.data}"
        return response

    def stamp(self):
        now = time.time_ns()
        return Time(sec=now // 1_000_000_000, nanosec=now % 1_000_000_000)

    def transform(self, parent, child, x):
        transform = TransformStamped()
        transform.header.stamp = self.stamp()
        transform.header.frame_id = parent
        transform.child_frame_id = child
        transform.transform.translation.x = x
        transform.transform.rotation.w = 1.0
        return transform

    def tick(self):
        self.ticks += 1
        stamp = self.stamp()
        self.string_pub.publish(String(data=f"tick {self.ticks}"))
        self.tf_pub.publish(TFMessage(transforms=[self.transform("odom", "base_link", 1.5)]))
        self.clock_pub.publish(Clock(clock=stamp))
        if self.ticks % 2 == 0:
            self.publish_image(stamp)
            self.publish_points(stamp)

    def publish_image(self, stamp):
        image = Image()
        image.header.stamp = stamp
        image.header.frame_id = "camera"
        image.height, image.width = 2, 4
        image.encoding = "rgb8"
        image.step = image.width * 3
        image.data = bytes(range(image.height * image.step))
        self.image_pub.publish(image)

    def publish_points(self, stamp):
        cloud = PointCloud2()
        cloud.header.stamp = stamp
        cloud.header.frame_id = "base_link"
        cloud.height, cloud.width = 1, 3
        cloud.fields = [
            PointField(name=name, offset=4 * i, datatype=PointField.FLOAT32, count=1)
            for i, name in enumerate(["x", "y", "z"])
        ]
        cloud.point_step = 12
        cloud.row_step = cloud.point_step * cloud.width
        cloud.data = b"".join(struct.pack("<3f", i, 2.0 * i, 3.0 * i) for i in range(3))
        cloud.is_dense = True
        self.points_pub.publish(cloud)


def main():
    rclpy.init()
    node = Fixture()
    try:
        rclpy.spin(node)
    except (KeyboardInterrupt, ExternalShutdownException):
        pass
    finally:
        node.destroy_node()
        rclpy.try_shutdown()


if __name__ == "__main__":
    main()
