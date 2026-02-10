#include "snmp_agent.h"

#include "ups_data.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "snmp_agent";

#ifndef UPS_SNMP_COMMUNITY
#define UPS_SNMP_COMMUNITY "public"
#endif

#ifndef UPS_SNMP_AGENT_TASK_STACK
#define UPS_SNMP_AGENT_TASK_STACK 4096U
#endif

#ifndef UPS_SNMP_AGENT_TASK_PRIO
#define UPS_SNMP_AGENT_TASK_PRIO 4U
#endif

typedef enum
{
    SNMP_TYPE_INTEGER = 0x02,
    SNMP_TYPE_OCTET_STRING = 0x04,
    SNMP_TYPE_NULL = 0x05,
    SNMP_TYPE_OBJECT_ID = 0x06,
    SNMP_TYPE_SEQUENCE = 0x30,
    SNMP_TYPE_GET_REQUEST = 0xA0,
    SNMP_TYPE_GET_NEXT_REQUEST = 0xA1,
    SNMP_TYPE_GET_RESPONSE = 0xA2,
} snmp_type_t;

typedef enum
{
    SNMP_ERR_NOERROR = 0,
    SNMP_ERR_TOOBIG = 1,
    SNMP_ERR_NOSUCHNAME = 2,
    SNMP_ERR_BADVALUE = 3,
    SNMP_ERR_READONLY = 4,
    SNMP_ERR_GENERR = 5,
} snmp_error_status_t;

typedef struct
{
    uint8_t *buf;
    size_t cap;
    size_t len;
} snmp_buf_t;

typedef struct
{
    const uint8_t *oid;
    size_t oid_len;
} snmp_oid_view_t;

typedef enum
{
    VALUE_KIND_INT32,
    VALUE_KIND_OCTETS,
} value_kind_t;

typedef struct
{
    value_kind_t kind;
    int32_t i32;
    const uint8_t *octets;
    size_t octets_len;
} snmp_value_t;

static bool s_snmp_started = false;

static const uint8_t OID_SYS_DESCR[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00};
static const uint8_t OID_SYS_NAME[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x01, 0x05, 0x00};

// RFC1628 UPS-MIB (1.3.6.1.2.1.33.1)
static const uint8_t OID_UPS_IDENT_MANUFACTURER[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x01, 0x01, 0x00};
static const uint8_t OID_UPS_IDENT_MODEL[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x01, 0x02, 0x00};
static const uint8_t OID_UPS_IDENT_UPS_SW_VER[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x01, 0x03, 0x00};
static const uint8_t OID_UPS_IDENT_AGENT_SW_VER[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x01, 0x04, 0x00};
static const uint8_t OID_UPS_IDENT_NAME[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x01, 0x05, 0x00};
static const uint8_t OID_UPS_IDENT_ATTACHED_DEVICES[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x01, 0x06, 0x00};

static const uint8_t OID_UPS_BATTERY_STATUS_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x02, 0x01, 0x00};
static const uint8_t OID_UPS_SECONDS_ON_BATTERY_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x02, 0x02, 0x00};
static const uint8_t OID_UPS_EST_MINUTES_REMAINING_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x02, 0x03, 0x00};
static const uint8_t OID_UPS_EST_CHARGE_REMAINING_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x02, 0x04, 0x00};
static const uint8_t OID_UPS_BATTERY_VOLTAGE_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x02, 0x05, 0x00};
static const uint8_t OID_UPS_BATTERY_CURRENT_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x02, 0x06, 0x00};
static const uint8_t OID_UPS_BATTERY_TEMP_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x02, 0x07, 0x00};

static const uint8_t OID_UPS_INPUT_LINE_BADS_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x03, 0x01, 0x00};
static const uint8_t OID_UPS_INPUT_NUM_LINES_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x03, 0x02, 0x00};
static const uint8_t OID_UPS_INPUT_FREQUENCY_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x03, 0x03, 0x01, 0x02, 0x01};
static const uint8_t OID_UPS_INPUT_VOLTAGE_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x03, 0x03, 0x01, 0x03, 0x01};

