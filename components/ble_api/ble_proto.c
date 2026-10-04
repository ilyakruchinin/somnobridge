/*
 * SomnoBridge — BLE protocol framing (host-testable, spec §10.1)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#include "ble_proto.h"

void blep_reasm_init(blep_reasm_t *r)
{
    memset(r, 0, sizeof(*r));
}

bool blep_reasm_feed(blep_reasm_t *r, const uint8_t *data, uint16_t len,
                     int64_t now_ms, uint32_t *rid_out)
{
    if (len < sizeof(blep_hdr_t)) {
        return false;
    }
    blep_hdr_t h;
    memcpy(&h, data, sizeof(h));      /* packed LE — struct is packed  */

    if (h.version != 1 || h.kind != BLEP_JSON_REQUEST) {
        return false;
    }
    const uint8_t *pl = data + sizeof(h);
    uint16_t plen = len - sizeof(h);

    if (h.flags & BLEP_FLAG_START) {
        /* a fresh logical message */
        if (h.offset != 0 || h.request_id == 0) {
            return false;
        }
        r->open = true;
        r->request_id = h.request_id;
        r->len = 0;
        r->expected = 0;
        r->deadline_ms = now_ms + BLEP_REASM_MS;
    } else if (!r->open || h.request_id != r->request_id ||
               h.offset != r->expected) {
        /* mid-stream fragment for the wrong/absent message */
        return false;
    }
    if (!r->open) {
        return false;
    }
    if (now_ms > r->deadline_ms) {
        r->open = false;
        return false;
    }
    if (r->len + plen > BLEP_JSON_MAX) {
        r->open = false;
        return false;
    }
    memcpy(r->buf + r->len, pl, plen);
    r->len += plen;
    r->expected += plen;

    if (h.flags & BLEP_FLAG_END) {
        r->open = false;
        *rid_out = r->request_id;
        return true;
    }
    return false;
}

uint16_t blep_build_frame(uint8_t *out, uint16_t mtu_payload,
                          blep_kind_t kind, uint32_t request_id,
                          uint64_t offset,
                          const uint8_t *payload, uint16_t plen,
                          bool first, bool last)
{
    /* caller guarantees plen <= mtu_payload - sizeof(hdr); clamp a
     * buggy caller rather than overrun the ATT write buffer         */
    uint16_t room = mtu_payload > sizeof(blep_hdr_t)
                        ? (uint16_t)(mtu_payload - sizeof(blep_hdr_t)) : 0;
    if (plen > room) plen = room;
    blep_hdr_t h = {
        .version = 1,
        .kind = (uint8_t)kind,
        .flags = (uint16_t)((first ? BLEP_FLAG_START : 0) |
                            (last ? BLEP_FLAG_END : 0)),
        .request_id = request_id,
        .offset = offset,
    };
    memcpy(out, &h, sizeof(h));
    if (plen) {
        memcpy(out + sizeof(h), payload, plen);
    }
    return (uint16_t)(sizeof(h) + plen);
}
