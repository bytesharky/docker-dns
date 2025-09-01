#!/bin/sh

# 定义默认值
DEFAULT_DOCKER_NET="docker-net"
DEFAULT_GATEWAY_IP="172.18.0.1"
DEFAULT_NETWORK_ADDRESS="172.18.0.0/24"
DEFAULT_RESOLV="/etc/resolv.conf"
DEFAULT_CONTAINER_NAME="docker-dns"
DEFAULT_IMAGE_NAME="docker-dns:alpine"

# 提示用户输入并处理默认值
read -p "请输入Docker网络名称 (默认: $DEFAULT_DOCKER_NET): " DOCKER_NET
DOCKER_NET=${DOCKER_NET:-$DEFAULT_DOCKER_NET}

read -p "请输入网关IP地址 (默认: $DEFAULT_GATEWAY_IP): " GATEWAY_IP
GATEWAY_IP=${GATEWAY_IP:-$DEFAULT_GATEWAY_IP}

read -p "请输入网络地址 (默认: $DEFAULT_NETWORK_ADDRESS): " NETWORK_ADDRESS
NETWORK_ADDRESS=${NETWORK_ADDRESS:-$DEFAULT_NETWORK_ADDRESS}

read -p "请输入resolv路径 (默认: $DEFAULT_RESOLV): " RESOLV
RESOLV=${RESOLV:-$DEFAULT_RESOLV}

read -p "请输入容器名称 (默认: $DEFAULT_CONTAINER_NAME): " CONTAINER_NAME
CONTAINER_NAME=${CONTAINER_NAME:-$DEFAULT_CONTAINER_NAME}

read -p "请输入镜像名称 (默认: $DEFAULT_IMAGE_NAME): " IMAGE_NAME
IMAGE_NAME=${IMAGE_NAME:-$DEFAULT_IMAGE_NAME}

# 显示最终配置
echo "----------------------------------------"
echo "已配置的参数："
echo "Docker网络名称: $DOCKER_NET"
echo "网关IP地址: $GATEWAY_IP"
echo "网络地址: $NETWORK_ADDRESS"
echo "resolv路径: $RESOLV"
echo "容器名称: $CONTAINER_NAME"
echo "镜像名称: $IMAGE_NAME"
echo "----------------------------------------"

# 确认继续
while true; do
    read -p "是否确认继续部署? (Y/N) " yn
    yn=${yn}
    case $yn in
        [Yy]* ) break;;
        [Nn]* ) exit;;
        * ) ;;
    esac
done

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

while true; do
    read -r -p "请输入选项 [1-2]: " choice
    case "$choice" in
        1)
            echo "构建镜像..."
            docker build -t "$IMAGE_NAME" .
            echo "镜像构建完成"
            break
            ;;
        2)
            echo "拉取镜像..."
            docker pull ccr.ccs.tencentyun.com/sharky/docker-dns:alpine
            docker tag ccr.ccs.tencentyun.com/sharky/docker-dns:alpine "$IMAGE_NAME"
            echo "镜像拉取完成"
            break
            ;;
        *) ;;
    esac
done

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
    echo "1) 删除并重建容器"
    echo "2) 使用新的容器名称"
    echo "3) 退出"
    
    while true; do
        read -r -p "请输入选项 [1-3]: " choice
        case "$choice" in
            1)
                echo "删除旧容器..."
                docker rm -f "$CONTAINER_NAME"
                start_container "$CONTAINER_NAME"
                break
                ;;
            2)
                read -r -p "请输入新容器名称: " newname
                if [ -z "$newname" ]; then
                    echo "名称不能为空，退出"
                    exit 1
                fi
                start_container "$newname"
                break
                ;;
            3)
                echo "已退出"
                exit 0
                ;;
            *) ;;
        esac
    done
else
    start_container "$CONTAINER_NAME"
fi

echo "容器已启动"
