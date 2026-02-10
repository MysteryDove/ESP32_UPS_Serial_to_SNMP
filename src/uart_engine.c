#include "uart_engine.h"

#include "main.h"

#include <string.h>

#ifndef UART_ENGINE_QUEUE_SIZE
#define UART_ENGINE_QUEUE_SIZE 32U
#endif

#ifndef UART_ENGINE_MAX_EXPECTED_LEN
#define UART_ENGINE_MAX_EXPECTED_LEN 256U
#endif

#ifndef UART_ENGINE_TX_TIMEOUT_MS
#define UART_ENGINE_TX_TIMEOUT_MS 250U
#endif

#ifndef UART_ENGINE_RETRY_COOLDOWN_MS
#define UART_ENGINE_RETRY_COOLDOWN_MS 25U
#endif

#ifndef UART_ENGINE_MAX_STEPS_PER_TICK
#define UART_ENGINE_MAX_STEPS_PER_TICK 8U
#endif

typedef enum
{
    UART_ENGINE_STATE_IDLE = 0,
    UART_ENGINE_STATE_TX_START,
    UART_ENGINE_STATE_TX_WAIT,
    UART_ENGINE_STATE_RX_WAIT,
    UART_ENGINE_STATE_PROCESS,
} uart_engine_state_t;

typedef struct
{
    uart_engine_request_t req;
    uint8_t retries_left;
    bool is_heartbeat;
} uart_engine_job_t;

static uart_engine_job_t s_queue[UART_ENGINE_QUEUE_SIZE];
static uint8_t s_q_head;
static uint8_t s_q_tail;
static uint8_t s_q_count;

static uart_engine_job_t s_active;
static uart_engine_state_t s_state;
static uint32_t s_state_start_ms;
static uint32_t s_retry_not_before_ms;

static uint8_t s_rx_buf[UART_ENGINE_MAX_EXPECTED_LEN];
static uint16_t s_rx_got;
static uint8_t s_tx_buf[8U];

static bool s_enabled;

static bool s_hb_enabled;
static uart_engine_heartbeat_cfg_t s_hb_cfg;
static uint32_t s_hb_next_due_ms;
static uint8_t s_hb_consecutive_failures;
static bool s_hb_queued_or_active;

static uint32_t engine_now_ms(void)
{
    return ups_tick_ms();
}

static void set_not_before_ms(uint32_t candidate_ms)
{
    if ((int32_t)(candidate_ms - s_retry_not_before_ms) > 0)
    {
        s_retry_not_before_ms = candidate_ms;
    }
}

static void apply_interjob_cooldown(uint32_t now_ms)
{
#if (UART_ENGINE_INTERJOB_COOLDOWN_MS > 0U)
    set_not_before_ms(now_ms + UART_ENGINE_INTERJOB_COOLDOWN_MS);
#else
    (void)now_ms;
#endif
}

static void uart_engine_debug_print_raw_rx(const char *reason, const uint8_t *rx, uint16_t rx_len)
{
    if (!g_ups_debug_status_print_enabled)
    {
        return;
    }

    printf("UART_ENG raw rx: %s len=%u",
           (reason != NULL) ? reason : "unknown",
           (unsigned int)rx_len);

    if ((rx == NULL) || (rx_len == 0U))
    {
        printf(" (empty)\r\n");
        return;
    }

    printf(" data=");
    for (uint16_t i = 0U; i < rx_len; i++)
    {
        printf("%02X", rx[i]);
        if ((uint16_t)(i + 1U) < rx_len)
        {
            printf(" ");
        }
    }
    printf("\r\n");
}

static void uart_engine_debug_print_failure(const uart_engine_job_t *job, const char *reason)
{
    if (!g_ups_debug_status_print_enabled || (job == NULL))
    {
        return;
    }

    printf("UART_ENG failure: %s cmd=0x%04X hb=%u retries_left=%u q=%u\r\n",
           (reason != NULL) ? reason : "unknown",
           (unsigned int)job->req.cmd,
           job->is_heartbeat ? 1U : 0U,
           (unsigned int)job->retries_left,
           (unsigned int)s_q_count);
}

