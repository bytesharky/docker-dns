# Docker DNS 转发器：宿主机解析 `.docker` 域名

在 Docker 中，容器之间通常可以通过容器名互相访问（依赖 Docker 内置 DNS `127.0.0.11`），但 **宿主机默认无法直接解析容器名**。

本工具 —— **Docker DNS 转发器**，就是为了解决这个问题：让宿主机能直接用 `容器名.docker` 访问容器服务，**无需端口映射、无需修改 hosts 文件**。

---

## ✨ 功能特性

* **宿主机直接解析容器名**
  在宿主机上通过 `容器名.docker` 访问容器，例如：

  ```bash
  curl http://myapp.docker:8080
  ```

* **轻量级转发逻辑**

  * 匹配 `.docker` 域名 → 转发到 Docker 内置 DNS（`127.0.0.11`）解析容器 IP；
  * 其他域名 → 返回 `REFUSED`，宿主机自动使用公共 DNS，不影响正常上网。

* **网关访问支持**

  默认提供 `gateway.docker` 域名，可解析为宿主机 IP（可通过环境变量 `GATEWAY` 修改）。

* **低资源占用**

  基于 `ldns` 库 + UDP 协议，仅用极少 CPU 和内存即可运行。

* **容器内使用**

  Docker内置的DNS服务器，会将非容器名的域名转发到宿主机的DNS服务，这就意味这你也可以在Docker容器中使用类似 `gateway.docker` 的域名。
  
  Docker会将它转发至宿主机配置的DNS服务器（即该转发器），该转发器先去除 `.docker` 后缀变为合法容器名，再由Docker内置的DNS服务器解析得到IP地址，并返回最终结果。

* **史诗级的大小优化**

  针对镜像体积进行史诗级的优化，从原本 8.73MB 到目前 1.22MB 。

  **原方案：** Alpine3.22(8.31MB) + ldns库(360.0KB) + docker-dns(37.3K)，最终镜像约为8.73MB。

  **新方案：** Scratch(0B) + docker-dns(4.08 MB，upx压缩后1.22MB)，最终镜像约为1.22MB。

  **优化过程：**

  **优化1：** 使用静态编译

  **优化2：** 优化编译，去除符号表

  **优化3：** 使用 UPX 压缩

  **优化4：** 基于 `Scratch` (空白镜像)

---

## ⚠️ 注意事项

* Docker **默认 bridge 网络（docker0）** 不支持容器名互通，只能通过 IP。

  请使用 **自定义网络**（`docker network create` 创建）才能正常通过容器名解析。

  有关Docker自定义网络的内容可参考Docker官方说明。

* `.docker` 域名 **不区分大小写**。

---

## 🛠️ 工作原理

```mermaid
graph TD
    A[宿主机发起 DNS 查询] --> B{域名是否以 .docker 结尾?}
    B -->|是| C[去掉 .docker 后缀 → 转发至 127.0.0.11]
    C --> D[返回容器 IP 给宿主机]
    B -->|否| E[返回 REFUSED]
    E --> F[宿主机尝试公共 DNS 解析]
```

## 🚀 部署方式

### 方式一：脚本自动化

下载并运行仓库中的 `docker_dns.sh`，按提示完成部署。

### 方式二：手动部署

1. 构建镜像并运行容器：

   ```bash
   # 克隆源代码
   git clone https://github.com/bytesharky/docker-dns
   # 国内可用镜像：
   # git clone https://gitee.com/bytesharky/docker-dns

   cd docker-dns
   docker build -t docker-dns:static .

   # 启动容器
   # 挂载时区数据（非必须，用日志显示本地时间）
   # 设置日志级别（非必须，默认为 INFO）
   docker run -d \
     -e LOG_LEVEL=INFO \
     -e TZ=/zoneinfo/Asia/Shanghai \
     -v /usr/share/zoneinfo:/zoneinfo:ro \
     --network docker-net \
     --name docker-dns \
     -p 53:53/udp \
     --restart always \
     docker-dns:static
   ```

2. 配置宿主机 DNS

   编辑 `/etc/resolv.conf`，将 `127.0.0.1` 置顶：

   ```conf
   nameserver 127.0.0.1       # 本地转发器
   nameserver 223.5.5.5       # 公共 DNS 1
   nameserver 8.8.8.8         # 公共 DNS 2
   ```

   （可选）防止文件被系统覆盖：

   ```bash
   sudo chattr +i /etc/resolv.conf
   ```

### 方式三：使用构建好的镜像