static const uint8_t OID_UPS_OUTPUT_SOURCE_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x04, 0x01, 0x00};
static const uint8_t OID_UPS_OUTPUT_FREQUENCY_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x04, 0x02, 0x00};
static const uint8_t OID_UPS_OUTPUT_NUM_LINES_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x04, 0x03, 0x00};
static const uint8_t OID_UPS_OUTPUT_VOLTAGE_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x04, 0x04, 0x01, 0x02, 0x01};
static const uint8_t OID_UPS_OUTPUT_CURRENT_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x04, 0x04, 0x01, 0x03, 0x01};
static const uint8_t OID_UPS_OUTPUT_POWER_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x04, 0x04, 0x01, 0x04, 0x01};
static const uint8_t OID_UPS_OUTPUT_PERCENT_LOAD_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x04, 0x04, 0x01, 0x05, 0x01};

static const uint8_t OID_UPS_CONFIG_INPUT_VOLTAGE_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x09, 0x01, 0x00};
static const uint8_t OID_UPS_CONFIG_OUTPUT_VOLTAGE_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x09, 0x03, 0x00};
static const uint8_t OID_UPS_CONFIG_OUTPUT_POWER_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x09, 0x06, 0x00};
static const uint8_t OID_UPS_CONFIG_LOW_BATT_TIME_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x09, 0x07, 0x00};
static const uint8_t OID_UPS_CONFIG_LOW_XFER_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x09, 0x09, 0x00};
static const uint8_t OID_UPS_CONFIG_HIGH_XFER_STD[] = {0x2B, 0x06, 0x01, 0x02, 0x01, 0x21, 0x01, 0x09, 0x0A, 0x00};

static const uint8_t VALUE_SYS_DESCR[] = "ESP32 UPS bridge";
static const uint8_t VALUE_SYS_NAME[] = "esp32-ups";
static const uint8_t VALUE_UPS_IDENT_MANUFACTURER[] = "APC";
static const uint8_t VALUE_UPS_IDENT_MODEL[] = "SPM2K";
static const uint8_t VALUE_UPS_IDENT_UPS_SW_VER[] = "N/A";
static const uint8_t VALUE_UPS_IDENT_AGENT_SW_VER[] = "esp32-ups-snmp";
static const uint8_t VALUE_UPS_IDENT_NAME[] = "ESP32-UPS";
static const uint8_t VALUE_UPS_IDENT_ATTACHED_DEVICES[] = "line1";

typedef struct
{
    const uint8_t *oid;
    size_t oid_len;
} oid_entry_t;

