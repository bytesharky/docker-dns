#ifndef DOH_H
#define DOH_H

// DoH/HTTP 服务器线程入口。
// 在 doh_port（默认 80）上启动一个极简 TCP HTTP 服务器，提供：
//   - RFC 8484 DNS-over-HTTPS：
//       POST /dns-query   （或任意路径）  Content-Type: application/dns-message
//       GET  /dns-query?dns=<base64url>  （或任意带 dns= 参数的路径）
//   - JSON 调试端点： GET /?domain=example.com&type=A
//   - 健康检查：     GET /healthz  或  GET /
// 后端复用 docker-dns 自身的解析逻辑（resolve_dns_query），
// 因此 .docker 后缀 / gateway.docker 等规则在 DoH 下同样生效。
void* doh_server_thread(void *arg);

#endif
