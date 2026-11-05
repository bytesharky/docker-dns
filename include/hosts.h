#ifndef HOSTS_H
#define HOSTS_H

#include <stddef.h>

/* 本地 hosts 文件解析支持。
 * 解析“IP + 多个主机名”的标准格式（IPv4 / IPv6），用于 DNS 查询的本地覆盖，
 * 优先级高于后缀门控与上游转发（行为同系统 /etc/hosts）。 */

/* hosts 应答的 TTL（秒） */
#define HOSTS_TTL 3600

/* 加载本地 hosts 文件；成功返回 0。文件不存在/无法打开返回 -1（调用方可忽略，
 * 表示关闭本地 hosts 解析）。多次调用会先释放旧数据再重新加载。 */
int hosts_load(const char *path);

/* 按主机名查找 hosts 条目。
 * name:       查询名（可带尾部点，内部会规范化：去尾点并转小写）
 * want_family: 0=任意；AF_INET=仅 IPv4；AF_INET6=仅 IPv6
 * out_ip:     输出缓冲，每条 46 字节（含 '\0'），存放匹配到的 IP 文本
 * max_out:    out_ip 可容纳的条目数
 * 返回实际填入的条目数（<= max_out），0 表示未命中 */
int hosts_lookup(const char *name, int want_family, char out_ip[][46], size_t max_out);

/* 释放 hosts 数据（程序退出时调用，可选） */
void hosts_free(void);

#endif