static const oid_entry_t k_oid_table[] = {
    {OID_SYS_DESCR, sizeof(OID_SYS_DESCR)},
    {OID_SYS_NAME, sizeof(OID_SYS_NAME)},
    {OID_UPS_IDENT_MANUFACTURER, sizeof(OID_UPS_IDENT_MANUFACTURER)},
    {OID_UPS_IDENT_MODEL, sizeof(OID_UPS_IDENT_MODEL)},
    {OID_UPS_IDENT_UPS_SW_VER, sizeof(OID_UPS_IDENT_UPS_SW_VER)},
    {OID_UPS_IDENT_AGENT_SW_VER, sizeof(OID_UPS_IDENT_AGENT_SW_VER)},
    {OID_UPS_IDENT_NAME, sizeof(OID_UPS_IDENT_NAME)},
    {OID_UPS_IDENT_ATTACHED_DEVICES, sizeof(OID_UPS_IDENT_ATTACHED_DEVICES)},
    {OID_UPS_BATTERY_STATUS_STD, sizeof(OID_UPS_BATTERY_STATUS_STD)},
    {OID_UPS_SECONDS_ON_BATTERY_STD, sizeof(OID_UPS_SECONDS_ON_BATTERY_STD)},
    {OID_UPS_EST_MINUTES_REMAINING_STD, sizeof(OID_UPS_EST_MINUTES_REMAINING_STD)},
    {OID_UPS_EST_CHARGE_REMAINING_STD, sizeof(OID_UPS_EST_CHARGE_REMAINING_STD)},
    {OID_UPS_BATTERY_VOLTAGE_STD, sizeof(OID_UPS_BATTERY_VOLTAGE_STD)},
    {OID_UPS_BATTERY_CURRENT_STD, sizeof(OID_UPS_BATTERY_CURRENT_STD)},
    {OID_UPS_BATTERY_TEMP_STD, sizeof(OID_UPS_BATTERY_TEMP_STD)},
    {OID_UPS_INPUT_LINE_BADS_STD, sizeof(OID_UPS_INPUT_LINE_BADS_STD)},
    {OID_UPS_INPUT_NUM_LINES_STD, sizeof(OID_UPS_INPUT_NUM_LINES_STD)},
    {OID_UPS_INPUT_FREQUENCY_STD, sizeof(OID_UPS_INPUT_FREQUENCY_STD)},
    {OID_UPS_INPUT_VOLTAGE_STD, sizeof(OID_UPS_INPUT_VOLTAGE_STD)},
    {OID_UPS_OUTPUT_SOURCE_STD, sizeof(OID_UPS_OUTPUT_SOURCE_STD)},
    {OID_UPS_OUTPUT_FREQUENCY_STD, sizeof(OID_UPS_OUTPUT_FREQUENCY_STD)},
    {OID_UPS_OUTPUT_NUM_LINES_STD, sizeof(OID_UPS_OUTPUT_NUM_LINES_STD)},
    {OID_UPS_OUTPUT_VOLTAGE_STD, sizeof(OID_UPS_OUTPUT_VOLTAGE_STD)},
    {OID_UPS_OUTPUT_CURRENT_STD, sizeof(OID_UPS_OUTPUT_CURRENT_STD)},
    {OID_UPS_OUTPUT_POWER_STD, sizeof(OID_UPS_OUTPUT_POWER_STD)},
    {OID_UPS_OUTPUT_PERCENT_LOAD_STD, sizeof(OID_UPS_OUTPUT_PERCENT_LOAD_STD)},
    {OID_UPS_CONFIG_INPUT_VOLTAGE_STD, sizeof(OID_UPS_CONFIG_INPUT_VOLTAGE_STD)},
    {OID_UPS_CONFIG_OUTPUT_VOLTAGE_STD, sizeof(OID_UPS_CONFIG_OUTPUT_VOLTAGE_STD)},
    {OID_UPS_CONFIG_OUTPUT_POWER_STD, sizeof(OID_UPS_CONFIG_OUTPUT_POWER_STD)},
    {OID_UPS_CONFIG_LOW_BATT_TIME_STD, sizeof(OID_UPS_CONFIG_LOW_BATT_TIME_STD)},
    {OID_UPS_CONFIG_LOW_XFER_STD, sizeof(OID_UPS_CONFIG_LOW_XFER_STD)},
    {OID_UPS_CONFIG_HIGH_XFER_STD, sizeof(OID_UPS_CONFIG_HIGH_XFER_STD)},
};

typedef struct
{
    int32_t version;
    const uint8_t *community;
    size_t community_len;
    int32_t request_id;
    uint8_t pdu_type;
    snmp_oid_view_t request_oid;
} snmp_request_t;

static int snmp_oid_compare(const uint8_t *lhs, size_t lhs_len, const uint8_t *rhs, size_t rhs_len)
{
    size_t const min_len = (lhs_len < rhs_len) ? lhs_len : rhs_len;
    for (size_t i = 0U; i < min_len; i++)
    {
        if (lhs[i] < rhs[i])
        {
            return -1;
        }
        if (lhs[i] > rhs[i])
        {
            return 1;
        }
    }

    if (lhs_len < rhs_len)
    {
        return -1;
    }
    if (lhs_len > rhs_len)
    {
        return 1;
    }
    return 0;
}

static bool snmp_read_len(const uint8_t **pp, const uint8_t *end, size_t *out_len)
{
    if ((*pp == NULL) || (out_len == NULL) || (*pp >= end))
    {
        return false;
    }

    uint8_t const first = **pp;
    (*pp)++;

    if ((first & 0x80U) == 0U)
    {
        *out_len = first;
        return ((size_t)(end - *pp) >= *out_len);
    }

    uint8_t const count = (uint8_t)(first & 0x7FU);
    if ((count == 0U) || (count > 2U) || ((size_t)(end - *pp) < count))
    {
        return false;
    }

    size_t len = 0U;
    for (uint8_t i = 0U; i < count; i++)
    {
        len = (len << 8) | (*pp)[i];
    }
    *pp += count;
    *out_len = len;
    return ((size_t)(end - *pp) >= len);
}

