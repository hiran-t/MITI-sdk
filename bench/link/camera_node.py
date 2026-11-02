"""A robot for the link benchmark: a raw 1280x720 rgb8 camera at 30 Hz and TF at 50 Hz, stamped
with the wall clock so the client can tell how late each message arrives."""

import array
import time

import numpy as np
import rclpy
from builtin_interfaces.msg import Time
from geometry_msgs.msg import TransformStamped
from rclpy.node import Node
from sensor_msgs.msg import Image
from tf2_msgs.msg import TFMessage

WIDTH, HEIGHT = 1280, 720


def frames(count=30):
    """Moving gradients and edges with a little sensor noise: not as easy to compress as a flat
    synthetic image, nor as hard as pure noise."""
    rng = np.random.default_rng(0)
    y, x = np.mgrid[0:HEIGHT, 0:WIDTH]
    out = []
    for i in range(count):
        red = 128 + 100 * np.sin((x + 8 * i) / 60.0)
        green = 128 + 100 * np.sin(y / 45.0 + 0.2 * i)
        blue = ((x // 80 + y // 80 + i) % 2) * 160 + 40
        image = np.stack([red, green, blue], axis=-1) + rng.normal(0, 4, (HEIGHT, WIDTH, 3))
        out.append(array.array("B", np.clip(image, 0, 255).astype(np.uint8).tobytes()))
    return out


def now():
    ns = time.time_ns()
    return Time(sec=ns // 1_000_000_000, nanosec=ns % 1_000_000_000)


class Robot(Node):
    def __init__(self):
        super().__init__("link_bench_robot")
        self.frames = frames()
        self.tick = 0
        self.camera = self.create_publisher(Image, "/camera/image_raw", 1)
        self.tf = self.create_publisher(TFMessage, "/tf", 10)
        self.create_timer(1 / 30, self.publish_image)
        self.create_timer(1 / 50, self.publish_tf)

    def publish_image(self):
        image = Image()
        image.header.stamp = now()
        image.header.frame_id = "camera"
        image.height, image.width = HEIGHT, WIDTH
        image.encoding = "rgb8"
        image.step = WIDTH * 3
        image.data = self.frames[self.tick % len(self.frames)]
        self.tick += 1
        self.camera.publish(image)

    def publish_tf(self):
        transform = TransformStamped()
        transform.header.stamp = now()
        transform.header.frame_id = "odom"
        transform.child_frame_id = "base_link"
        transform.transform.rotation.w = 1.0
        self.tf.publish(TFMessage(transforms=[transform]))


def main():
    rclpy.init()
    rclpy.spin(Robot())


if __name__ == "__main__":
    main()