static void uart_engine_debug_print_retry(const uart_engine_job_t *job, const char *reason)
{
    if (!g_ups_debug_status_print_enabled || (job == NULL))
    {
        return;
    }

    printf("UART_ENG retry: %s cmd=0x%04X hb=%u retries_left=%u q=%u\r\n",
           (reason != NULL) ? reason : "unknown",
           (unsigned int)job->req.cmd,
           job->is_heartbeat ? 1U : 0U,
           (unsigned int)job->retries_left,
           (unsigned int)s_q_count);
}

static void uart_engine_debug_print_timeout(const uart_engine_job_t *job,
                                            const char *phase,
                                            uint32_t elapsed_ms,
                                            uint32_t timeout_ms)
{
    if (!g_ups_debug_status_print_enabled || (job == NULL))
    {
        return;
    }

    printf("UART_ENG timeout: %s cmd=0x%04X hb=%u elapsed=%lu timeout=%lu retries_left=%u\r\n",
           (phase != NULL) ? phase : "unknown",
           (unsigned int)job->req.cmd,
           job->is_heartbeat ? 1U : 0U,
           (unsigned long)elapsed_ms,
           (unsigned long)timeout_ms,
           (unsigned int)job->retries_left);
}

static bool queue_is_full(void)
{
    return (s_q_count >= UART_ENGINE_QUEUE_SIZE);
}

static bool queue_push(const uart_engine_request_t *req, bool is_heartbeat)
{
    if ((req == NULL) || queue_is_full())
    {
        return false;
    }

    s_queue[s_q_tail].req = *req;
    s_queue[s_q_tail].retries_left = req->max_retries;
    s_queue[s_q_tail].is_heartbeat = is_heartbeat;

    s_q_tail = (uint8_t)((s_q_tail + 1U) % UART_ENGINE_QUEUE_SIZE);
    s_q_count++;
    return true;
}

static bool queue_pop(uart_engine_job_t *out)
{
    if ((out == NULL) || (s_q_count == 0U))
    {
        return false;
    }

    *out = s_queue[s_q_head];
    s_q_head = (uint8_t)((s_q_head + 1U) % UART_ENGINE_QUEUE_SIZE);
    s_q_count--;
    return true;
}

static uint16_t build_cmd_bytes(uint8_t *tx, uint16_t tx_cap, uint16_t cmd, uint8_t cmd_bits)
{
    if ((tx == NULL) || (tx_cap == 0U))
    {
        return 0U;
    }

    if (cmd_bits == 8U)
    {
        if (tx_cap < 1U)
        {
            return 0U;
        }
        tx[0] = (uint8_t)(cmd & 0xFFU);
        return 1U;
    }

    if (cmd_bits == 16U)
    {
        if (tx_cap < 2U)
        {
            return 0U;
        }
        tx[0] = (uint8_t)((cmd >> 8) & 0xFFU);
        tx[1] = (uint8_t)(cmd & 0xFFU);
        return 2U;
    }

    return 0U;
}

static bool request_is_valid(const uart_engine_request_t *req)
{
    if (req == NULL)
    {
        return false;
    }

    if ((req->cmd_bits != 8U) && (req->cmd_bits != 16U))
    {
        return false;
    }

    if (req->expected_len > UART_ENGINE_MAX_EXPECTED_LEN)
    {
        return false;
    }

    if (req->expected_ending)
    {
        if ((req->expected_ending_len == 0U) || (req->expected_ending_len > UART_ENGINE_MAX_ENDING_LEN))
        {
            return false;
        }
    }

    return true;
}

static uint16_t request_rx_cap(const uart_engine_request_t *req)
{
    if (req == NULL)
    {
        return 0U;
    }

    if (!req->expected_ending)
    {
        return req->expected_len;
    }

    if (req->expected_len == 0U)
    {
        return UART_ENGINE_MAX_EXPECTED_LEN;
    }

    return req->expected_len;
}

static bool rx_has_expected_ending(const uart_engine_request_t *req, const uint8_t *rx, uint16_t rx_len)
{
    if ((req == NULL) || !req->expected_ending)
    {
        return false;
    }

    uint8_t const ending_len = req->expected_ending_len;
    if ((ending_len == 0U) || (ending_len > UART_ENGINE_MAX_ENDING_LEN) || (rx == NULL) || (rx_len < ending_len))
    {
        return false;
    }

    return (memcmp(&rx[rx_len - ending_len], req->expected_ending_bytes, ending_len) == 0);
}