static bool snmp_expect_tlv(const uint8_t **pp,
                            const uint8_t *end,
                            uint8_t expected_type,
                            const uint8_t **value,
                            size_t *value_len)
{
    if ((*pp == NULL) || (*pp >= end) || (**pp != expected_type))
    {
        return false;
    }

    (*pp)++;
    if (!snmp_read_len(pp, end, value_len))
    {
        return false;
    }

    *value = *pp;
    *pp += *value_len;
    return true;
}

static bool snmp_decode_int32(const uint8_t *buf, size_t len, int32_t *out)
{
    if ((buf == NULL) || (out == NULL) || (len == 0U) || (len > 4U))
    {
        return false;
    }

    int32_t v = ((buf[0] & 0x80U) != 0U) ? -1 : 0;
    for (size_t i = 0U; i < len; i++)
    {
        v = (int32_t)((v << 8) | buf[i]);
    }
    *out = v;
    return true;
}

static size_t snmp_len_field_size(size_t len)
{
    if (len < 128U)
    {
        return 1U;
    }
    if (len <= 0xFFU)
    {
        return 2U;
    }
    return 3U;
}

static bool snmp_buf_put_u8(snmp_buf_t *w, uint8_t v)
{
    if ((w == NULL) || (w->len >= w->cap))
    {
        return false;
    }
    w->buf[w->len++] = v;
    return true;
}

static bool snmp_buf_put_mem(snmp_buf_t *w, const uint8_t *src, size_t len)
{
    if ((w == NULL) || ((len > 0U) && (src == NULL)) || ((w->len + len) > w->cap))
    {
        return false;
    }

    if (len > 0U)
    {
        memcpy(&w->buf[w->len], src, len);
        w->len += len;
    }
    return true;
}

static bool snmp_buf_put_len(snmp_buf_t *w, size_t len)
{
    if (len < 128U)
    {
        return snmp_buf_put_u8(w, (uint8_t)len);
    }
    if (len <= 0xFFU)
    {
        return snmp_buf_put_u8(w, 0x81U) && snmp_buf_put_u8(w, (uint8_t)len);
    }

    return snmp_buf_put_u8(w, 0x82U) &&
           snmp_buf_put_u8(w, (uint8_t)((len >> 8) & 0xFFU)) &&
           snmp_buf_put_u8(w, (uint8_t)(len & 0xFFU));
}

static size_t snmp_int32_encoded_len(int32_t value)
{
    size_t len = 4U;
    uint8_t bytes[4];
    bytes[0] = (uint8_t)((value >> 24) & 0xFF);
    bytes[1] = (uint8_t)((value >> 16) & 0xFF);
    bytes[2] = (uint8_t)((value >> 8) & 0xFF);
    bytes[3] = (uint8_t)(value & 0xFF);

    while (len > 1U)
    {
        if ((bytes[4U - len] == 0x00U) && ((bytes[4U - len + 1U] & 0x80U) == 0U))
        {
            len--;
            continue;
        }
        if ((bytes[4U - len] == 0xFFU) && ((bytes[4U - len + 1U] & 0x80U) != 0U))
        {
            len--;
            continue;
        }
        break;
    }

    return len;
}

static bool snmp_put_tlv_header(snmp_buf_t *w, uint8_t type, size_t value_len)
{
    return snmp_buf_put_u8(w, type) && snmp_buf_put_len(w, value_len);
}

static bool snmp_put_int32(snmp_buf_t *w, int32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)((value >> 24) & 0xFF);
    bytes[1] = (uint8_t)((value >> 16) & 0xFF);
    bytes[2] = (uint8_t)((value >> 8) & 0xFF);
    bytes[3] = (uint8_t)(value & 0xFF);

    size_t len = snmp_int32_encoded_len(value);
    size_t start = 4U - len;

    return snmp_put_tlv_header(w, SNMP_TYPE_INTEGER, len) &&
           snmp_buf_put_mem(w, &bytes[start], len);
}

static bool snmp_put_octets(snmp_buf_t *w, const uint8_t *buf, size_t len)
{
    return snmp_put_tlv_header(w, SNMP_TYPE_OCTET_STRING, len) &&
           snmp_buf_put_mem(w, buf, len);
}

static bool snmp_put_oid(snmp_buf_t *w, const uint8_t *oid, size_t len)
{
    return snmp_put_tlv_header(w, SNMP_TYPE_OBJECT_ID, len) &&
           snmp_buf_put_mem(w, oid, len);
}

