#!/usr/bin/env bash
set -euo pipefail
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
bash "$script_dir/cli_integration_core.sh" "$@"
bash "$script_dir/video_cli_export_all.sh" "${1:?codec binary path required}"