static void active_clear(void)
{
    (void)memset(&s_active, 0, sizeof(s_active));
    s_rx_got = 0U;
}

static void on_job_success(const uart_engine_job_t *job)
{
    if ((job != NULL) && job->is_heartbeat)
    {
        s_hb_consecutive_failures = 0U;
    }
}

static void on_job_final_failure(const uart_engine_job_t *job)
{
    if ((job == NULL) || !job->is_heartbeat)
    {
        return;
    }

    if (s_hb_consecutive_failures < 255U)
    {
        s_hb_consecutive_failures++;
    }

    uint8_t threshold = s_hb_cfg.failure_threshold;
    if (threshold == 0U)
    {
        threshold = 5U;
    }

    if (s_hb_consecutive_failures >= threshold)
    {
        g_battery.remaining_capacity = 1U;
        g_battery.remaining_time_limit_s = 1U;
        g_power_summary_present_status.fully_charged = false;
        g_power_summary_present_status.below_remaining_capacity_limit = true;
        g_power_summary_present_status.shutdown_imminent = true;
        g_power_summary_present_status.charging = false;
        g_power_summary_present_status.discharging = true;
        g_power_summary_present_status.ac_present = false;
    }
}

void uart_engine_init(void)
{
    s_q_head = 0U;
    s_q_tail = 0U;
    s_q_count = 0U;

    s_state = UART_ENGINE_STATE_IDLE;
    s_state_start_ms = 0U;
    s_retry_not_before_ms = 0U;

    s_hb_enabled = false;
    (void)memset(&s_hb_cfg, 0, sizeof(s_hb_cfg));
    s_hb_next_due_ms = 0U;
    s_hb_consecutive_failures = 0U;
    s_hb_queued_or_active = false;

    s_enabled = true;
    active_clear();
}

static void uart_engine_reset_internal(void)
{
    s_q_head = 0U;
    s_q_tail = 0U;
    s_q_count = 0U;

    s_state = UART_ENGINE_STATE_IDLE;
    s_state_start_ms = 0U;
    s_retry_not_before_ms = 0U;

    s_hb_enabled = false;
    (void)memset(&s_hb_cfg, 0, sizeof(s_hb_cfg));
    s_hb_next_due_ms = 0U;
    s_hb_consecutive_failures = 0U;
    s_hb_queued_or_active = false;

    active_clear();
    UART2_Unlock();
}

void uart_engine_set_enabled(bool enable)
{
    if (enable == s_enabled)
    {
        return;
    }

    s_enabled = enable;
    if (!s_enabled)
    {
        uart_engine_reset_internal();
    }
}

bool uart_engine_is_enabled(void)
{
    return s_enabled;
}

bool uart_engine_is_busy(void)
{
    return (s_state != UART_ENGINE_STATE_IDLE) || (s_q_count != 0U);
}

uart_engine_result_t uart_engine_enqueue(const uart_engine_request_t *req)
{
    if (!s_enabled)
    {
        return UART_ENGINE_ERR_DISABLED;
    }

    if (!request_is_valid(req))
    {
        return UART_ENGINE_ERR_BAD_PARAM;
    }

    if (!queue_push(req, false))
    {
        return UART_ENGINE_ERR_QUEUE_FULL;
    }

    return UART_ENGINE_OK;
}

void uart_engine_set_heartbeat(const uart_engine_heartbeat_cfg_t *cfg)
{
    if (!s_enabled)
    {
        return;
    }

    if (cfg == NULL)
    {
        s_hb_enabled = false;
        s_hb_queued_or_active = false;
        s_hb_consecutive_failures = 0U;
        return;
    }

    s_hb_cfg = *cfg;
    if (!request_is_valid(&s_hb_cfg.req))
    {
        s_hb_enabled = false;
        return;
    }

    if (s_hb_cfg.failure_threshold == 0U)
    {
        s_hb_cfg.failure_threshold = 5U;
    }

    s_hb_enabled = true;
    s_hb_next_due_ms = engine_now_ms();
    s_hb_consecutive_failures = 0U;
    s_hb_queued_or_active = false;
}