static bool snmp_put_null(snmp_buf_t *w)
{
    return snmp_put_tlv_header(w, SNMP_TYPE_NULL, 0U);
}

static bool snmp_decode_request(const uint8_t *pkt,
                                size_t pkt_len,
                                snmp_request_t *out_req)
{
    if ((pkt == NULL) || (out_req == NULL))
    {
        return false;
    }

    const uint8_t *p = pkt;
    const uint8_t *end = pkt + pkt_len;
    const uint8_t *value = NULL;
    size_t value_len = 0U;

    if (!snmp_expect_tlv(&p, end, SNMP_TYPE_SEQUENCE, &value, &value_len))
    {
        return false;
    }
    const uint8_t *msg_p = value;
    const uint8_t *msg_end = value + value_len;

    if (!snmp_expect_tlv(&msg_p, msg_end, SNMP_TYPE_INTEGER, &value, &value_len))
    {
        return false;
    }
    if (!snmp_decode_int32(value, value_len, &out_req->version))
    {
        return false;
    }

    if (!snmp_expect_tlv(&msg_p, msg_end, SNMP_TYPE_OCTET_STRING, &value, &value_len))
    {
        return false;
    }
    out_req->community = value;
    out_req->community_len = value_len;

    if ((msg_p >= msg_end) || ((*msg_p != SNMP_TYPE_GET_REQUEST) && (*msg_p != SNMP_TYPE_GET_NEXT_REQUEST)))
    {
        return false;
    }
    out_req->pdu_type = *msg_p;
    msg_p++;
    if (!snmp_read_len(&msg_p, msg_end, &value_len))
    {
        return false;
    }

    const uint8_t *pdu_end = msg_p + value_len;

    if (!snmp_expect_tlv(&msg_p, pdu_end, SNMP_TYPE_INTEGER, &value, &value_len))
    {
        return false;
    }
    if (!snmp_decode_int32(value, value_len, &out_req->request_id))
    {
        return false;
    }

    if (!snmp_expect_tlv(&msg_p, pdu_end, SNMP_TYPE_INTEGER, &value, &value_len))
    {
        return false;
    }

    if (!snmp_expect_tlv(&msg_p, pdu_end, SNMP_TYPE_INTEGER, &value, &value_len))
    {
        return false;
    }

    if (!snmp_expect_tlv(&msg_p, pdu_end, SNMP_TYPE_SEQUENCE, &value, &value_len))
    {
        return false;
    }

    const uint8_t *vb_list_p = value;
    const uint8_t *vb_list_end = value + value_len;
    if (!snmp_expect_tlv(&vb_list_p, vb_list_end, SNMP_TYPE_SEQUENCE, &value, &value_len))
    {
        return false;
    }

    const uint8_t *vb_p = value;
    const uint8_t *vb_end = value + value_len;

    if (!snmp_expect_tlv(&vb_p, vb_end, SNMP_TYPE_OBJECT_ID, &value, &value_len))
    {
        return false;
    }

    out_req->request_oid.oid = value;
    out_req->request_oid.oid_len = value_len;

    return true;
}

static bool snmp_lookup_exact(snmp_oid_view_t oid, size_t *out_index)
{
    if ((oid.oid == NULL) || (out_index == NULL))
    {
        return false;
    }

    for (size_t i = 0U; i < (sizeof(k_oid_table) / sizeof(k_oid_table[0])); i++)
    {
        if ((oid.oid_len == k_oid_table[i].oid_len) &&
            (memcmp(oid.oid, k_oid_table[i].oid, oid.oid_len) == 0))
        {
            *out_index = i;
            return true;
        }
    }

    return false;
}

static bool snmp_lookup_next(snmp_oid_view_t oid, size_t *out_index)
{
    if ((oid.oid == NULL) || (out_index == NULL))
    {
        return false;
    }

    for (size_t i = 0U; i < (sizeof(k_oid_table) / sizeof(k_oid_table[0])); i++)
    {
        if (snmp_oid_compare(k_oid_table[i].oid, k_oid_table[i].oid_len, oid.oid, oid.oid_len) > 0)
        {
            *out_index = i;
            return true;
        }
    }

    return false;
}