1. 拉取我构建好的镜像

   ```bash
   docker pull ccr.ccs.tencentyun.com/sharky/docker-dns:static

   docker tag ccr.ccs.tencentyun.com/sharky/docker-dns:static docker-dns:static
   
   # 启动容器
   # 挂载时区数据（非必须，用日志显示本地时间）
   # 设置日志级别（非必须，默认为 INFO）
   docker run -d \
     -e LOG_LEVEL=INFO \
     -e TZ=/zoneinfo/Asia/Shanghai \
     -v /usr/share/zoneinfo:/zoneinfo:ro \
     --network docker-net \
     --name docker-dns \
     -p 53:53/udp \
     --restart always \
     docker-dns:static
   ```

2. 配置宿主机 DNS

   参考方式二

---

## ✅ 功能验证

1. **验证容器名解析**

   ```bash
   ping -c 3 docker-dns.docker
   ```

   预期输出（IP 即容器内网地址）：

   ```bash
   PING docker-dns.docker (172.18.0.6): 56 data bytes
   64 bytes from 172.18.0.6: icmp_seq=1 ttl=64 time=0.05 ms
   ```

2. **验证公共域名解析**

   ```bash
   ping -c 3 github.com
   ```

   预期输出（公共 IP）：

   ```bash
   PING github.com (140.82.112.4): 56 data bytes
   64 bytes from 140.82.112.4: icmp_seq=1 ttl=51 time=10.2 ms
   ```

---

## 故障排除

通过设置环境变量 `LOG_LEVEL` 来控制程序输出信息。默认为 `INFO`。

支持的级别：`DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`

### 日志级别（DEBUG/INFO/WARN/ERROR/FATAL）说明表

| 日志级别 | 核心含义 | 严重程度 |
|----------|----------|----------|
| 0=>DEBUG    | 调试级别，用于开发/测试阶段打印详细运行信息，辅助定位代码问题 | 最低（仅开发环境常用） |
| 1=>INFO     | 信息级别，记录系统正常运行的关键状态 | 较低（生产环境可开启，记录正常事件） |
| 2=>WARN     | 警告级别，记录非致命性异常或潜在风险，系统可继续运行 | 中等（需监控，可能预示后续问题） |
| 3=>ERROR    | 错误级别，记录致命性异常，单次DNS解析失败，但不影响系统整体运行 | 较高（需及时排查，避免影响范围扩大） |
| 4=>FATAL    | 致命级别，记录导致系统完全无法运行的严重错误 | 最高（系统不可用，需紧急处理） |

## 📌 总结

Docker DNS 转发器相当于宿主机与 Docker 内置 DNS 之间的“桥梁”，特点是：

* 🟢 宿主机可无缝解析 `.docker` 域名
* 🟢 不影响正常上网解析
* 🟢 部署简单、占用极低

适用于：

**开发/测试环境**，或希望宿主机直接通过容器名访问服务的场景。

**生产环境**谨慎使用，更推荐固定IP方式。

## 附：命令行参数/环境变量说明

| 命令行选项 | 环境变量 | 说明 | 默认值 |
|------------|----------|------|--------|
| `-L, --log-level` | `LOG_LEVEL` | 设置日志级别 | INFO |
| `-G, --gateway` | `GATEWAY_NAME` | 设置网关名称 | gateway |
| `-S, --suffix` | `SUFFIX_DOMAIN` | 设置后缀名称 | .docker |
| `-C, --container` | `CONTAINER_NAME` | 设置容器名称 | docker-dns |
| `-D, --dns-server` | `FORWARD_DNS` | 设置转发DNS服务器 | 127.0.0.11 |
| `-P, --port` | `LISTEN_PORT` | 设置监听端口 | 53 |
| `-f, --foreground` | - | 以前台模式运行（不守护进程） | 默认后台运行 |
| `-h, --help` | - | 显示帮助信息并退出 | - |

```bash
./docker-dns -h
Usage: ./docker-dns [OPTIONS]
Options:
  -L, --log-level    Set log level (DEBUG, default: INFO, WARN, ERROR, FATAL)
  -G, --gateway      Set gateway name (default: gateway)
  -S, --suffix       Set suffix name (default: .docker)
  -C, --container    Set container name (default: docker-dns)
  -D, --dns-server   Set forward DNS server (default: 127.0.0.11)
  -P, --port         Set listening port (default: 53)
  -f, --foreground   Run in foreground mode (do not daemonize)
  -h, --help         Show this help message and exit
```

---
