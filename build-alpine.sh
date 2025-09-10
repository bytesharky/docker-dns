#!/bin/sh

TAG="alpine"

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

cd "$SCRIPT_DIR" || exit

chmod +x ./docker-build.sh

./docker-build.sh $TAG $1
