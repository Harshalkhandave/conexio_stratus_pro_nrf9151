/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_dect_phy.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/drivers/hwinfo.h>

LOG_MODULE_REGISTER(app);

BUILD_ASSERT(CONFIG_CARRIER, "Carrier must be configured according to local regulations");

struct sensor_payload {
    uint16_t node_id;
    uint32_t sequence;
    int16_t  temperature_x10;
    uint16_t humidity_x10;
    uint16_t battery_mv;
    uint32_t uptime_ms;         /* ms since node boot (for rejoin timing) */
    uint8_t  checksum;
} __attribute__((packed));

#define MAX_TRACKED_DEVICES 8
#define OFFLINE_THRESHOLD_MS   2000   /* declare offline after 2s silence */
#define OFFLINE_WARN_PERIOD_MS 10000  /* throttle "still offline" spam */

static bool exit;
static uint16_t device_id;
static uint64_t modem_time;
static uint16_t last_sender_id = 0;

/* Header type 1, due to endianness the order is different than in the specification. */
struct phy_ctrl_field_common {
	uint32_t packet_length : 4;
	uint32_t packet_length_type : 1;
	uint32_t header_format : 3;
	uint32_t short_network_id : 8;
	uint32_t transmitter_id_hi : 8;
	uint32_t transmitter_id_lo : 8;
	uint32_t df_mcs : 3;
	uint32_t reserved : 1;
	uint32_t transmit_power : 4;
	uint32_t pad : 24;
};

struct node_state {
    uint16_t id;                /* device ID                          */
    bool     active;            /* currently seen as online           */
    bool     ever_seen;         /* has this device been seen before   */
    uint64_t last_seen_ms;      /* uptime when last packet arrived    */
    uint64_t first_seen_ms;     /* uptime when first ever seen        */
    uint64_t offline_since_ms;  /* listener time of last packet before offline */
    uint64_t offline_declared_ms; /* listener time when marked OFFLINE */
    uint64_t last_offline_warn_ms;
    uint32_t rx_count;          /* total packets received from device */
    uint32_t cs_fail_count;     /* checksum failures from device      */
    uint32_t rejoin_count;      /* how many times rejoined            */
};
static struct node_state devices[MAX_TRACKED_DEVICES];
static int device_count = 0;

/* Semaphore to synchronize modem calls. */
K_SEM_DEFINE(operation_sem, 0, 1);

K_SEM_DEFINE(deinit_sem, 0, 1);

/* ── Checksum helper ────────────────────────────────────────────── */
static uint8_t calc_checksum(const void *data, size_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    uint8_t cs = 0;
    for (size_t i = 0; i < len; i++) {
        cs ^= b[i];
    }
    return cs;
}

/* ── Find or create device entry ────────────────────────────────── */
static struct node_state *get_device(uint16_t id)
{
    /* Search existing */
    for (int i = 0; i < device_count; i++) {
        if (devices[i].id == id) {
            return &devices[i];
        }
    }
    /* Create new entry */
    if (device_count < MAX_TRACKED_DEVICES) {
        struct node_state *d = &devices[device_count++];
        memset(d, 0, sizeof(*d));
        d->id = id;
        return d;
    }
    return NULL;
}
/* Callback after init operation. */
static void on_init(const struct nrf_modem_dect_phy_init_event *evt)
{
	if (evt->err) {
		LOG_ERR("Init failed, err %d", evt->err);
		exit = true;
		return;
	}

	k_sem_give(&operation_sem);
}

/* Callback after deinit operation. */
static void on_deinit(const struct nrf_modem_dect_phy_deinit_event *evt)
{
	if (evt->err) {
		LOG_ERR("Deinit failed, err %d", evt->err);
		return;
	}

	k_sem_give(&deinit_sem);
}

static void on_activate(const struct nrf_modem_dect_phy_activate_event *evt)
{
	if (evt->err) {
		LOG_ERR("Activate failed, err %d", evt->err);
		exit = true;
		return;
	}

	k_sem_give(&operation_sem);
}

static void on_deactivate(const struct nrf_modem_dect_phy_deactivate_event *evt)
{

	if (evt->err) {
		LOG_ERR("Deactivate failed, err %d", evt->err);
		return;
	}

	k_sem_give(&deinit_sem);
}

static void on_configure(const struct nrf_modem_dect_phy_configure_event *evt)
{
	if (evt->err) {
		LOG_ERR("Configure failed, err %d", evt->err);
		return;
	}

	k_sem_give(&operation_sem);
}

