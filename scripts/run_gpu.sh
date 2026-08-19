#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
source .venv/bin/activate

CUDA_LIB_PATH="$(python - <<'PY'
import os

try:
    import nvidia.cublas.lib
    import nvidia.cudnn.lib
except Exception:
    raise SystemExit(0)

print(
    os.path.dirname(nvidia.cublas.lib.__file__)
    + ":"
    + os.path.dirname(nvidia.cudnn.lib.__file__)
)
PY
)"

if [ -n "${CUDA_LIB_PATH}" ]; then
  export LD_LIBRARY_PATH="${CUDA_LIB_PATH}:${LD_LIBRARY_PATH:-}"
fi

python -m voice_memo.gpu_server
