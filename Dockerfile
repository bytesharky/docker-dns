# ========================
# Stage 1: Build
# ========================
FROM alpine:3.22 AS builder

# 安装编译工具和 ldns
RUN apk add --no-cache build-base ldns-dev

# 创建工作目录
WORKDIR /app

# 复制源代码
COPY docker-dns.c /app/
COPY logging.c /app/
COPY logging.h /app/

# 编译
RUN gcc docker-dns.c logging.c -o docker-dns -lldns

# ========================
# Stage 2: Runtime
# ========================
FROM alpine:3.22

# 安装 ldns
RUN apk add --no-cache ldns \
    && rm -rf /var/cache/apk/*

# 拷贝二进制到运行镜像
COPY --from=builder /app/docker-dns /app/docker-dns

WORKDIR /app

# 暴露 53/udp 端口
EXPOSE 53/udp

# 设置容器入口
CMD ["./docker-dns"]
