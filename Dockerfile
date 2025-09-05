# ========================
# Stage 1: Build
# ========================
FROM alpine:3.22 AS builder

# 安装构建工具和静态依赖
RUN apk add --no-cache \
      build-base wget upx\
      openssl-dev openssl-libs-static \
      libevent-dev libevent-static \
      zlib-dev zlib-static

# 下载ldns发行版
WORKDIR /src
RUN wget https://www.nlnetlabs.nl/downloads/ldns/ldns-1.8.3.tar.gz && \
    tar xzf ldns-1.8.3.tar.gz && \
    cd ldns-1.8.3

# 编译ldns静态版（alpine官方没有提供）
WORKDIR /src/ldns-1.8.3
RUN  ./configure --enable-static --disable-shared --with-ssl=/usr LDFLAGS="-static" && \
    make -j$(nproc) && \
    make install

# 复制程序源码
COPY docker-dns.c /src/
COPY logging.c /src/
COPY logging.h /src/

# 编译成静态二进制
RUN gcc -static -O2 -s -o /src/docker-dns /src/docker-dns.c /src/logging.c \
    -I/usr/local/include \
    /usr/local/lib/libldns.a \
    -lssl -lcrypto -levent -lz -lpthread

RUN upx --best --lzma /src/docker-dns

# ========================
# Stage 2: Runtime
# ========================
FROM scratch

COPY --from=builder /src/docker-dns /docker-dns

ENTRYPOINT ["/docker-dns"]