bool uart_engine_process_expect_exact(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value)
{
    (void)cmd;

    const uart_engine_expect_bytes_t *exp = (const uart_engine_expect_bytes_t *)out_value;
    if ((exp == NULL) || (exp->expected == NULL))
    {
        return false;
    }

    if (rx_len != exp->expected_len)
    {
        return false;
    }

    if ((rx == NULL) && (rx_len != 0U))
    {
        return false;
    }

    return (memcmp(rx, exp->expected, rx_len) == 0);
}

static void maybe_enqueue_heartbeat(uint32_t now_ms)
{
    if (!s_hb_enabled || s_hb_queued_or_active)
    {
        return;
    }

    if ((int32_t)(now_ms - s_hb_next_due_ms) < 0)
    {
        return;
    }

    if (queue_push(&s_hb_cfg.req, true))
    {
        s_hb_queued_or_active = true;

        uint32_t interval = s_hb_cfg.interval_ms;
        if (interval == 0U)
        {
            interval = 1000U;
        }
        s_hb_next_due_ms = now_ms + interval;
    }
}

static void job_finish_failure(uint32_t now_ms, const char *reason)
{
    UART2_Unlock();

    if (s_active.retries_left > 0U)
    {
        s_active.retries_left--;
        if (queue_push(&s_active.req, s_active.is_heartbeat))
        {
            uart_engine_debug_print_retry(&s_active, reason);
            s_retry_not_before_ms = now_ms + UART_ENGINE_RETRY_COOLDOWN_MS;
        }
        else
        {
            uart_engine_debug_print_failure(&s_active, "retry enqueue failed");
            on_job_final_failure(&s_active);
            if (s_active.is_heartbeat)
            {
                s_hb_queued_or_active = false;
            }
        }
    }
    else
    {
        uart_engine_debug_print_failure(&s_active, reason);
        on_job_final_failure(&s_active);
        if (s_active.is_heartbeat)
        {
            s_hb_queued_or_active = false;
        }
    }

    s_state = UART_ENGINE_STATE_IDLE;
    apply_interjob_cooldown(now_ms);
    active_clear();
}

static void job_start_tx(uint32_t now_ms)
{
    uint16_t const tx_len = build_cmd_bytes(s_tx_buf,
                                            (uint16_t)sizeof(s_tx_buf),
                                            s_active.req.cmd,
                                            s_active.req.cmd_bits);
    if (tx_len == 0U)
    {
        job_finish_failure(now_ms, "build tx bytes failed");
        return;
    }

    UART2_DiscardBuffered();
    UART2_TxDoneClear();
    UPS_DebugPrintTxCommand(s_tx_buf, tx_len);

    if (UART2_SendBytesDMA(s_tx_buf, tx_len) != ESP_OK)
    {
        job_finish_failure(now_ms, "tx start failed");
        return;
    }

    s_state = UART_ENGINE_STATE_TX_WAIT;
    s_state_start_ms = now_ms;
}

