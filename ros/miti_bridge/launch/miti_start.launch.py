"""Starts miti_bridge, which serves this ROS 2 graph to MITI.

    ros2 launch miti_bridge miti_start
    ros2 launch miti_bridge miti_start port:=8765 use_sim_time:=true "topic_whitelist:=['/tf.*']"

Each argument sets the node parameter of the same name; see the package's README. It is installed
as `miti_start` and as `miti_start.launch.py`, so either name works with `ros2 launch`.
"""

import yaml
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node

# (name, default, description); a default in [] marks a list.
ARGUMENTS = [
    ('port', '8765', 'WebSocket port'),
    ('address', '0.0.0.0', 'Address to listen on'),
    ('topic_whitelist', "['.*']", 'Regular expressions of the topics to offer'),
    ('service_whitelist', "['.*']", 'Regular expressions of the services to offer'),
    ('param_whitelist', "['.*']", 'Regular expressions of the parameters clients may read'),
    ('client_topic_whitelist', "['.*']", 'Regular expressions of the topics clients may publish on'),
    ('capabilities', "['clientPublish', 'parameters', 'services', 'assets']", 'What clients may do'),
    ('max_qos_depth', '10', 'The largest queue a subscription keeps'),
    ('asset_roots', '[]', 'Folders file:// assets may come from'),
    ('use_sim_time', 'false', "Use and send the robot's /clock"),
]


def bridge(context):
    parameters = {}
    for name, default, _ in ARGUMENTS:
        value = yaml.safe_load(LaunchConfiguration(name).perform(context))
        if default.startswith('['):
            if isinstance(value, str):
                value = [value]  # topic_whitelist:=/tf.* means one pattern
            if not value:
                continue  # ROS cannot tell an empty list's type; the node's default is empty
        parameters[name] = value
    return [
        Node(
            package='miti_bridge',
            executable='miti_bridge',
            name='miti_bridge',
            output='screen',
            parameters=[parameters],
        )
    ]


def generate_launch_description():
    return LaunchDescription(
        [DeclareLaunchArgument(name, default_value=default, description=description)
         for name, default, description in ARGUMENTS]
        + [OpaqueFunction(function=bridge)]
    )
