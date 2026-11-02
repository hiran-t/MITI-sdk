#!/usr/bin/env bash
# Starts the fixture graph. conformance/fixture is an install prefix of its own (it holds the
# miti_conformance_fixture package's share directory), so package:// URIs to its mesh resolve
# for any server started from this shell or one that inherits AMENT_PREFIX_PATH.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export AMENT_PREFIX_PATH="$here${AMENT_PREFIX_PATH:+:$AMENT_PREFIX_PATH}"
exec python3 "$here/fixture_node.py"
