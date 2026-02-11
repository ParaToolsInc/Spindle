#!/bin/bash
set -euxo pipefail

docker compose --progress=plain build
docker compose up -d --wait
