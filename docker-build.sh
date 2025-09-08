#!/bin/sh
set -e

if [ -n "$1" ]; then
    TAG=$1
else
    echo "标签不能为空"
    exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

cd "$SCRIPT_DIR" || exit

sed -i -E 's/^DEFAULT_IMAGE_NAME="([^"]+)"/DEFAULT_IMAGE_NAME="docker-dns:'${TAG}'"/' ./docker-dns.sh

rm -f Dockerfile

cp Dockerfile-${TAG} Dockerfile

# chmod +x ./docker-dns.sh && ./docker-dns.sh

docker build $1 -t docker-dns:${TAG} .