static bool snmp_get_value_by_index(size_t index, snmp_value_t *out_value)
{
    if (out_value == NULL)
    {
        return false;
    }

    memset(out_value, 0, sizeof(*out_value));

    switch (index)
    {
    case 0U:
        out_value->kind = VALUE_KIND_OCTETS;
        out_value->octets = VALUE_SYS_DESCR;
        out_value->octets_len = sizeof(VALUE_SYS_DESCR) - 1U;
        return true;
    case 1U:
        out_value->kind = VALUE_KIND_OCTETS;
        out_value->octets = VALUE_SYS_NAME;
        out_value->octets_len = sizeof(VALUE_SYS_NAME) - 1U;
        return true;
    case 2U:
        out_value->kind = VALUE_KIND_OCTETS;
        out_value->octets = VALUE_UPS_IDENT_MANUFACTURER;
        out_value->octets_len = sizeof(VALUE_UPS_IDENT_MANUFACTURER) - 1U;
        return true;
    case 3U:
        out_value->kind = VALUE_KIND_OCTETS;
        out_value->octets = VALUE_UPS_IDENT_MODEL;
        out_value->octets_len = sizeof(VALUE_UPS_IDENT_MODEL) - 1U;
        return true;
    case 4U:
        out_value->kind = VALUE_KIND_OCTETS;
        out_value->octets = VALUE_UPS_IDENT_UPS_SW_VER;
        out_value->octets_len = sizeof(VALUE_UPS_IDENT_UPS_SW_VER) - 1U;
        return true;
    case 5U:
        out_value->kind = VALUE_KIND_OCTETS;
        out_value->octets = VALUE_UPS_IDENT_AGENT_SW_VER;
        out_value->octets_len = sizeof(VALUE_UPS_IDENT_AGENT_SW_VER) - 1U;
        return true;
    case 6U:
        out_value->kind = VALUE_KIND_OCTETS;
        out_value->octets = VALUE_UPS_IDENT_NAME;
        out_value->octets_len = sizeof(VALUE_UPS_IDENT_NAME) - 1U;
        return true;
    case 7U:
        out_value->kind = VALUE_KIND_OCTETS;
        out_value->octets = VALUE_UPS_IDENT_ATTACHED_DEVICES;
        out_value->octets_len = sizeof(VALUE_UPS_IDENT_ATTACHED_DEVICES) - 1U;
        return true;
    case 8U:
        out_value->kind = VALUE_KIND_INT32;
        if ((g_battery.remaining_capacity == 0U) ||
            g_power_summary_present_status.shutdown_imminent)
        {
            out_value->i32 = 4;
        }
        else if (g_power_summary_present_status.need_replacement)
        {
            out_value->i32 = 4;
        }
        else if (g_power_summary_present_status.below_remaining_capacity_limit ||
                 (g_battery.remaining_capacity <= g_power_summary.remaining_capacity_limit))
        {
            out_value->i32 = 3;
        }
        else
        {
            out_value->i32 = 2;
        }
        return true;
    case 9U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = g_power_summary_present_status.ac_present ? 0 : (int32_t)g_battery.run_time_to_empty_s;
        return true;
    case 10U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)(g_battery.run_time_to_empty_s / 60U);
        return true;
    case 11U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)g_battery.remaining_capacity;
        return true;
    case 12U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)(g_battery.battery_voltage / 10U);
        return true;
    case 13U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)(g_battery.battery_current / 10);
        return true;
    case 14U:
        out_value->kind = VALUE_KIND_INT32;
        if (g_battery.temperature >= 2731U)
        {
            out_value->i32 = (int32_t)((g_battery.temperature - 2731U) / 10U);
        }
        else
        {
            out_value->i32 = 0;
        }
        return true;
    case 15U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = 0;
        return true;
    case 16U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = 1;
        return true;
    case 17U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)(g_input.frequency / 10U);
        return true;
    case 18U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)((g_input.voltage + 50U) / 100U);
        return true;
    case 19U:
        out_value->kind = VALUE_KIND_INT32;
        if (g_power_summary_present_status.ac_present)
        {
            out_value->i32 = 3;
        }
        else if (g_power_summary_present_status.discharging)
        {
            out_value->i32 = 5;
        }
        else
        {
            out_value->i32 = 6;
        }
        return true;
    case 20U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)(g_output.frequency / 10U);
        return true;
    case 21U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = 1;
        return true;
    case 22U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)((g_output.voltage + 50U) / 100U);
        return true;
    case 23U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)(g_output.current / 10);
        return true;
    case 24U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)(((uint32_t)g_output.config_active_power *
                                    (uint32_t)g_output.percent_load) /
                                   100U);
        return true;
    case 25U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)g_output.percent_load;
        return true;
    case 26U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)((g_input.config_voltage + 50U) / 100U);
        return true;
    case 27U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)((g_output.config_voltage + 50U) / 100U);
        return true;
    case 28U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)g_output.config_active_power;
        return true;
    case 29U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)(g_battery.remaining_time_limit_s / 60U);
        return true;
    case 30U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)((g_input.low_voltage_transfer + 50U) / 100U);
        return true;
    case 31U:
        out_value->kind = VALUE_KIND_INT32;
        out_value->i32 = (int32_t)((g_input.high_voltage_transfer + 50U) / 100U);
        return true;
    default:
        return false;
    }
}

