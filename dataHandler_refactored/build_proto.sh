#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 -m grpc_tools.protoc \
  -I "${ROOT_DIR}/grpc_protos/java_protos" \
  --python_out="${ROOT_DIR}/grpc_protos/java_protos_out" \
  --grpc_python_out="${ROOT_DIR}/grpc_protos/java_protos_out" \
  "${ROOT_DIR}"/grpc_protos/java_protos/*.proto