#include "logging.h"    // for log_msg, LOG_ERROR, LOG_DEBUG
#include "loop_marker.h"
#include <arpa/inet.h>  // for htons, ntohs
#include <string.h>     // for size_t, memcpy
// #include <ldns/edns.h>  // for ldns_edns_deep_free, ldns_edns_get_code, ldns...
#include <ldns/ldns.h>

// 添加或更新环路检测标识
void add_loop_marker(ldns_pkt *pkt, uint16_t hop_count)
{
    if (!pkt) {
        log_msg(LOG_ERROR, "Failed for add loopmarker: pkt is NULL");
        return;
    }

    /* 确保 packet 有 EDNS（OPT RR 会自动生成） */
    ldns_pkt_set_edns_udp_size(pkt, 4096);
    ldns_pkt_set_edns_version(pkt, 0);

    /* 获取或创建 EDNS option list */
    ldns_edns_option_list *olist = ldns_pkt_edns_get_option_list(pkt);
    if (!olist) {
        olist = ldns_edns_option_list_new();
        if (!olist) {
            log_msg(LOG_ERROR, "Failed to create EDNS option list");
            return;
        }
        ldns_pkt_set_edns_option_list(pkt, olist);
    }

    /* 构造新的 option 数据 */
    uint16_t hops_n = htons(hop_count);
    ldns_edns_option *new_opt = ldns_edns_new_from_data(
        (ldns_edns_option_code)MY_OPTION_CODE,
        sizeof(hops_n),
        &hops_n
    );
    if (!new_opt) {
        log_msg(LOG_ERROR, "Failed to create EDNS option");
        return;
    }

    /* 检查是否已有同样 code 的 option，替换旧的 */
    size_t count = ldns_edns_option_list_get_count(olist);
    for (size_t i = 0; i < count; i++) {
        ldns_edns_option *opt = ldns_edns_option_list_get_option(olist, i);
        if (!opt) continue;

        if (ldns_edns_get_code(opt) == (ldns_edns_option_code)MY_OPTION_CODE) {
            ldns_edns_option *old = ldns_edns_option_list_set_option(olist, new_opt, i);
            if (old) {
                ldns_edns_deep_free(old);
            }
            /* new_opt 已被列表接管 */
            return;
        }
    }

    /* 没找到则追加 */
    if (!ldns_edns_option_list_push(olist, new_opt)) {
        log_msg(LOG_ERROR, "Failed to push new EDNS option");
        ldns_edns_deep_free(new_opt);
    }
}

// 获取环路检测标识
uint16_t get_loop_marker(ldns_pkt *pkt)
{
    if (!pkt) {
        log_msg(LOG_ERROR, "Failed for get loop marker: pkt is NULL");
        return 0;
    }

    ldns_edns_option_list *olist = ldns_pkt_edns_get_option_list(pkt);
    if (!olist) {
        log_msg(LOG_DEBUG, "Failed for get loop marker: no EDNS option list");
        return 0;
    }

    size_t count = ldns_edns_option_list_get_count(olist);
    for (size_t i = 0; i < count; i++) {
        ldns_edns_option *opt = ldns_edns_option_list_get_option(olist, i);
        if (!opt) continue;

        if (ldns_edns_get_code(opt) == (ldns_edns_option_code)MY_OPTION_CODE) {
            const uint8_t *data = ldns_edns_get_data(opt);
            size_t datalen = ldns_edns_get_size(opt);
            if (!data || datalen < HOP_COUNT_DATA_LEN) {
                log_msg(LOG_ERROR, "Option found but data too short");
                return 0;
            }

            uint16_t hops;
            memcpy(&hops, data, sizeof(uint16_t));
            return ntohs(hops);
        }
    }

    log_msg(LOG_ERROR, "Option code %u not found", MY_OPTION_CODE);
    return 0;
}