static bool snmp_build_response(const snmp_request_t *req,
                                int32_t error_status,
                                int32_t error_index,
                                const uint8_t *resp_oid,
                                size_t resp_oid_len,
                                const snmp_value_t *value,
                                uint8_t *out_buf,
                                size_t out_cap,
                                size_t *out_len)
{
    if ((req == NULL) || (out_buf == NULL) || (out_len == NULL) ||
        (resp_oid == NULL) || (resp_oid_len == 0U))
    {
        return false;
    }

    size_t value_tlv_len = 2U;
    if ((error_status == SNMP_ERR_NOERROR) && (value != NULL))
    {
        if (value->kind == VALUE_KIND_INT32)
        {
            size_t const i_len = snmp_int32_encoded_len(value->i32);
            value_tlv_len = 1U + snmp_len_field_size(i_len) + i_len;
        }
        else
        {
            value_tlv_len = 1U + snmp_len_field_size(value->octets_len) + value->octets_len;
        }
    }

    size_t const oid_tlv_len = 1U + snmp_len_field_size(resp_oid_len) + resp_oid_len;
    size_t const varbind_content_len = oid_tlv_len + value_tlv_len;
    size_t const varbind_tlv_len = 1U + snmp_len_field_size(varbind_content_len) + varbind_content_len;
    size_t const varbind_list_tlv_len = 1U + snmp_len_field_size(varbind_tlv_len) + varbind_tlv_len;

    size_t const reqid_payload_len = snmp_int32_encoded_len(req->request_id);
    size_t const err_status_payload_len = snmp_int32_encoded_len(error_status);
    size_t const err_index_payload_len = snmp_int32_encoded_len(error_index);

    size_t const reqid_tlv_len = 1U + snmp_len_field_size(reqid_payload_len) + reqid_payload_len;
    size_t const err_status_tlv_len = 1U + snmp_len_field_size(err_status_payload_len) + err_status_payload_len;
    size_t const err_index_tlv_len = 1U + snmp_len_field_size(err_index_payload_len) + err_index_payload_len;

    size_t const pdu_content_len = reqid_tlv_len + err_status_tlv_len + err_index_tlv_len + varbind_list_tlv_len;
    size_t const pdu_tlv_len = 1U + snmp_len_field_size(pdu_content_len) + pdu_content_len;

    size_t const version_payload_len = snmp_int32_encoded_len(req->version);
    size_t const version_tlv_len = 1U + snmp_len_field_size(version_payload_len) + version_payload_len;
    size_t const community_tlv_len = 1U + snmp_len_field_size(req->community_len) + req->community_len;

    size_t const msg_content_len = version_tlv_len + community_tlv_len + pdu_tlv_len;
    size_t const msg_tlv_len = 1U + snmp_len_field_size(msg_content_len) + msg_content_len;

    if (msg_tlv_len > out_cap)
    {
        return false;
    }

    snmp_buf_t w = {
        .buf = out_buf,
        .cap = out_cap,
        .len = 0U,
    };

    if (!snmp_put_tlv_header(&w, SNMP_TYPE_SEQUENCE, msg_content_len))
    {
        return false;
    }
    if (!snmp_put_int32(&w, req->version))
    {
        return false;
    }
    if (!snmp_put_octets(&w, req->community, req->community_len))
    {
        return false;
    }

    if (!snmp_put_tlv_header(&w, SNMP_TYPE_GET_RESPONSE, pdu_content_len))
    {
        return false;
    }
    if (!snmp_put_int32(&w, req->request_id))
    {
        return false;
    }
    if (!snmp_put_int32(&w, error_status))
    {
        return false;
    }
    if (!snmp_put_int32(&w, error_index))
    {
        return false;
    }

    if (!snmp_put_tlv_header(&w, SNMP_TYPE_SEQUENCE, varbind_tlv_len))
    {
        return false;
    }
    if (!snmp_put_tlv_header(&w, SNMP_TYPE_SEQUENCE, varbind_content_len))
    {
        return false;
    }
    if (!snmp_put_oid(&w, resp_oid, resp_oid_len))
    {
        return false;
    }

    if ((error_status == SNMP_ERR_NOERROR) && (value != NULL))
    {
        if (value->kind == VALUE_KIND_INT32)
        {
            if (!snmp_put_int32(&w, value->i32))
            {
                return false;
            }
        }
        else
        {
            if (!snmp_put_octets(&w, value->octets, value->octets_len))
            {
                return false;
            }
        }
    }
    else
    {
        if (!snmp_put_null(&w))
        {
            return false;
        }
    }

    *out_len = w.len;
    return true;
}

