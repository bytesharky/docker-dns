#!/bin/sh

DOCKER_NET=docker-net
GATEWAY_IP=172.18.0.1
NETWORK_ADDRESS=172.18.0.0/24
RESOLV=/etc/resolv.conf
CONTAINER_NAME=docker-dns
IMAGE_NAME=docker-dns:alpine

# ========================
# 启动容器函数
# ========================
start_container() {
    name="$1"
    echo "启动容器 $name..."
    docker run -d \
        --network "$DOCKER_NET" \
        --name "$name" \
        -p 53:53/udp \
        --restart unless-stopped \
        "$IMAGE_NAME"
}

# ========================
# 检查 Docker 网络
# ========================
if ! docker network inspect "$DOCKER_NET" >/dev/null 2>&1; then
    echo "Docker 网络 $DOCKER_NET 不存在，正在创建..."
    if docker network create "$DOCKER_NET" --subnet="$NETWORK_ADDRESS" --gateway="$GATEWAY_IP"; then
        echo "Docker 网络 $DOCKER_NET 创建成功"
    else
        echo "Docker 网络 $DOCKER_NET 创建失败"
        exit 1
    fi
else
    echo "Docker 网络 $DOCKER_NET 已存在"
fi

# ========================
# 构建镜像（可选）
# ========================
# echo "构建镜像..."
# git clone https://github.com/bytesharky/docker-dns
# cd docker-dns
# docker build -t $IMAGE_NAME .
# echo "镜像构建完成"

# ========================
# 拉取镜像
# ========================
# echo "拉取镜像..."
# docker pull ccr.ccs.tencentyun.com/sharky/docker-dns:alpine
# docker tag ccr.ccs.tencentyun.com/sharky/docker-dns:alpine $IMAGE_NAME
# echo "镜像拉取完成"

echo "请选择操作："
echo "1) 构建镜像"
echo "2) 拉取镜像"
read -r -p "请输入选项 [1-2]: " choice

case "$choice" in
    1)
        echo "构建镜像..."
        docker build -t "$IMAGE_NAME" .
        echo "镜像构建完成"
        ;;
    2)
        echo "拉取镜像..."
        docker pull ccr.ccs.tencentyun.com/sharky/docker-dns:alpine
        docker tag ccr.ccs.tencentyun.com/sharky/docker-dns:alpine "$IMAGE_NAME"
        echo "镜像拉取完成"
        ;;
    *)
        echo "无效选项，退出"
        exit 1
        ;;
esac

# ========================
# 修改 resolv.conf：保证第一DNS是127.0.0.1
# ========================
echo "设置 DNS 服务器为 127.0.0.1"

# 如果是软链接，解析实际文件
TARGET=$(readlink -f "$RESOLV")
[ -z "$TARGET" ] && TARGET="$RESOLV"

first_dns=$(grep '^nameserver' "$TARGET" | head -n1 | awk '{print $2}')
if [ "$first_dns" = "127.0.0.1" ]; then
    echo "DNS 已经正确，无需修改"
else
    TMPFILE=$(mktemp)

    # 拿到 nameserver 列表，排除 127.0.0.1
    orig_dns=$(grep '^nameserver' "$TARGET" | awk '{print $2}' | grep -v '^127\.0\.0\.1$')

    {
      echo "nameserver 127.0.0.1"
      for dns in $orig_dns; do
          echo "nameserver $dns"
      done
    } > "$TMPFILE"

    # 追加非 nameserver 配置
    grep -v '^nameserver' "$TARGET" >> "$TMPFILE"

    # 覆盖原文件
    cat "$TMPFILE" > "$TARGET"
    rm -f "$TMPFILE"
    echo "DNS 服务器设置完成"
fi

# ========================
# 启动/处理容器
# ========================
if docker ps -a --format '{{.Names}}' | grep -q "^$CONTAINER_NAME$"; then
    echo "容器 $CONTAINER_NAME 已存在，请选择操作："
    echo "1) 删除并重建"
    echo "2) 重命名并启动新容器"
    echo "3) 退出"
    read -r -p "请输入选项 [1-3]: " choice

    case "$choice" in
        1)
            echo "删除旧容器..."
            docker rm -f "$CONTAINER_NAME"
            start_container "$CONTAINER_NAME"
            ;;
        2)
            read -r -p "请输入新容器名称: " newname
            if [ -z "$newname" ]; then
                echo "名称不能为空，退出"
                exit 1
            fi
            start_container "$newname"
            ;;
        3)
            echo "已退出"
            exit 0
            ;;
        *)
            echo "无效选项，退出"
            exit 1
            ;;
    esac
else
    start_container "$CONTAINER_NAME"
fi

echo "容器已启动"
