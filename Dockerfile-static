# ========================
# Stage 1: Build
# ========================
FROM alpine:3.23 AS builder

# 使用国内源
# MIRRORS RUN sed -i 's/dl-cdn.alpinelinux.org/mirrors.tencent.com/g' /etc/apk/repositories && apk update

# 安装构建工具和静态依赖
RUN apk add --no-cache \
      build-base wget upx\
      openssl-dev openssl-libs-static \
      libevent-dev libevent-static \
      zlib-dev zlib-static \
      ldns-dev ldns-static

# 已经在alpine官方提交PR增加ldns静态版，alpine:3.23 以上无需在自行编译
# # 下载ldns发行版
# WORKDIR /src
# RUN wget https://www.nlnetlabs.nl/downloads/ldns/ldns-1.8.3.tar.gz && \
#     tar xzf ldns-1.8.3.tar.gz && \
#     cd ldns-1.8.3

# # 编译ldns静态版（alpine官方没有提供）
# WORKDIR /src/ldns-1.8.3
# RUN  ./configure --enable-static --disable-shared --with-ssl=/usr LDFLAGS="-static" && \
#     make -j$(nproc) && \
#     make install

# 复制程序源码
WORKDIR /app
COPY /src ./src
COPY /include ./include

# 增加静态标记
RUN sed -i -E 's/^#define[[:space:]]+VERSION[[:space:]]+"([^"]+)"/#define VERSION "\1(static)"/' ./include/config.h

# 编译成静态二进制
RUN gcc -static -O2 -s -o ./docker-dns ./src/*.c \
    -I./include -lldns -lssl -lcrypto -levent -lz -lpthread

RUN upx --best --lzma ./docker-dns

# ========================
# Stage 2: Runtime
# ========================
FROM scratch

# 拷贝二进制到运行镜像
COPY --from=builder /app/docker-dns /docker-dns

# 暴露 53/udp 端口
EXPOSE 53/udp

# 设置容器入口
ENTRYPOINT ["/docker-dns"]

CMD ["-f"]