static void snmp_agent_task(void *arg)
{
    (void)arg;

    int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Failed to create SNMP UDP socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(161);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (lwip_bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0)
    {
        ESP_LOGE(TAG, "Failed to bind SNMP socket to UDP/161");
        lwip_close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SNMP agent listening on UDP/161");

    uint8_t rx_buf[512];
    uint8_t tx_buf[512];

    while (1)
    {
        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        int const rlen = lwip_recvfrom(sock,
                                       rx_buf,
                                       sizeof(rx_buf),
                                       0,
                                       (struct sockaddr *)&src_addr,
                                       &src_len);
        if (rlen <= 0)
        {
            continue;
        }

        snmp_request_t req;
        memset(&req, 0, sizeof(req));
        if (!snmp_decode_request(rx_buf, (size_t)rlen, &req))
        {
            continue;
        }

        if (!((req.version == 0) || (req.version == 1)))
        {
            continue;
        }

        if ((req.community_len != strlen(UPS_SNMP_COMMUNITY)) ||
            (memcmp(req.community, UPS_SNMP_COMMUNITY, req.community_len) != 0))
        {
            continue;
        }

        size_t oid_index = 0U;
        bool found = false;
        if (req.pdu_type == SNMP_TYPE_GET_REQUEST)
        {
            found = snmp_lookup_exact(req.request_oid, &oid_index);
        }
        else
        {
            found = snmp_lookup_next(req.request_oid, &oid_index);
        }

        snmp_value_t resp_value;
        memset(&resp_value, 0, sizeof(resp_value));

        int32_t error_status = SNMP_ERR_NOERROR;
        int32_t error_index = 0;
        const uint8_t *resp_oid = req.request_oid.oid;
        size_t resp_oid_len = req.request_oid.oid_len;

        if (!found)
        {
            error_status = SNMP_ERR_NOSUCHNAME;
            error_index = 1;
        }
        else
        {
            resp_oid = k_oid_table[oid_index].oid;
            resp_oid_len = k_oid_table[oid_index].oid_len;
            if (!snmp_get_value_by_index(oid_index, &resp_value))
            {
                error_status = SNMP_ERR_GENERR;
                error_index = 1;
            }
        }

        size_t tx_len = 0U;
        if (!snmp_build_response(&req,
                                 error_status,
                                 error_index,
                                 resp_oid,
                                 resp_oid_len,
                                 &resp_value,
                                 tx_buf,
                                 sizeof(tx_buf),
                                 &tx_len))
        {
            continue;
        }

        lwip_sendto(sock,
                    tx_buf,
                    tx_len,
                    0,
                    (struct sockaddr *)&src_addr,
                    src_len);
    }
}

esp_err_t snmp_agent_start(void)
{
    if (s_snmp_started)
    {
        return ESP_OK;
    }

    BaseType_t const task_ok = xTaskCreate(snmp_agent_task,
                                           "snmp_agent",
                                           UPS_SNMP_AGENT_TASK_STACK,
                                           NULL,
                                           UPS_SNMP_AGENT_TASK_PRIO,
                                           NULL);
    if (task_ok != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create SNMP task");
        return ESP_FAIL;
    }

    s_snmp_started = true;
    return ESP_OK;
}
