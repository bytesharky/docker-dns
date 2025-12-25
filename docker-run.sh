#!/bin/sh

# ========================
# 国际化设置
# ========================

# 默认语言
DEFAULT_LANG="en"

# 从环境变量获取语言设置，默认为英文
LANGUAGE="${LANGUAGE:-$DEFAULT_LANG}"

# 语言文件目录
LANG_DIR="./langs"

# 加载对应的语言文件
load_language() {
    local lang_file="$LANG_DIR/${1}.sh"
    
    if [ -f "$lang_file" ]; then
        # shellcheck source=/dev/null
        . "$lang_file"
        echo "Loaded language file: $lang_file" >&2
    else
        echo "Language file $lang_file not found, using default (en)" >&2
        # shellcheck source=/dev/null
        . "$LANG_DIR/en.sh"
    fi
}

# 创建语言目录（如果不存在）
mkdir -p "$LANG_DIR"

# 加载语言文件
load_language "$LANGUAGE"

# ========================
# 默认配置
# ========================
DEFAULT_DOCKER_NET="docker-net"
DEFAULT_GATEWAY_IP="172.18.0.1"
DEFAULT_NETWORK_ADDRESS="172.18.0.0/24"
DEFAULT_RESOLV="/etc/resolv.conf"
DEFAULT_CONTAINER_NAME="docker-dns"
DEFAULT_IMAGE_NAME="docker-dns:static"
DEFAULT_TZ="Asia/Shanghai"
POSIX_TZ=""
LOG_LEVEL="INFO"
GATEWAY_NAME="gateway"

# ========================
# 容器启动函数
# ========================
start_container() {
    name="$1"
    echo "$LANG_STARTING_CONTAINER ${1}..."
    docker run -d \
        -e "TZ=$POSIX_TZ" \
        -e "LOG_LEVEL=$LOG_LEVEL" \
        -e "GATEWAY_NAME=$GATEWAY_NAME" \
        -e "CONTAINER_NAME=$CONTAINER_NAME" \
        --network "$DOCKER_NET" \
        --name "$name" \
        -p 53:53/udp \
        --restart unless-stopped \
        "$LOCAL_IMAGE_NAME"
}

# ========================
# 获取主机系统时区名称
# ========================
get_system_timezone() {
    if command -v timedatectl >/dev/null 2>&1; then
        timedatectl show -p Timezone --value
    elif [ -f /etc/timezone ]; then
        cat /etc/timezone
    elif [ -L /etc/localtime ]; then
        readlink /etc/localtime | sed 's|/usr/share/zoneinfo/||'
    else
        echo "$DEFAULT_TZ"
    fi
}

# ========================
# 转换为POSIX标准TZ格式
# ========================
to_posix_tz() {
    TZ_NAME="$1"

    if echo "$TZ_NAME" | grep -Eq '^[Uu][Tt][Cc][+-][0-9]{1,2}(:[0-9]{1,2})?$'; then
        # 解析UTC±N或UTC±N:MM格式
        SIGN=$(echo "$TZ_NAME" | grep -oE '[+-]' | head -n1)
        HOUR=$(echo "$TZ_NAME" | grep -oE '[0-9]{1,2}' | head -n1)
        MIN=$(echo "$TZ_NAME" | grep -oE ':[0-9]{1,2}' | cut -c2-)
    else
        # tzdata名称格式
        OFFSET=$(TZ="$TZ_NAME" date +%z)
        if [ "$OFFSET" = "+0000" ] && [ "$TZ_NAME" != "UTC" ]; then
            echo "$LANG_WARN_UNKNOWN_TZ $TZ_NAME, $LANG_FALLBACK_UTC" >&2
            SIGN="+"
            HOUR="00"
            MIN="00"
        else
            SIGN=$(echo "$OFFSET" | cut -c1)
            HOUR=$(echo "$OFFSET" | cut -c2-3)
            MIN=$(echo "$OFFSET" | cut -c4-5)
        fi
    fi
    
    SIGN=$(echo "$SIGN" | tr '+-' '-+')
    [ -z "$SIGN" ] && SIGN="+"
    [ -z "$HOUR" ] && HOUR="00"
    [ -z "$MIN" ] && MIN="00"

    if [ "$HOUR" = "00" ] && [ "$MIN" = "00" ]; then
        echo "UTC"
        return
    fi
    echo "UTC$SIGN$HOUR:$MIN"
}

# 提示用户输入并处理默认值
read -p "$LANG_ENTER_DOCKER_NET ($LANG_DEFAULT: $DEFAULT_DOCKER_NET): " DOCKER_NET
DOCKER_NET=${DOCKER_NET:-$DEFAULT_DOCKER_NET}

read -p "$LANG_ENTER_GATEWAY_IP ($LANG_DEFAULT: $DEFAULT_GATEWAY_IP): " GATEWAY_IP
GATEWAY_IP=${GATEWAY_IP:-$DEFAULT_GATEWAY_IP}

read -p "$LANG_ENTER_NETWORK_ADDR ($LANG_DEFAULT: $DEFAULT_NETWORK_ADDRESS): " NETWORK_ADDRESS
NETWORK_ADDRESS=${NETWORK_ADDRESS:-$DEFAULT_NETWORK_ADDRESS}