/* Callback after link configuration operation. */
static void on_link_config(const struct nrf_modem_dect_phy_link_config_event *evt)
{
	LOG_DBG("link_config cb time %"PRIu64" status %d", modem_time, evt->err);
}

static void on_radio_config(const struct nrf_modem_dect_phy_radio_config_event *evt)
{
	if (evt->err) {
		LOG_ERR("Radio config failed, err %d", evt->err);
		return;
	}

	k_sem_give(&operation_sem);
}

/* Callback after capability get operation. */
static void on_capability_get(const struct nrf_modem_dect_phy_capability_get_event *evt)
{
	LOG_DBG("capability_get cb time %"PRIu64" status %d", modem_time, evt->err);
}

static void on_bands_get(const struct nrf_modem_dect_phy_band_get_event *evt)
{
	LOG_DBG("bands_get cb status %d", evt->err);
}

static void on_latency_info_get(const struct nrf_modem_dect_phy_latency_info_event *evt)
{
	LOG_DBG("latency_info_get cb status %d", evt->err);
}

/* Callback after time query operation. */
static void on_time_get(const struct nrf_modem_dect_phy_time_get_event *evt)
{
	LOG_DBG("time_get cb time %"PRIu64" status %d", modem_time, evt->err);
}

static void on_cancel(const struct nrf_modem_dect_phy_cancel_event *evt)
{
	LOG_DBG("on_cancel cb status %d", evt->err);
	k_sem_give(&operation_sem);
}

/* Operation complete notification. */
static void on_op_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
	LOG_DBG("op_complete cb time %"PRIu64" status %d", modem_time, evt->err);
	k_sem_give(&operation_sem);
}

/* Physical Control Channel reception notification. */
static void on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
    last_sender_id = (evt->hdr.hdr_type_1.transmitter_id_hi << 8) |
                      evt->hdr.hdr_type_1.transmitter_id_lo;
    LOG_DBG("PCC from device %d", last_sender_id);
}

/* Physical Control Channel CRC error notification. */
static void on_pcc_crc_err(const struct nrf_modem_dect_phy_pcc_crc_failure_event *evt)
{
	LOG_DBG("pcc_crc_err cb time %"PRIu64"", modem_time);
}


