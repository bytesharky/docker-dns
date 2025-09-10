#!/bin/sh
set -e

# 语言选择功能
echo "请选择语言 / Please select language:"
echo "1) 中文"
echo "2) English"

# 根据选择设置LANGUAGE环境变量
while true; do
    read -p "> (1/2): " lang_choice
    case $lang_choice in
        1)
            export LANGUAGE="zh"
            break
            ;;
        2)
            export LANGUAGE="en"
            break
            ;;
        *)
            echo "无效选择 / Invalid choice"
            ;;
    esac
done

# 加载语言文件
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
LANG_DIR="$SCRIPT_DIR/langs"

# 检查语言文件是否存在，不存在则使用英文（警告信息用英文）
if [ -f "$LANG_DIR/$LANGUAGE.sh" ]; then
    . "$LANG_DIR/$LANGUAGE.sh"
else
    echo "警告：未找到语言文件 $LANG_DIR/$LANGUAGE.sh，使用英文默认值"
    echo "Warning: Language file $LANG_DIR/$LANGUAGE.sh not found, using English defaults"
    . "$LANG_DIR/en.sh"
fi

IMAGE="docker-dns"

if [ -n "$1" ]; then
    TAG=$1
else
    echo "$LANG_ERROR_TAG_EMPTY"
    exit 1
fi


cd "$SCRIPT_DIR" || exit

echo "$LANG_BUILDING_IMAGE"

rm -f Dockerfile

cp Dockerfile-${TAG} Dockerfile

docker build $2 -t ${IMAGE}:${TAG} .

echo "$LANG_BUILD_COMPLETE"

echo "$LANG_INITIALIZE_CONTAINER"

sed -i -E 's/^DEFAULT_IMAGE_NAME="([^"]+)"/DEFAULT_IMAGE_NAME="'${IMAGE}:${TAG}'"/' ./docker-run.sh

chmod +x ./docker-run.sh

./docker-run.sh

echo "$LANG_COMPLETED"