read -p "$LANG_ENTER_RESOLV_PATH ($LANG_DEFAULT: $DEFAULT_RESOLV): " RESOLV
RESOLV=${RESOLV:-$DEFAULT_RESOLV}

read -p "$LANG_ENTER_CONTAINER_NAME ($LANG_DEFAULT: $DEFAULT_CONTAINER_NAME): " CONTAINER_NAME
CONTAINER_NAME=${CONTAINER_NAME:-$DEFAULT_CONTAINER_NAME}

read -p "$LANG_ENTER_IMAGE_NAME ($LANG_DEFAULT: $DEFAULT_IMAGE_NAME): " IMAGE_NAME
LOCAL_IMAGE_NAME=${LOCAL_IMAGE_NAME:-$DEFAULT_IMAGE_NAME}

DEFAULT_TZ=$(get_system_timezone)

read -p "$LANG_ENTER_TIMEZONE ($LANG_DEFAULT: $DEFAULT_TZ): " TZ
TZ=${TZ:-$DEFAULT_TZ}
POSIX_TZ=$(to_posix_tz "$TZ")

# 显示最终配置
echo "----------------------------------------"
echo "$LANG_CONFIGURED_PARAMS:"
echo "$LANG_DOCKER_NET: $DOCKER_NET"
echo "$LANG_GATEWAY_IP: $GATEWAY_IP"
echo "$LANG_NETWORK_ADDR: $NETWORK_ADDRESS"
echo "$LANG_RESOLV_PATH: $RESOLV"
echo "$LANG_CONTAINER_NAME: $CONTAINER_NAME"
echo "$LANG_IMAGE_NAME: $LOCAL_IMAGE_NAME"
echo "$LANG_STD_TIMEZONE: $TZ"
echo "$LANG_POSIX_TIMEZONE: $POSIX_TZ"
echo "----------------------------------------"

# 确认继续
while true; do
    read -p "$LANG_CONFIRM_DEPLOY? (Y/N) " yn
    case $yn in
        [Yy]* ) break;;
        [Nn]* ) exit;;
        * ) ;;
    esac
done

# ========================
# 检查Docker网络
# ========================
if ! docker network inspect "$DOCKER_NET" >/dev/null 2>&1; then
    echo "$LANG_NETWORK_NOT_EXIST $DOCKER_NET, $LANG_CREATING..."
    if docker network create "$DOCKER_NET" --subnet="$NETWORK_ADDRESS" --gateway="$GATEWAY_IP"; then
        echo "$LANG_NETWORK_CREATED $DOCKER_NET"
    else
        echo "$LANG_NETWORK_CREATE_FAILED $DOCKER_NET"
        exit 1
    fi
else
    echo "$LANG_NETWORK_EXISTS $DOCKER_NET"
fi

# ========================
# 修改resolv.conf
# 确保第一个DNS是127.0.0.1
# ========================
echo "$LANG_SETTING_DNS 127.0.0.1"

# 如果是符号链接，解析实际文件
TARGET=$(readlink -f "$RESOLV")
[ -z "$TARGET" ] && TARGET="$RESOLV"

first_dns=$(grep '^nameserver' "$TARGET" | head -n1 | awk '{print $2}')
if [ "$first_dns" = "127.0.0.1" ]; then
    echo "$LANG_DNS_ALREADY_CONFIGURED"
else
    TMPFILE=$(mktemp)

    # 获取nameservers列表并排除127.0.0.1
    orig_dns=$(grep '^nameserver' "$TARGET" | awk '{print $2}' | grep -v '^127\.0\.0\.1$')

    {
      echo "nameserver 127.0.0.1"
      for dns in $orig_dns; do
          echo "nameserver $dns"
      done
    } > "$TMPFILE"

    # 追加非nameserver配置
    grep -v '^nameserver' "$TARGET" >> "$TMPFILE"

    # 覆盖原始文件
    cat "$TMPFILE" > "$TARGET"
    rm -f "$TMPFILE"
    echo "$LANG_DNS_CONFIG_COMPLETED"
fi

# ========================
# 启动/处理容器
# ========================
if docker ps -a --format '{{.Names}}' | grep -q "^$CONTAINER_NAME$"; then
    echo "$LANG_CONTAINER_EXISTS $CONTAINER_NAME, $LANG_SELECT_OPERATION:"
    echo "1) $LANG_DELETE_REBUILD"
    echo "2) $LANG_USE_NEW_NAME"
    echo "3) $LANG_EXIT"
    
    while true; do
        read -r -p "$LANG_ENTER_CHOICE [1-3]: " choice
        case "$choice" in
            1)
                echo "$LANG_DELETING_OLD_CONTAINER..."
                docker rm -f "$CONTAINER_NAME"
                start_container "$CONTAINER_NAME"
                break
                ;;
            2)
                read -r -p "$LANG_ENTER_NEW_NAME: " newname
                if [ -z "$newname" ]; then
                    echo "$LANG_NAME_CANNOT_BE_EMPTY, $LANG_EXITING"
                    exit 1
                fi
                start_container "$newname"
                break
                ;;
            3)
                echo "$LANG_EXITED"
                exit 0
                ;;
            *) ;;
        esac
    done
else
    start_container "$CONTAINER_NAME"
fi

echo "$LANG_CONTAINER_STARTED_SUCCESSFULLY"