/* ── PDC callback — heart of the listener ───────────────────────── */
static void on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{
    uint64_t now = k_uptime_get();
 
    /* Get sender ID from PHY header */
    uint16_t sender_id = last_sender_id;
 
    /* Verify payload size */
    if (evt->len < sizeof(struct sensor_payload)) {
        LOG_WRN("T=%lld | FROM:%d | Short packet (%d bytes) — ignored",
                now, sender_id, evt->len);
        return;
    }
 
    const struct sensor_payload *pkt =
        (const struct sensor_payload *)evt->data;
 
    /* ── Checksum validation ──────────────────────────────────── */
    uint8_t expected_cs = calc_checksum(pkt, sizeof(*pkt) - 1);
    bool cs_ok = (pkt->checksum == expected_cs);
 
    /* ── Device state tracking ────────────────────────────────── */
    struct node_state *d = get_device(sender_id);
    if (d == NULL) {
        LOG_WRN("Device table full — cannot track device %d", sender_id);
        return;
    }
 
    if (!d->ever_seen) {
        /* First time we ever see this device */
        d->ever_seen     = true;
        d->active        = true;
        d->first_seen_ms = now;
        d->last_seen_ms  = now;
        LOG_INF("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        LOG_INF("NEW DEVICE DISCOVERED: %d", sender_id);
        LOG_INF("   Node uptime at first hear: %u ms", pkt->uptime_ms);
        LOG_INF("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        /* boot_to_heard_ms = how long after NODE boot until listener heard it */
        LOG_INF("METRIC,event=discover,id=%u,boot_to_heard_ms=%u,seq=%u,rssi_x2=%d",
                sender_id, pkt->uptime_ms, pkt->sequence, evt->rssi_2);
    } else if (!d->active) {
        /* Device was offline — now it's back */
        uint64_t silence_gap_ms = now - d->offline_since_ms;
        uint64_t offline_to_rejoin_ms = now - d->offline_declared_ms;

        d->active = true;
        d->rejoin_count++;

        LOG_INF("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        LOG_INF("DEVICE %d REJOINED!", sender_id);
        LOG_INF("   Silence gap (last RX -> now) : %lld ms", silence_gap_ms);
        LOG_INF("   Offline flag -> rejoin       : %lld ms", offline_to_rejoin_ms);
        LOG_INF("   Node boot -> heard (AUTO)    : %u ms", pkt->uptime_ms);
        LOG_INF("   First packet SEQ             : %u", pkt->sequence);
        LOG_INF("   Rejoin #                     : %u", d->rejoin_count);
        LOG_INF("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        /*
         * boot_to_heard_ms is the report metric you want:
         * time from node power-on until listener received first packet.
         * No stopwatch needed — node puts k_uptime_get() in the packet.
         */
        LOG_INF("METRIC,event=rejoin,id=%u,boot_to_heard_ms=%u,silence_gap_ms=%llu,"
                "offline_to_rejoin_ms=%llu,seq=%u,rejoin_n=%u,rssi_x2=%d",
                sender_id, pkt->uptime_ms,
                (unsigned long long)silence_gap_ms,
                (unsigned long long)offline_to_rejoin_ms,
                pkt->sequence, d->rejoin_count, evt->rssi_2);
    }
 
    d->last_seen_ms = now;
    d->rx_count++;
 
    if (!cs_ok) {
        d->cs_fail_count++;
    }
 
    /* ── Print reception log ─────────────────────────────────── */
    LOG_INF("─────────────────────────────────────────────");
    LOG_INF("T=%lld ms | FROM: %d | SEQ: %u | node_uptime: %u ms",
            now, sender_id, pkt->sequence, pkt->uptime_ms);
    LOG_INF("  Temp     : %d.%d °C",
            pkt->temperature_x10 / 10,
            pkt->temperature_x10 < 0 ?
                -(pkt->temperature_x10 % 10) : (pkt->temperature_x10 % 10));
    LOG_INF("  Humidity : %d.%d %%",
            pkt->humidity_x10 / 10,
            pkt->humidity_x10 % 10);
    LOG_INF("  Battery  : %d mV", pkt->battery_mv);
    LOG_INF("  RSSI     : %d.%d dBm",
            evt->rssi_2 / 2,
            (evt->rssi_2 & 1) * 5);
    LOG_INF("  Checksum : 0x%02X %s",
            pkt->checksum,
            cs_ok ? "PASS" : "FAIL");
    LOG_INF("  RX count : %d from this device", d->rx_count);
    LOG_INF("─────────────────────────────────────────────");
}

/* Physical Data Channel CRC error notification. */
static void on_pdc_crc_err(const struct nrf_modem_dect_phy_pdc_crc_failure_event *evt)
{
	LOG_DBG("pdc_crc_err cb time %"PRIu64"", modem_time);
}

/* RSSI measurement result notification. */
static void on_rssi(const struct nrf_modem_dect_phy_rssi_event *evt)
{
	LOG_DBG("rssi cb time %"PRIu64" carrier %d", modem_time, evt->carrier);
}

static void on_stf_cover_seq_control(const struct nrf_modem_dect_phy_stf_control_event *evt)
{
	LOG_WRN("Unexpectedly in %s\n", (__func__));
}

static void on_test_rf_tx_cw_ctrl(const struct nrf_modem_dect_phy_test_rf_tx_cw_control_event *evt)
{
	LOG_WRN("Unexpectedly in %s\n", (__func__));
}

static void dect_phy_event_handler(const struct nrf_modem_dect_phy_event *evt)
{
	modem_time = evt->time;

	switch (evt->id) {
	case NRF_MODEM_DECT_PHY_EVT_INIT:
		on_init(&evt->init);
		break;
	case NRF_MODEM_DECT_PHY_EVT_DEINIT:
		on_deinit(&evt->deinit);
		break;
	case NRF_MODEM_DECT_PHY_EVT_ACTIVATE:
		on_activate(&evt->activate);
		break;
	case NRF_MODEM_DECT_PHY_EVT_DEACTIVATE:
		on_deactivate(&evt->deactivate);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CONFIGURE:
		on_configure(&evt->configure);
		break;
	case NRF_MODEM_DECT_PHY_EVT_RADIO_CONFIG:
		on_radio_config(&evt->radio_config);
		break;
	case NRF_MODEM_DECT_PHY_EVT_COMPLETED:
		on_op_complete(&evt->op_complete);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CANCELED:
		on_cancel(&evt->cancel);
		break;
	case NRF_MODEM_DECT_PHY_EVT_RSSI:
		on_rssi(&evt->rssi);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC:
		on_pcc(&evt->pcc);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC_ERROR:
		on_pcc_crc_err(&evt->pcc_crc_err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PDC:
		on_pdc(&evt->pdc);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PDC_ERROR:
		on_pdc_crc_err(&evt->pdc_crc_err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_TIME:
		on_time_get(&evt->time_get);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CAPABILITY:
		on_capability_get(&evt->capability_get);
		break;
	case NRF_MODEM_DECT_PHY_EVT_BANDS:
		on_bands_get(&evt->band_get);
		break;
	case NRF_MODEM_DECT_PHY_EVT_LATENCY:
		on_latency_info_get(&evt->latency_get);
		break;
	case NRF_MODEM_DECT_PHY_EVT_LINK_CONFIG:
		on_link_config(&evt->link_config);
		break;
	case NRF_MODEM_DECT_PHY_EVT_STF_CONFIG:
		on_stf_cover_seq_control(&evt->stf_cover_seq_control);
		break;
	case NRF_MODEM_DECT_PHY_EVT_TEST_RF_TX_CW_CONTROL_CONFIG:
		on_test_rf_tx_cw_ctrl(&evt->test_rf_tx_cw_control);
		break;
	}
}

/* Dect PHY config parameters. */
static struct nrf_modem_dect_phy_config_params dect_phy_config_params = {
	.band_group_index = ((CONFIG_CARRIER >= 525 && CONFIG_CARRIER <= 551)) ? 1 : 0,
	.harq_rx_process_count = 4,
	.harq_rx_expiry_time_us = 5000000,
};

/* Send operation. */
static int transmit(uint32_t handle, void *data, size_t data_len)
{
	int err;

	struct phy_ctrl_field_common header = {
		.header_format = 0x0,
		.packet_length_type = 0x0,
		.packet_length = 0x01,
		.short_network_id = (CONFIG_NETWORK_ID & 0xff),
		.transmitter_id_hi = (device_id >> 8),
		.transmitter_id_lo = (device_id & 0xff),
		.transmit_power = CONFIG_TX_POWER,
		.reserved = 0,
		.df_mcs = CONFIG_MCS,
	};

	struct nrf_modem_dect_phy_tx_params tx_op_params = {
		.start_time = 0,
		.handle = handle,
		.network_id = CONFIG_NETWORK_ID,
		.phy_type = 0,
		.lbt_rssi_threshold_max = 0,
		.carrier = CONFIG_CARRIER,
		.lbt_period = NRF_MODEM_DECT_LBT_PERIOD_MAX,
		.phy_header = (union nrf_modem_dect_phy_hdr *)&header,
		.data = data,
		.data_size = data_len,
	};

	err = nrf_modem_dect_phy_tx(&tx_op_params);
	if (err != 0) {
		return err;
	}

	return 0;
}

/* Receive operation. */
static int receive(uint32_t handle)
{
	int err;

	struct nrf_modem_dect_phy_rx_params rx_op_params = {
		.start_time = 0,
		.handle = handle,
		.network_id = CONFIG_NETWORK_ID,
		.mode = NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS,
		.rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
		.link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
		.rssi_level = -60,
		.carrier = CONFIG_CARRIER,
		.duration = CONFIG_RX_PERIOD_S * MSEC_PER_SEC *
			    NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ,
		.filter.short_network_id = CONFIG_NETWORK_ID & 0xff,
		.filter.is_short_network_id_used = 1,
		/* listen for everything (broadcast mode used) */
		.filter.receiver_identity = 0,
	};

	err = nrf_modem_dect_phy_rx(&rx_op_params);
	if (err != 0) {
		return err;
	}

	return 0;
}

static void monitor_fn(void *p1, void *p2, void *p3)
{
    while (1) {
        k_sleep(K_MSEC(500));
 
        uint64_t now = k_uptime_get();
 
        for (int i = 0; i < device_count; i++) {
            struct node_state *d = &devices[i];
 
            if (!d->ever_seen) {
                continue;
            }
 
            uint64_t gap = now - d->last_seen_ms;
 
            if (d->active && gap > OFFLINE_THRESHOLD_MS) {
                /* Transition: active → offline */
                d->active = false;
                d->offline_since_ms = d->last_seen_ms;
                d->offline_declared_ms = now;
                d->last_offline_warn_ms = now;

                LOG_WRN("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
                LOG_WRN("DEVICE %d OFFLINE", d->id);
                LOG_WRN("   Last packet      : T=%lld ms", d->last_seen_ms);
                LOG_WRN("   Silence before flag: %lld ms", gap);
                LOG_WRN("   Total RX count   : %d", d->rx_count);
                LOG_WRN("   CS failures      : %d", d->cs_fail_count);
                LOG_WRN("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
                LOG_WRN("METRIC,event=offline,id=%u,silence_before_flag_ms=%llu,"
                        "last_rx_ms=%llu,rx_count=%u",
                        d->id,
                        (unsigned long long)gap,
                        (unsigned long long)d->last_seen_ms,
                        d->rx_count);

            } else if (!d->active &&
                       (now - d->last_offline_warn_ms) >= OFFLINE_WARN_PERIOD_MS) {
                d->last_offline_warn_ms = now;
                LOG_WRN("DEVICE %d still offline — %lld ms since last packet",
                        d->id, gap);
            }
        }
    }
}
 
K_THREAD_DEFINE(monitor_tid, 2048, monitor_fn, NULL, NULL, NULL, 5, 0, 0);
 
/* ── Summary print thread — prints stats every 30s ─────────────── */
static void summary_fn(void *p1, void *p2, void *p3)
{
    while (1) {
        k_sleep(K_SECONDS(30));
 
        LOG_INF("══════════════ NETWORK SUMMARY ══════════════");
        LOG_INF("Uptime: %lld ms", k_uptime_get());
        LOG_INF("Tracked devices: %d", device_count);
 
        for (int i = 0; i < device_count; i++) {
            struct node_state *d = &devices[i];
            LOG_INF("  Device %d: %s | RX:%d | CS_FAIL:%d | REJOINS:%d",
                    d->id,
                    d->active ? "ONLINE" : "OFFLINE",
                    d->rx_count,
                    d->cs_fail_count,
                    d->rejoin_count);
        }
        LOG_INF("═════════════════════════════════════════════");
    }
}
 
K_THREAD_DEFINE(summary_tid, 1024, summary_fn, NULL, NULL, NULL, 6, 0, 0);

int main(void)
{
	int err;
	uint32_t rx_handle = 0;

	LOG_INF("===========================================");
    LOG_INF("  DECT NR+ Listener starting...");
    LOG_INF("  Channel: %d  Network: 0x%08X",
            CONFIG_CARRIER, CONFIG_NETWORK_ID);
    LOG_INF("  Offline threshold: %d ms", OFFLINE_THRESHOLD_MS);
    LOG_INF("  Auto metric: boot_to_heard_ms (no stopwatch)");
    LOG_INF("===========================================");

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("modem init failed, err %d", err);
		return err;
	}

	err = nrf_modem_dect_phy_event_handler_set(dect_phy_event_handler);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_event_handler_set failed, err %d", err);
		return err;
	}

	err = nrf_modem_dect_phy_init();
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_init failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (exit) {
		return -EIO;
	}

	err = nrf_modem_dect_phy_configure(&dect_phy_config_params);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_configure failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (exit) {
		return -EIO;
	}

	err = nrf_modem_dect_phy_activate(NRF_MODEM_DECT_PHY_RADIO_MODE_LOW_LATENCY);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_activate failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (exit) {
		return -EIO;
	}

	hwinfo_get_device_id((void *)&device_id, sizeof(device_id));

	LOG_INF("Listener ID: %d — waiting for broadcasters...", device_id);

	err = nrf_modem_dect_phy_capability_get();
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_capability_get failed, err %d", err);
	}

	while (1) {
		err = receive(rx_handle);
		if (err) {
            LOG_ERR("RX open failed %d — retrying in 1s", err);
            k_sleep(K_SECONDS(1));
            continue;
        }
    	k_sem_take(&operation_sem, K_FOREVER);
	}

	LOG_INF("Shutting down");

	err = nrf_modem_dect_phy_deactivate();
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_deactivate failed, err %d", err);
		return err;
	}

	k_sem_take(&deinit_sem, K_FOREVER);

	err = nrf_modem_dect_phy_deinit();
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_deinit() failed, err %d", err);
		return err;
	}

	k_sem_take(&deinit_sem, K_FOREVER);

	err = nrf_modem_lib_shutdown();
	if (err) {
		LOG_ERR("nrf_modem_lib_shutdown() failed, err %d", err);
		return err;
	}

	LOG_INF("Bye!");

	return 0;
}