void uart_engine_tick(void)
{
    if (!s_enabled)
    {
        return;
    }

    for (uint8_t step = 0U; step < UART_ENGINE_MAX_STEPS_PER_TICK; ++step)
    {
        uint32_t const now_ms = engine_now_ms();
        maybe_enqueue_heartbeat(now_ms);

        if ((int32_t)(now_ms - s_retry_not_before_ms) < 0)
        {
            return;
        }

        bool progressed = false;

        switch (s_state)
        {
        case UART_ENGINE_STATE_IDLE:
        {
            if (s_q_count == 0U)
            {
                return;
            }

            if (!UART2_TryLock())
            {
                return;
            }

            if (!queue_pop(&s_active))
            {
                UART2_Unlock();
                return;
            }

            s_state = UART_ENGINE_STATE_TX_START;
            s_state_start_ms = now_ms;
            if (s_active.is_heartbeat)
            {
                s_hb_queued_or_active = true;
            }
            progressed = true;
            break;
        }

        case UART_ENGINE_STATE_TX_START:
            job_start_tx(now_ms);
            progressed = true;
            break;

        case UART_ENGINE_STATE_TX_WAIT:
            if (UART2_TxDone())
            {
                s_state = UART_ENGINE_STATE_RX_WAIT;
                s_state_start_ms = now_ms;
                s_rx_got = 0U;
                progressed = true;
            }
            else if ((now_ms - s_state_start_ms) >= UART_ENGINE_TX_TIMEOUT_MS)
            {
                uart_engine_debug_print_timeout(&s_active,
                                                "tx wait",
                                                (uint32_t)(now_ms - s_state_start_ms),
                                                UART_ENGINE_TX_TIMEOUT_MS);
                job_finish_failure(now_ms, "tx timeout");
                progressed = true;
            }
            break;

        case UART_ENGINE_STATE_RX_WAIT:
        {
            uint16_t const rx_cap = request_rx_cap(&s_active.req);
            if (rx_cap == 0U)
            {
                s_state = UART_ENGINE_STATE_PROCESS;
                progressed = true;
                break;
            }

            if (s_rx_got < rx_cap)
            {
                uint16_t const want = (uint16_t)(rx_cap - s_rx_got);
                uint16_t const got = UART2_Read(&s_rx_buf[s_rx_got], want);
                if (got > 0U)
                {
                    s_rx_got = (uint16_t)(s_rx_got + got);
                    progressed = true;
                }
            }

            if (s_active.req.expected_ending)
            {
                if (rx_has_expected_ending(&s_active.req, s_rx_buf, s_rx_got))
                {
                    s_state = UART_ENGINE_STATE_PROCESS;
                    progressed = true;
                    break;
                }

                if (s_rx_got >= rx_cap)
                {
                    uart_engine_debug_print_failure(&s_active, "rx reached cap before ending");
                    uart_engine_debug_print_raw_rx("rx cap", s_rx_buf, s_rx_got);
                    job_finish_failure(now_ms, "rx ending not found");
                    progressed = true;
                    break;
                }
            }
            else if (s_rx_got >= rx_cap)
            {
                s_state = UART_ENGINE_STATE_PROCESS;
                progressed = true;
                break;
            }

            if ((now_ms - s_state_start_ms) >= s_active.req.timeout_ms)
            {
                uart_engine_debug_print_timeout(&s_active,
                                                "rx wait",
                                                (uint32_t)(now_ms - s_state_start_ms),
                                                s_active.req.timeout_ms);
                uart_engine_debug_print_raw_rx("rx timeout", s_rx_buf, s_rx_got);
                job_finish_failure(now_ms, "rx timeout");
                progressed = true;
            }
            break;
        }

        case UART_ENGINE_STATE_PROCESS:
        {
            bool ok = true;
            if (s_active.req.process_fn != NULL)
            {
                ok = s_active.req.process_fn(s_active.req.cmd,
                                             s_rx_buf,
                                             s_rx_got,
                                             s_active.req.out_value);
            }

            UART2_Unlock();

            if (ok)
            {
                on_job_success(&s_active);
                if (s_active.is_heartbeat)
                {
                    s_hb_queued_or_active = false;
                }

                s_state = UART_ENGINE_STATE_IDLE;
                apply_interjob_cooldown(now_ms);
                active_clear();
                progressed = true;
                break;
            }

            uart_engine_debug_print_raw_rx("process callback returned false", s_rx_buf, s_rx_got);
            if (s_active.is_heartbeat)
            {
                s_hb_queued_or_active = false;
            }

            if (s_active.retries_left > 0U)
            {
                s_active.retries_left--;
                if (queue_push(&s_active.req, s_active.is_heartbeat))
                {
                    uart_engine_debug_print_retry(&s_active, "process callback returned false");
                    s_retry_not_before_ms = now_ms + UART_ENGINE_RETRY_COOLDOWN_MS;
                }
                else
                {
                    uart_engine_debug_print_failure(&s_active, "parse failed and retry enqueue failed");
                    on_job_final_failure(&s_active);
                }
            }
            else
            {
                uart_engine_debug_print_failure(&s_active, "process callback returned false");
                on_job_final_failure(&s_active);
            }

            s_state = UART_ENGINE_STATE_IDLE;
            apply_interjob_cooldown(now_ms);
            active_clear();
            progressed = true;
            break;
        }

        default:
            s_state = UART_ENGINE_STATE_IDLE;
            active_clear();
            progressed = true;
            break;
        }

        if (!progressed)
        {
            break;
        }
    }
}
