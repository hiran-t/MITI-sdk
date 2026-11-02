# Generic service client prototype

Phase 0 of [the design](../../docs/design.md) names one risk to settle first: calling a ROS 2 service whose type is known only at run time, with a CDR request, on Humble. miti_bridge needs this to serve MITI's service calls.

**Settled.** `generic_service_call` calls the fixture's `std_srvs/srv/SetBool` service with a CDR request and prints the CDR response. It works on Humble and on Jazzy, from one source file, with no warnings on either.

```bash
pixi run -e humble service-prototype
```

```bash
pixi run -e jazzy service-prototype
```

Each command builds the prototype, starts the conformance fixture on an isolated domain, and checks that the response is `success=true, message "set True"`. CI runs the same in `ros:humble-ros-base` and `ros:jazzy-ros-base`.

## What Humble lacks, and how this gets around it

| Needed                            | Humble                             | Jazzy                                                     | Used here, on both                                                                                                                        |
| --------------------------------- | ---------------------------------- | --------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| A generic client                  | No `rclcpp::GenericClient`         | `rclcpp::GenericClient`                                   | `rcl_client_init`, `rcl_send_request`, `rcl_take_response_with_info`                                                                      |
| The service's type support        | No helper                          | `rclcpp::get_service_typesupport_handle`                  | The exported symbol `rosidl_typesupport_cpp__get_service_type_support_handle__<pkg>__srv__<Type>`, from `rclcpp::get_typesupport_library` |
| Request and response type support | `rclcpp::get_typesupport_handle`   | The same, deprecated for `get_message_typesupport_handle` | The exported symbols, the same way. This avoids the deprecation                                                                           |
| Memory for a request or response  | None                               | None                                                      | The introspection type support's `size_of_`, `init_function` and `fini_function`                                                          |
| CDR ↔ message                     | `rmw_deserialize`, `rmw_serialize` | The same                                                  | The same                                                                                                                                  |

## What miti_bridge does differently

- **Waiting for responses.** The prototype polls `rcl_take_response_with_info` until a response arrives. miti_bridge adds the client to its executor's wait set, so responses arrive as events.
- **Type support.** Libraries and handles are looked up once per service type and kept.
- **Failures.** A failed call becomes `serviceCallFailure`: a missing service, an unknown type, a request that does not deserialize, or a timeout. The prototype prints these to stderr instead.
