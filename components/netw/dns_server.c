/*
 * SomnoBridge — minimal wildcard DNS responder for the setup AP.
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Answers every query with 192.168.8.1 so OS captive-portal probes
 * (generate_204, hotspot-detect, ncsi, ...) land on the setup portal.
 * Only runs while the setup AP is up.
 */
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "netw.h"

static const char *TAG = "dns";

static TaskHandle_t s_task;
static int s_sock = -1;

/* Build a response: copy header+question, flip flags, append answer. */
static int dns_reply(uint8_t *buf, int rx_len)
{
    if (rx_len < 12) {
        return -1;
    }
    /* header: ID copied, flags = standard response, no error */
    buf[2] = 0x81;
    buf[3] = 0x80;
    buf[6] = buf[7] = 0;                    /* ANCOUNT hi cleared below */
    buf[6] = 0;
    buf[7] = 1;                             /* ANCOUNT = 1 */
    buf[8] = buf[9] = buf[10] = buf[11] = 0;/* NS/AR counts = 0 */

    /* find end of question section */
    int i = 12;
    while (i < rx_len && buf[i] != 0) {
        i += buf[i] + 1;
        if (i > 512) {
            return -1;
        }
    }
    i += 5;   /* skip root label + QTYPE(2) + QCLASS(2) */
    if (i > rx_len) {
        return -1;
    }
    /* answer: name pointer + A record -> 192.168.8.1 */
    static const uint8_t ans[] = {
        0xC0, 0x0C,             /* NAME  -> ptr to question */
        0x00, 0x01,             /* TYPE  = A                */
        0x00, 0x01,             /* CLASS = IN               */
        0x00, 0x00, 0x00, 0x3C, /* TTL   = 60 s             */
        0x00, 0x04,             /* RDLENGTH = 4             */
        192, 168, 8, 1,         /* RDATA                    */
    };
    memcpy(buf + i, ans, sizeof(ans));
    return i + (int)sizeof(ans);
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[600];
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0 || bind(s_sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        goto out;
    }
    ESP_LOGI(TAG, "captive DNS up");
    for (;;) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &slen);
        if (n < 0) {
            break;      /* socket closed by dns_stop */
        }
        int r = dns_reply(buf, n);
        if (r > 0) {
            sendto(s_sock, buf, r, 0, (struct sockaddr *)&src, slen);
        }
    }
out:
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

void dns_server_start(void)
{
    if (s_task) {
        return;
    }
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, &s_task);
}

void dns_server_stop(void)
{
    if (s_sock >= 0) {
        close(s_sock);   /* wakes recvfrom -> task exits */
        s_sock = -1;
    }
}
