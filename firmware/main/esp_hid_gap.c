/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */


#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_hid_gap.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "host/ble_gap.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_adv.h"
#include "services/gap/ble_svc_gap.h"
#include "nimble/ble.h"
#include "host/ble_sm.h"
#else
#include "esp_bt_device.h"
#endif

static const char *TAG = "ESP_HID_GAP";

#if CONFIG_BT_NIMBLE_ENABLED
static uint16_t s_active_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint32_t s_connection_generation;
static uint32_t s_connection_update_scheduled_generation;

static void log_connection_parameters(const char *phase, uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        ESP_LOGW(TAG,
                 "BLE connection parameters unavailable: phase=%s conn_handle=%u rc=%d",
                 phase, conn_handle, rc);
        return;
    }

    uint32_t interval_us = (uint32_t)desc.conn_itvl * 1250U;
    uint32_t rate_milli_hz = interval_us == 0
                                 ? 0
                                 : (uint32_t)(1000000000ULL / interval_us);
    ESP_LOGI(TAG,
             "BLE connection parameters: phase=%s conn_handle=%u "
             "interval=%" PRIu32 " us rate=%" PRIu32 ".%03" PRIu32
             " Hz latency=%u timeout=%u ms",
             phase,
             conn_handle,
             interval_us,
             rate_milli_hz / 1000U,
             rate_milli_hz % 1000U,
             desc.conn_latency,
             desc.supervision_timeout * 10);
}

static void verify_low_latency_connection_parameters(const struct ble_gap_conn_desc *desc)
{
    const uint16_t target_min = BLE_GAP_CONN_ITVL_MS(7.5);
    const uint16_t target_max = BLE_GAP_CONN_ITVL_MS(10);
    uint32_t interval_us = (uint32_t)desc->conn_itvl * 1250U;

    if (desc->conn_itvl >= target_min && desc->conn_itvl <= target_max &&
        desc->conn_latency == 0) {
        ESP_LOGI(TAG,
                 "low-latency connection parameters confirmed: interval=%" PRIu32
                 " us latency=0",
                 interval_us);
        return;
    }

    ESP_LOGW(TAG,
             "negotiated connection parameters outside target 7.5-10 ms, latency=0: "
             "interval=%" PRIu32 " us latency=%u",
             interval_us, desc->conn_latency);
}

static void request_narrow_connection_interval(void *argument)
{
    const struct ble_gap_upd_params params = {
        .itvl_min = BLE_GAP_CONN_ITVL_MS(7.5),
        .itvl_max = BLE_GAP_CONN_ITVL_MS(10),
        .latency = 0,
        .supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(5000),
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    uint32_t generation = (uint32_t)(uintptr_t)argument;

    vTaskDelay(pdMS_TO_TICKS(500));
    for (int attempt = 1; attempt <= 30; attempt++) {
        if (generation != s_connection_generation ||
            s_active_connection_handle == BLE_HS_CONN_HANDLE_NONE) {
            break;
        }
        int rc = ble_gap_update_params(s_active_connection_handle, &params);
        if (rc == 0) {
            ESP_LOGI(TAG,
                     "submitted delayed connection interval 7.5-10 ms, latency=0; "
                     "attempt=%d generation=%" PRIu32,
                     attempt, generation);
            break;
        }
        if (rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG,
                     "delayed narrow connection update failed; rc=%d attempt=%d "
                     "generation=%" PRIu32,
                     rc, attempt, generation);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

static void schedule_narrow_connection_interval(uint32_t generation, const char *source)
{
    if (s_connection_update_scheduled_generation == generation) {
        ESP_LOGI(TAG,
                 "skip duplicate low-latency connection update; source=%s "
                 "generation=%" PRIu32,
                 source, generation);
        return;
    }

    s_connection_update_scheduled_generation = generation;
    if (xTaskCreate(request_narrow_connection_interval,
                    "ble_conn_narrow", 3072,
                    (void *)(uintptr_t)generation, 5, NULL) != pdPASS) {
        s_connection_update_scheduled_generation = 0;
        ESP_LOGW(TAG,
                 "create delayed narrow connection task failed; source=%s "
                 "generation=%" PRIu32,
                 source, generation);
        return;
    }
    ESP_LOGI(TAG,
             "scheduled low-latency connection update; source=%s "
             "target=7.5-10 ms latency=0 generation=%" PRIu32,
             source, generation);
}
#endif

// uncomment to print all devices that were seen during a scan
#define GAP_DBG_PRINTF(...) //printf(__VA_ARGS__)
//static const char * gap_bt_prop_type_names[5] = {"","BDNAME","COD","RSSI","EIR"};

#if !CONFIG_BT_NIMBLE_ENABLED
static esp_hid_scan_result_t *bt_scan_results = NULL;
static size_t num_bt_scan_results = 0;

static esp_hid_scan_result_t *ble_scan_results = NULL;
static size_t num_ble_scan_results = 0;
#endif

static SemaphoreHandle_t bt_hidh_cb_semaphore = NULL;
#define WAIT_BT_CB() xSemaphoreTake(bt_hidh_cb_semaphore, portMAX_DELAY)
#define SEND_BT_CB() xSemaphoreGive(bt_hidh_cb_semaphore)

static SemaphoreHandle_t ble_hidh_cb_semaphore = NULL;
#define WAIT_BLE_CB() xSemaphoreTake(ble_hidh_cb_semaphore, portMAX_DELAY)
#define SEND_BLE_CB() xSemaphoreGive(ble_hidh_cb_semaphore)

#define SIZEOF_ARRAY(a) (sizeof(a)/sizeof(*a))

#if !CONFIG_BT_NIMBLE_ENABLED
static const char *ble_gap_evt_names[] = { "ADV_DATA_SET_COMPLETE", "SCAN_RSP_DATA_SET_COMPLETE", "SCAN_PARAM_SET_COMPLETE", "SCAN_RESULT", "ADV_DATA_RAW_SET_COMPLETE", "SCAN_RSP_DATA_RAW_SET_COMPLETE", "ADV_START_COMPLETE", "SCAN_START_COMPLETE", "AUTH_CMPL", "KEY", "SEC_REQ", "PASSKEY_NOTIF", "PASSKEY_REQ", "OOB_REQ", "LOCAL_IR", "LOCAL_ER", "NC_REQ", "ADV_STOP_COMPLETE", "SCAN_STOP_COMPLETE", "SET_STATIC_RAND_ADDR", "UPDATE_CONN_PARAMS", "SET_PKT_LENGTH_COMPLETE", "SET_LOCAL_PRIVACY_COMPLETE", "REMOVE_BOND_DEV_COMPLETE", "CLEAR_BOND_DEV_COMPLETE", "GET_BOND_DEV_COMPLETE", "READ_RSSI_COMPLETE", "UPDATE_WHITELIST_COMPLETE"};
static const char *bt_gap_evt_names[] = { "DISC_RES", "DISC_STATE_CHANGED", "RMT_SRVCS", "RMT_SRVC_REC", "AUTH_CMPL", "PIN_REQ", "CFM_REQ", "KEY_NOTIF", "KEY_REQ", "READ_RSSI_DELTA"};
static const char *ble_addr_type_names[] = {"PUBLIC", "RANDOM", "RPA_PUBLIC", "RPA_RANDOM"};

const char *ble_addr_type_str(esp_ble_addr_type_t ble_addr_type)
{
    if (ble_addr_type > BLE_ADDR_TYPE_RPA_RANDOM) {
        return "UNKNOWN";
    }
    return ble_addr_type_names[ble_addr_type];
}

const char *ble_gap_evt_str(uint8_t event)
{
    if (event >= SIZEOF_ARRAY(ble_gap_evt_names)) {
        return "UNKNOWN";
    }
    return ble_gap_evt_names[event];
}

const char *bt_gap_evt_str(uint8_t event)
{
    if (event >= SIZEOF_ARRAY(bt_gap_evt_names)) {
        return "UNKNOWN";
    }
    return bt_gap_evt_names[event];
}
#endif

#if CONFIG_BT_BLE_ENABLED
const char *esp_ble_key_type_str(esp_ble_key_type_t key_type)
{
    const char *key_str = NULL;
    switch (key_type) {
    case ESP_LE_KEY_NONE:
        key_str = "ESP_LE_KEY_NONE";
        break;
    case ESP_LE_KEY_PENC:
        key_str = "ESP_LE_KEY_PENC";
        break;
    case ESP_LE_KEY_PID:
        key_str = "ESP_LE_KEY_PID";
        break;
    case ESP_LE_KEY_PCSRK:
        key_str = "ESP_LE_KEY_PCSRK";
        break;
    case ESP_LE_KEY_PLK:
        key_str = "ESP_LE_KEY_PLK";
        break;
    case ESP_LE_KEY_LLK:
        key_str = "ESP_LE_KEY_LLK";
        break;
    case ESP_LE_KEY_LENC:
        key_str = "ESP_LE_KEY_LENC";
        break;
    case ESP_LE_KEY_LID:
        key_str = "ESP_LE_KEY_LID";
        break;
    case ESP_LE_KEY_LCSRK:
        key_str = "ESP_LE_KEY_LCSRK";
        break;
    default:
        key_str = "INVALID BLE KEY TYPE";
        break;

    }
    return key_str;
}
#endif /* CONFIG_BT_BLE_ENABLED */

#if !CONFIG_BT_NIMBLE_ENABLED
void esp_hid_scan_results_free(esp_hid_scan_result_t *results)
{
    esp_hid_scan_result_t *r = NULL;
    while (results) {
        r = results;
        results = results->next;
        if (r->name != NULL) {
            free((char *)r->name);
        }
        free(r);
    }
}
#endif

#if (CONFIG_BT_HID_DEVICE_ENABLED || CONFIG_BT_BLE_ENABLED)
static esp_hid_scan_result_t *find_scan_result(esp_bd_addr_t bda, esp_hid_scan_result_t *results)
{
    esp_hid_scan_result_t *r = results;
    while (r) {
        if (memcmp(bda, r->bda, sizeof(esp_bd_addr_t)) == 0) {
            return r;
        }
        r = r->next;
    }
    return NULL;
}
#endif /* (CONFIG_BT_HID_DEVICE_ENABLED || CONFIG_BT_BLE_ENABLED) */

#if CONFIG_BT_HID_DEVICE_ENABLED
static void add_bt_scan_result(esp_bd_addr_t bda, esp_bt_cod_t *cod, esp_bt_uuid_t *uuid, uint8_t *name, uint8_t name_len, int rssi)
{
    esp_hid_scan_result_t *r = find_scan_result(bda, bt_scan_results);
    if (r) {
        //Some info may come later
        if (r->name == NULL && name && name_len) {
            char *name_s = (char *)malloc(name_len + 1);
            if (name_s == NULL) {
                ESP_LOGE(TAG, "Malloc result name failed!");
                return;
            }
            memcpy(name_s, name, name_len);
            name_s[name_len] = 0;
            r->name = (const char *)name_s;
        }
        if (r->bt.uuid.len == 0 && uuid->len) {
            memcpy(&r->bt.uuid, uuid, sizeof(esp_bt_uuid_t));
        }
        if (rssi != 0) {
            r->rssi = rssi;
        }
        return;
    }

    r = (esp_hid_scan_result_t *)malloc(sizeof(esp_hid_scan_result_t));
    if (r == NULL) {
        ESP_LOGE(TAG, "Malloc bt_hidh_scan_result_t failed!");
        return;
    }
    r->transport = ESP_HID_TRANSPORT_BT;
    memcpy(r->bda, bda, sizeof(esp_bd_addr_t));
    memcpy(&r->bt.cod, cod, sizeof(esp_bt_cod_t));
    memcpy(&r->bt.uuid, uuid, sizeof(esp_bt_uuid_t));
    r->usage = esp_hid_usage_from_cod((uint32_t)cod);
    r->rssi = rssi;
    r->name = NULL;
    if (name_len && name) {
        char *name_s = (char *)malloc(name_len + 1);
        if (name_s == NULL) {
            free(r);
            ESP_LOGE(TAG, "Malloc result name failed!");
            return;
        }
        memcpy(name_s, name, name_len);
        name_s[name_len] = 0;
        r->name = (const char *)name_s;
    }
    r->next = bt_scan_results;
    bt_scan_results = r;
    num_bt_scan_results++;
}
#endif

#if CONFIG_BT_BLE_ENABLED
static void add_ble_scan_result(esp_bd_addr_t bda, esp_ble_addr_type_t addr_type, uint16_t appearance, uint8_t *name, uint8_t name_len, int rssi)
{
    if (find_scan_result(bda, ble_scan_results)) {
        ESP_LOGW(TAG, "Result already exists!");
        return;
    }
    esp_hid_scan_result_t *r = (esp_hid_scan_result_t *)malloc(sizeof(esp_hid_scan_result_t));
    if (r == NULL) {
        ESP_LOGE(TAG, "Malloc ble_hidh_scan_result_t failed!");
        return;
    }
    r->transport = ESP_HID_TRANSPORT_BLE;
    memcpy(r->bda, bda, sizeof(esp_bd_addr_t));
    r->ble.appearance = appearance;
    r->ble.addr_type = addr_type;
    r->usage = esp_hid_usage_from_appearance(appearance);
    r->rssi = rssi;
    r->name = NULL;
    if (name_len && name) {
        char *name_s = (char *)malloc(name_len + 1);
        if (name_s == NULL) {
            free(r);
            ESP_LOGE(TAG, "Malloc result name failed!");
            return;
        }
        memcpy(name_s, name, name_len);
        name_s[name_len] = 0;
        r->name = (const char *)name_s;
    }
    r->next = ble_scan_results;
    ble_scan_results = r;
    num_ble_scan_results++;
}
#endif /* CONFIG_BT_BLE_ENABLED */

#if !CONFIG_BT_NIMBLE_ENABLED
void print_uuid(esp_bt_uuid_t *uuid)
{
    if (uuid->len == ESP_UUID_LEN_16) {
        GAP_DBG_PRINTF("UUID16: 0x%04x", uuid->uuid.uuid16);
    } else if (uuid->len == ESP_UUID_LEN_32) {
        GAP_DBG_PRINTF("UUID32: 0x%08x", uuid->uuid.uuid32);
    } else if (uuid->len == ESP_UUID_LEN_128) {
        GAP_DBG_PRINTF("UUID128: %02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x", uuid->uuid.uuid128[0],
                       uuid->uuid.uuid128[1], uuid->uuid.uuid128[2], uuid->uuid.uuid128[3],
                       uuid->uuid.uuid128[4], uuid->uuid.uuid128[5], uuid->uuid.uuid128[6],
                       uuid->uuid.uuid128[7], uuid->uuid.uuid128[8], uuid->uuid.uuid128[9],
                       uuid->uuid.uuid128[10], uuid->uuid.uuid128[11], uuid->uuid.uuid128[12],
                       uuid->uuid.uuid128[13], uuid->uuid.uuid128[14], uuid->uuid.uuid128[15]);
    }
}
#endif

#if CONFIG_BT_HID_DEVICE_ENABLED
static void handle_bt_device_result(struct disc_res_param *disc_res)
{
    GAP_DBG_PRINTF("BT : " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(disc_res->bda));
    uint32_t codv = 0;
    esp_bt_cod_t *cod = (esp_bt_cod_t *)&codv;
    int8_t rssi = 0;
    uint8_t *name = NULL;
    uint8_t name_len = 0;
    esp_bt_uuid_t uuid;

    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = 0;

    for (int i = 0; i < disc_res->num_prop; i++) {
        esp_bt_gap_dev_prop_t *prop = &disc_res->prop[i];
        if (prop->type != ESP_BT_GAP_DEV_PROP_EIR) {
            GAP_DBG_PRINTF(", %s: ", gap_bt_prop_type_names[prop->type]);
        }
        if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
            if (prop->val == NULL) {
                continue;
            }
            name = (uint8_t *)prop->val;
            name_len = strlen((const char *)name);
            GAP_DBG_PRINTF("%s", (const char *)name);
        } else if (prop->type == ESP_BT_GAP_DEV_PROP_RSSI) {
            rssi = *((int8_t *)prop->val);
            GAP_DBG_PRINTF("%d", rssi);
        } else if (prop->type == ESP_BT_GAP_DEV_PROP_COD) {
            memcpy(&codv, prop->val, sizeof(uint32_t));
            GAP_DBG_PRINTF("major: %s, minor: %d, service: 0x%03x", esp_hid_cod_major_str(cod->major), cod->minor, cod->service);
        } else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR) {
            uint8_t len = 0;
            uint8_t *data = 0;

            data = esp_bt_gap_resolve_eir_data((uint8_t *)prop->val, ESP_BT_EIR_TYPE_CMPL_16BITS_UUID, &len);
            if (data == NULL) {
                data = esp_bt_gap_resolve_eir_data((uint8_t *)prop->val, ESP_BT_EIR_TYPE_INCMPL_16BITS_UUID, &len);
            }
            if (data && len == ESP_UUID_LEN_16) {
                uuid.len = ESP_UUID_LEN_16;
                uuid.uuid.uuid16 = data[0] + (data[1] << 8);
                GAP_DBG_PRINTF(", "); print_uuid(&uuid);
                continue;
            }

            data = esp_bt_gap_resolve_eir_data((uint8_t *)prop->val, ESP_BT_EIR_TYPE_CMPL_32BITS_UUID, &len);
            if (data == NULL) {
                data = esp_bt_gap_resolve_eir_data((uint8_t *)prop->val, ESP_BT_EIR_TYPE_INCMPL_32BITS_UUID, &len);
            }
            if (data && len == ESP_UUID_LEN_32) {
                uuid.len = len;
                memcpy(&uuid.uuid.uuid32, data, sizeof(uint32_t));
                GAP_DBG_PRINTF(", "); print_uuid(&uuid);
                continue;
            }

            data = esp_bt_gap_resolve_eir_data((uint8_t *)prop->val, ESP_BT_EIR_TYPE_CMPL_128BITS_UUID, &len);
            if (data == NULL) {
                data = esp_bt_gap_resolve_eir_data((uint8_t *)prop->val, ESP_BT_EIR_TYPE_INCMPL_128BITS_UUID, &len);
            }
            if (data && len == ESP_UUID_LEN_128) {
                uuid.len = len;
                memcpy(uuid.uuid.uuid128, (uint8_t *)data, len);
                GAP_DBG_PRINTF(", "); print_uuid(&uuid);
                continue;
            }

            //try to find a name
            if (name == NULL) {
                data = esp_bt_gap_resolve_eir_data((uint8_t *)prop->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
                if (data == NULL) {
                    data = esp_bt_gap_resolve_eir_data((uint8_t *)prop->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
                }
                if (data && len) {
                    name = data;
                    name_len = len;
                    GAP_DBG_PRINTF(", NAME: ");
                    for (int x = 0; x < len; x++) {
                        GAP_DBG_PRINTF("%c", (char)data[x]);
                    }
                }
            }
        }
    }
    GAP_DBG_PRINTF("\n");

    if (cod->major == ESP_BT_COD_MAJOR_DEV_PERIPHERAL || (find_scan_result(disc_res->bda, bt_scan_results) != NULL)) {
        add_bt_scan_result(disc_res->bda, cod, &uuid, name, name_len, rssi);
    }
}
#endif

#if CONFIG_BT_BLE_ENABLED
static void handle_ble_device_result(struct ble_scan_result_evt_param *scan_rst)
{

    uint16_t uuid = 0;
    uint16_t appearance = 0;
    char name[64] = {0};

    uint8_t uuid_len = 0;
    uint8_t *uuid_d = esp_ble_resolve_adv_data_by_type(scan_rst->ble_adv,
                      scan_rst->adv_data_len + scan_rst->scan_rsp_len,
                      ESP_BLE_AD_TYPE_16SRV_CMPL,
                      &uuid_len);
    if (uuid_d != NULL && uuid_len) {
        uuid = uuid_d[0] + (uuid_d[1] << 8);
    }

    uint8_t appearance_len = 0;
    uint8_t *appearance_d = esp_ble_resolve_adv_data_by_type(scan_rst->ble_adv,
                                                    scan_rst->adv_data_len + scan_rst->scan_rsp_len,
                                                    ESP_BLE_AD_TYPE_APPEARANCE,
                                                    &appearance_len);
    if (appearance_d != NULL && appearance_len) {
        appearance = appearance_d[0] + (appearance_d[1] << 8);
    }

    uint8_t adv_name_len = 0;
    uint8_t *adv_name = esp_ble_resolve_adv_data_by_type(scan_rst->ble_adv,
                                                 scan_rst->adv_data_len + scan_rst->scan_rsp_len,
                                                 ESP_BLE_AD_TYPE_NAME_CMPL,
                                                 &adv_name_len);

    if (adv_name == NULL) {
        adv_name = esp_ble_resolve_adv_data_by_type(scan_rst->ble_adv,
                                            scan_rst->adv_data_len + scan_rst->scan_rsp_len,
                                            ESP_BLE_AD_TYPE_NAME_SHORT,
                                            &adv_name_len);
    }

    if (adv_name != NULL && adv_name_len) {
        memcpy(name, adv_name, adv_name_len);
        name[adv_name_len] = 0;
    }

    GAP_DBG_PRINTF("BLE: " ESP_BD_ADDR_STR ", ", ESP_BD_ADDR_HEX(scan_rst->bda));
    GAP_DBG_PRINTF("RSSI: %d, ", scan_rst->rssi);
    GAP_DBG_PRINTF("UUID: 0x%04x, ", uuid);
    GAP_DBG_PRINTF("APPEARANCE: 0x%04x, ", appearance);
    GAP_DBG_PRINTF("ADDR_TYPE: '%s'", ble_addr_type_str(scan_rst->ble_addr_type));
    if (adv_name_len) {
        GAP_DBG_PRINTF(", NAME: '%s'", name);
    }
    GAP_DBG_PRINTF("\n");

    if (uuid == ESP_GATT_UUID_HID_SVC) {
        add_ble_scan_result(scan_rst->bda, scan_rst->ble_addr_type, appearance, adv_name, adv_name_len, scan_rst->rssi);
    }
}
#endif /* CONFIG_BT_BLE_ENABLED */

#if CONFIG_BT_HID_DEVICE_ENABLED
/*
 * BT GAP
 * */

static void bt_gap_event_handler(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        ESP_LOGV(TAG, "BT GAP DISC_STATE %s", (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) ? "START" : "STOP");
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            SEND_BT_CB();
        }
        break;
    }
    case ESP_BT_GAP_DISC_RES_EVT: {
        handle_bt_device_result(&param->disc_res);
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(TAG, "BT GAP PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            ESP_LOGI(TAG, "Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            ESP_LOGI(TAG, "Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "BT GAP CFM_REQ_EVT Please compare the numeric value: %06" PRIu32,
                 param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "BT GAP KEY_NOTIF_EVT passkey:%06" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "BT GAP KEY_REQ_EVT Please enter passkey!");
        break;
#endif
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "BT GAP MODE_CHG_EVT mode:%d", param->mode_chg.mode);
        break;
    default:
        ESP_LOGV(TAG, "BT GAP EVENT %s", bt_gap_evt_str(event));
        break;
    }
}

static esp_err_t init_bt_gap(void)
{
    esp_err_t ret;
#if (CONFIG_EXAMPLE_SSP_ENABLED)
    /* Set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif
    /*
     * Set default parameters for Legacy Pairing
     * Use fixed pin code
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code;
    pin_code[0] = '1';
    pin_code[1] = '2';
    pin_code[2] = '3';
    pin_code[3] = '4';
    esp_bt_gap_set_pin(pin_type, 4, pin_code);

    if ((ret = esp_bt_gap_register_callback(bt_gap_event_handler)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_gap_register_callback failed: %d", ret);
        return ret;
    }

    // Allow BT devices to connect back to us
    if ((ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_gap_set_scan_mode failed: %d", ret);
        return ret;
    }
    return ret;
}

static esp_err_t start_bt_scan(uint32_t seconds)
{
    esp_err_t ret = ESP_OK;
    if ((ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, (int)(seconds / 1.28), 0)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_gap_start_discovery failed: %d", ret);
        return ret;
    }
    return ret;
}
#endif

extern void ble_hid_task_start_up(void);
extern void ble_hid_task_shut_down(void);

#if CONFIG_BT_BLE_ENABLED
/*
 * BLE GAP
 * */
static void ble_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    /*
     * SCAN
     * */
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        ESP_LOGV(TAG, "BLE GAP EVENT SCAN_PARAM_SET_COMPLETE");
        SEND_BLE_CB();
        break;
    }
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
        switch (scan_result->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT: {
            handle_ble_device_result(&scan_result->scan_rst);
            break;
        }
        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            ESP_LOGV(TAG, "BLE GAP EVENT SCAN DONE: %d", scan_result->scan_rst.num_resps);
            SEND_BLE_CB();
            break;
        default:
            break;
        }
        break;
    }
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT: {
        ESP_LOGV(TAG, "BLE GAP EVENT SCAN CANCELED");
        break;
    }

    /*
     * ADVERTISEMENT
     * */
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGV(TAG, "BLE GAP ADV_DATA_SET_COMPLETE");
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        ESP_LOGV(TAG, "BLE GAP ADV_START_COMPLETE");
        break;

    /*
     * AUTHENTICATION
     * */
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (!param->ble_security.auth_cmpl.success) {
            // if AUTH ERROR,hid maybe don't work.
            ESP_LOGE(TAG, "BLE GAP AUTH ERROR: 0x%x", param->ble_security.auth_cmpl.fail_reason);
        } else {
            ESP_LOGI(TAG, "BLE GAP AUTH SUCCESS");
        }
        ble_hid_task_start_up();
        break;

    case ESP_GAP_BLE_KEY_EVT: //shows the ble key info share with peer device to the user.
        ESP_LOGI(TAG, "BLE GAP KEY type = %s", esp_ble_key_type_str(param->ble_security.ble_key.key_type));
        break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: // ESP_IO_CAP_OUT
        // The app will receive this evt when the IO has Output capability and the peer device IO has Input capability.
        // Show the passkey number to the user to input it in the peer device.
        ESP_LOGI(TAG, "BLE GAP PASSKEY_NOTIF passkey:%"PRIu32, param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT: // ESP_IO_CAP_IO
        // The app will receive this event when the IO has DisplayYesNO capability and the peer device IO also has DisplayYesNo capability.
        // show the passkey number to the user to confirm it with the number displayed by peer device.
        ESP_LOGI(TAG, "BLE GAP NC_REQ passkey:%"PRIu32, param->ble_security.key_notif.passkey);
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT: // ESP_IO_CAP_IN
        // The app will receive this evt when the IO has Input capability and the peer device IO has Output capability.
        // See the passkey number on the peer device and send it back.
        ESP_LOGI(TAG, "BLE GAP PASSKEY_REQ");
        //esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, 1234);
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_LOGI(TAG, "BLE GAP SEC_REQ");
        // Send the positive(true) security response to the peer device to accept the security request.
        // If not accept the security request, should send the security response with negative(false) accept value.
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    default:
        ESP_LOGV(TAG, "BLE GAP EVENT %s", ble_gap_evt_str(event));
        break;
    }
}

static esp_err_t init_ble_gap(void)
{
    esp_err_t ret;

    if ((ret = esp_ble_gap_register_callback(ble_gap_event_handler)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_register_callback failed: %d", ret);
        return ret;
    }
    return ret;
}

static esp_ble_scan_params_t hid_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_ENABLE,
};

static esp_err_t start_ble_scan(uint32_t seconds)
{
    esp_err_t ret = ESP_OK;
    if ((ret = esp_ble_gap_set_scan_params(&hid_scan_params)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_set_scan_params failed: %d", ret);
        return ret;
    }
    WAIT_BLE_CB();

    if ((ret = esp_ble_gap_start_scanning(seconds)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_start_scanning failed: %d", ret);
        return ret;
    }
    return ret;
}

#if !CONFIG_BT_NIMBLE_ENABLED
esp_err_t esp_hid_ble_gap_adv_init(uint16_t appearance, const char *device_name)
{

    esp_err_t ret;

    const uint8_t hidd_service_uuid128[] = {
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
    };

    esp_ble_adv_data_t ble_adv_data = {
        .set_scan_rsp = false,
        .include_name = true,
        .include_txpower = true,
        .min_interval = 0x0006, //slave connection min interval, Time = min_interval * 1.25 msec
        .max_interval = 0x0010, //slave connection max interval, Time = max_interval * 1.25 msec
        .appearance = appearance,
        .manufacturer_len = 0,
        .p_manufacturer_data =  NULL,
        .service_data_len = 0,
        .p_service_data = NULL,
        .service_uuid_len = sizeof(hidd_service_uuid128),
        .p_service_uuid = (uint8_t *)hidd_service_uuid128,
        .flag = 0x6,
    };

    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    //esp_ble_io_cap_t iocap = ESP_IO_CAP_OUT;//you have to enter the key on the host
    //esp_ble_io_cap_t iocap = ESP_IO_CAP_IN;//you have to enter the key on the device
    esp_ble_io_cap_t iocap = ESP_IO_CAP_IO;//you have to agree that key matches on both
    //esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;//device is not capable of input or output, insecure
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16; //the key size should be 7~16 bytes
    uint32_t passkey = 1234;//ESP_IO_CAP_OUT

    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "GAP set_security_param AUTHEN_REQ_MODE failed: %d", ret);
        return ret;
    }

    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "GAP set_security_param IOCAP_MODE failed: %d", ret);
        return ret;
    }

    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "GAP set_security_param SET_INIT_KEY failed: %d", ret);
        return ret;
    }

    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "GAP set_security_param SET_RSP_KEY failed: %d", ret);
        return ret;
    }

    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "GAP set_security_param MAX_KEY_SIZE failed: %d", ret);
        return ret;
    }

    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(uint32_t))) != ESP_OK) {
        ESP_LOGE(TAG, "GAP set_security_param SET_STATIC_PASSKEY failed: %d", ret);
        return ret;
    }

    if ((ret = esp_ble_gap_set_device_name(device_name)) != ESP_OK) {
        ESP_LOGE(TAG, "GAP set_device_name failed: %d", ret);
        return ret;
    }

    if ((ret = esp_ble_gap_config_adv_data(&ble_adv_data)) != ESP_OK) {
        ESP_LOGE(TAG, "GAP config_adv_data failed: %d", ret);
        return ret;
    }

    return ret;
}
#endif
esp_err_t esp_hid_ble_gap_adv_start(void)
{
    static esp_ble_adv_params_t hidd_adv_params = {
        .adv_int_min        = 0x20,
        .adv_int_max        = 0x30,
        .adv_type           = ADV_TYPE_IND,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .channel_map        = ADV_CHNL_ALL,
        .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    return esp_ble_gap_start_advertising(&hidd_adv_params);
}
#endif /* CONFIG_BT_BLE_ENABLED */

#if CONFIG_BT_NIMBLE_ENABLED
#define GATT_SVR_SVC_HID_UUID 0x1812

extern void ble_hid_task_start_up(void);
static struct ble_hs_adv_fields fields;
static struct ble_hs_adv_fields scan_response_fields;
static ble_uuid16_t hid_service_uuid = BLE_UUID16_INIT(GATT_SVR_SVC_HID_UUID);

esp_err_t esp_hid_ble_gap_adv_init(uint16_t appearance, const char *device_name)
{
    /**
     *  Set the advertisement data included in our advertisements:
     *     o Flags (indicates advertisement type and other general info).
     *     o Advertising tx power.
     *     o Device name.
     *     o 16-bit service UUIDs (HID).
     */

    memset(&fields, 0, sizeof fields);

    /* Advertise two flags:
     *     o Discoverability in forthcoming advertisement (general)
     *     o BLE-only (BR/EDR unsupported).
     */
    fields.flags = BLE_HS_ADV_F_DISC_GEN |
                   BLE_HS_ADV_F_BREDR_UNSUP;

    fields.appearance = appearance;
    fields.appearance_is_present = 1;

    /* Indicate that the TX power level field should be included; have the
     * stack fill this value automatically.  This is done by assigning the
     * special value BLE_HS_ADV_TX_PWR_LVL_AUTO.
     */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    fields.uuids16 = &hid_service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    /*
     * 名称、HID UUID、外观和发射功率无法同时装入 31 字节广播包。
     * 主广播保留 HID 服务，完整名称改由扫描响应提供。
     */
    memset(&scan_response_fields, 0, sizeof(scan_response_fields));
    scan_response_fields.name = (uint8_t *)device_name;
    scan_response_fields.name_len = strlen(device_name);
    scan_response_fields.name_is_complete = 1;

    int rc = ble_svc_gap_device_name_set(device_name);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error setting GAP device name; rc=%d\n", rc);
        return rc;
    }

    /*
     * 初始化 BLE 安全参数。
     *
     * 这是一个没有屏幕、没有输入键盘的 HID 外设，只能使用 Just Works，
     * 不能要求 Windows 输入 PIN 或确认数值比较。Windows 自带 BLE HID
     * 配对路径对 Secure Connections 的无输入设备兼容性并不一致；先使用
     * 蓝牙规范允许的 legacy Just Works，并保留 bonding 以便 HID 后续连接
     * 复用密钥。这样不会引入 MITM 要求，也不会触发不存在的 passkey 流程。
     */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;

    return ESP_OK;

}

static int nimble_restart_advertising_if_idle(const char *reason)
{
    struct ble_gap_conn_desc desc;
    if (s_active_connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        int find_rc = ble_gap_conn_find(s_active_connection_handle, &desc);
        if (find_rc == 0) {
            ESP_LOGI(TAG,
                     "skip advertising restart (%s): active conn_handle=%u",
                     reason, s_active_connection_handle);
            return 0;
        }
        ESP_LOGW(TAG,
                 "clearing stale conn_handle=%u before advertising restart (%s); rc=%d",
                 s_active_connection_handle, reason, find_rc);
        s_active_connection_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    if (ble_gap_adv_active()) {
        ESP_LOGI(TAG, "advertising already active (%s)", reason);
        return 0;
    }

    int rc = esp_hid_ble_gap_adv_start();
    if (rc == 0) {
        ESP_LOGI(TAG, "advertising restarted (%s)", reason);
    } else {
        ESP_LOGE(TAG, "advertising restart failed (%s); rc=%d", reason, rc);
    }
    return rc;
}

static int
nimble_hid_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    const struct ble_gap_upd_params low_latency_params = {
        .itvl_min = BLE_GAP_CONN_ITVL_MS(7.5),
        .itvl_max = BLE_GAP_CONN_ITVL_MS(10),
        .latency = 0,
        .supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(5000),
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /*
         * On the ESP-IDF NimBLE peripheral path this event can carry the
         * remote-feature exchange status. A non-zero value does not always
         * mean that the ACL connection was removed, so verify the handle.
         */
        rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
        if (rc != 0) {
            ESP_LOGW(TAG,
                     "connection failed; status=%d conn_handle=%u is not active; rc=%d",
                     event->connect.status, event->connect.conn_handle, rc);
            ++s_connection_generation;
            s_active_connection_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_hid_task_shut_down();
            nimble_restart_advertising_if_idle("connection attempt failed");
            return 0;
        }
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "connection established; status=0 conn_handle=%u",
                     event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG,
                     "connection active despite non-zero status=%d; conn_handle=%u",
                     event->connect.status, event->connect.conn_handle);
        }

        s_active_connection_handle = event->connect.conn_handle;
        uint32_t generation = ++s_connection_generation;
        log_connection_parameters("connect_event_initial", event->connect.conn_handle);

        /*
         * status=26/19 can accompany a still-present ACL on this NimBLE
         * peripheral path.  Do not immediately send a connection-parameter
         * update in that ambiguous state: Windows may terminate the link
         * while service discovery/encryption is still completing.  The HID
         * path only needs the existing negotiated parameters to reconnect;
         * prioritize completing encryption and subscriptions first.
         */
        if (event->connect.status != 0) {
            ESP_LOGI(TAG,
                     "skip low-latency connection update until encryption; "
                     "connect_status=%d",
                     event->connect.status);
            return 0;
        }

        rc = ble_gap_update_params(event->connect.conn_handle,
                                   &low_latency_params);
        if (rc != 0) {
            ESP_LOGW(TAG, "low-latency connection update failed; rc=%d", rc);
        } else {
            ESP_LOGI(TAG,
                     "submitted connection interval 7.5-10 ms, latency=0; source=connect_event");
        }
        schedule_narrow_connection_interval(generation, "connect_event");
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ++s_connection_generation;
        s_active_connection_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "disconnect; reason=%d", event->disconnect.reason);
        ble_hid_task_shut_down();
        nimble_restart_advertising_if_idle("connection disconnected");
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
        /* The central has updated the connection parameters. */
        ESP_LOGI(TAG, "connection updated; status=%d",
                event->conn_update.status);
        if (event->conn_update.status == 0) {
            rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
            if (rc == 0) {
                log_connection_parameters("conn_update_final", event->conn_update.conn_handle);
                verify_low_latency_connection_parameters(&desc);
            } else {
                ESP_LOGW(TAG, "read negotiated connection parameters failed; rc=%d", rc);
            }
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertise complete; reason=%d",
                event->adv_complete.reason);
        nimble_restart_advertising_if_idle("advertising completed");
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe event; conn_handle=%d attr_handle=%d "
                "reason=%d prevn=%d curn=%d previ=%d curi=%d\n",
                event->subscribe.conn_handle,
                event->subscribe.attr_handle,
                event->subscribe.reason,
                event->subscribe.prev_notify,
                event->subscribe.cur_notify,
                event->subscribe.prev_indicate,
                event->subscribe.cur_indicate);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu update event; conn_handle=%d cid=%d mtu=%d",
                event->mtu.conn_handle,
                event->mtu.channel_id,
                event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        /* Encryption has been enabled or disabled for this connection. */
        MODLOG_DFLT(INFO, "encryption change event; status=%d ",
                event->enc_change.status);
        if (event->enc_change.status == 0) {
            rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
            assert(rc == 0);
            log_connection_parameters("encryption_current", event->enc_change.conn_handle);
            schedule_narrow_connection_interval(s_connection_generation, "encryption_complete");
            ble_hid_task_start_up();
        } else {
            ble_hid_task_shut_down();
            /*
             * ENC_CHANGE.status 使用的是 NimBLE Host 状态码，而不是原始
             * HCI 错误码。特别是 BLE_HS_ENOTCONN == 7 只表示该事件到达
             * 时 ACL 已经消失，不能据此删除仍可能有效的持久化 bond，
             * 也不能再次 terminate 一个已经不存在的连接。
             *
             * 旧逻辑会在每次冷启动重连竞争中删除本端 bond，导致中心端
             * 仍持有旧密钥时反复连接/断开。这里统一等待 DISCONNECT 和
             * 广播恢复；只有 BLE_GAP_EVENT_REPEAT_PAIRING 明确要求重新
             * 配对时才删除旧 bond。
             */
            if (event->enc_change.status == BLE_HS_ENOTCONN) {
                ESP_LOGW(TAG,
                         "encryption change arrived after connection loss; "
                         "status=%d (BLE_HS_ENOTCONN), preserve bond and wait for disconnect",
                         event->enc_change.status);
            } else {
                ESP_LOGW(TAG,
                         "encryption failed; status=%d, preserve bond and wait for disconnect",
                         event->enc_change.status);
            }
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        MODLOG_DFLT(INFO, "notify_tx event; conn_handle=%d attr_handle=%d "
                "status=%d is_indication=%d",
                event->notify_tx.conn_handle,
                event->notify_tx.attr_handle,
                event->notify_tx.status,
                event->notify_tx.indication);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* We already have a bond with the peer, but it is attempting to
         * establish a new secure link.  This app sacrifices security for
         * convenience: just throw away the old bond and accept the new link.
         */

        /* Delete the old bond. */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        ble_store_util_delete_peer(&desc.peer_id_addr);

        /* Return BLE_GAP_REPEAT_PAIRING_RETRY to indicate that the host should
         * continue with the pairing operation.
         */
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(TAG, "PASSKEY_ACTION_EVENT started");
        struct ble_sm_io pkey = {0};
        int key = 1;

        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action = event->passkey.params.action;
            pkey.passkey = 123456; // This is the passkey to be entered on peer
            ESP_LOGI(TAG, "Enter passkey %" PRIu32 "on the peer side", pkey.passkey);
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            ESP_LOGI(TAG, "Accepting passkey..");
            pkey.action = event->passkey.params.action;
            pkey.numcmp_accept = key;
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_OOB) {
            static uint8_t tem_oob[16] = {0};
            pkey.action = event->passkey.params.action;
            for (int i = 0; i < 16; i++) {
                pkey.oob[i] = tem_oob[i];
            }
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            ESP_LOGI(TAG, "Input not supported passing -> 123456");
            pkey.action = event->passkey.params.action;
            pkey.passkey = 123456;
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io result: %d", rc);
        }
        return 0;
    }
    return 0;
}
esp_err_t esp_hid_ble_gap_adv_start(void)
{
    static bool random_address_set;
    int rc;
    uint8_t random_address[BLE_DEV_ADDR_LEN];
    struct ble_gap_adv_params adv_params;
    if (!random_address_set) {
        rc = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, random_address, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "read public BLE address failed; rc=%d", rc);
            return rc;
        }
        /* 保留芯片地址的唯一部分，并设置静态随机地址规定的高两位。 */
        random_address[5] = (random_address[5] & 0x3f) | 0xc0;
        random_address[0] ^= 0x08;
        rc = ble_hs_id_set_rnd(random_address);
        if (rc != 0) {
            ESP_LOGE(TAG, "set static random BLE address failed; rc=%d", rc);
            return rc;
        }
        random_address_set = true;
        ESP_LOGI(TAG, "static random BLE address: %02x:%02x:%02x:%02x:%02x:%02x",
                 random_address[5], random_address[4], random_address[3],
                 random_address[2], random_address[1], random_address[0]);
    }

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error setting advertisement data; rc=%d\n", rc);
        return rc;
    }
    rc = ble_gap_adv_rsp_set_fields(&scan_response_fields);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error setting scan response data; rc=%d\n", rc);
        return rc;
    }
    /* Begin advertising. */
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);/* Recommended interval 30ms to 50ms */
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);
    rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER,
                           &adv_params, nimble_hid_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error enabling advertisement; rc=%d\n", rc);
        return rc;
    }
    return rc;
}
#endif


/*
 * CONTROLLER INIT
 * */

#if !CONFIG_BT_NIMBLE_ENABLED
static esp_err_t init_low_level(uint8_t mode)
{
    esp_err_t ret;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32
    bt_cfg.mode = mode;
#endif
#if CONFIG_BT_HID_DEVICE_ENABLED
    if (mode & ESP_BT_MODE_CLASSIC_BT) {
        bt_cfg.bt_max_acl_conn = 3;
        bt_cfg.bt_max_sync_conn = 3;
    } else
#endif
    {
        ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (ret) {
            ESP_LOGE(TAG, "esp_bt_controller_mem_release failed: %d", ret);
            return ret;
        }
    }
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %d", ret);
        return ret;
    }

    ret = esp_bt_controller_enable(mode);
    if (ret) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %d", ret);
        return ret;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
#if (CONFIG_EXAMPLE_SSP_ENABLED == false)
    bluedroid_cfg.ssp_en = false;
#endif
    ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret) {
        ESP_LOGE(TAG, "esp_bluedroid_init failed: %d", ret);
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "esp_bluedroid_enable failed: %d", ret);
        return ret;
    }
#if CONFIG_BT_HID_DEVICE_ENABLED
    if (mode & ESP_BT_MODE_CLASSIC_BT) {
        ret = init_bt_gap();
        if (ret) {
            return ret;
        }
    }
#endif
#if CONFIG_BT_BLE_ENABLED
    if (mode & ESP_BT_MODE_BLE) {
        ret = init_ble_gap();
        if (ret) {
            return ret;
        }
    }
#endif /* CONFIG_BT_BLE_ENABLED */
    return ret;
}

static esp_err_t deinit_low_level(void)
{
    esp_err_t ret;

    if (bt_scan_results) {
        esp_hid_scan_results_free(bt_scan_results);
        bt_scan_results = NULL;
        num_bt_scan_results = 0;
    }
    if (ble_scan_results) {
        esp_hid_scan_results_free(ble_scan_results);
        ble_scan_results = NULL;
        num_ble_scan_results = 0;
    }

    ret = esp_bluedroid_disable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_disable failed: %d", ret);
    }

    ret = esp_bluedroid_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_deinit failed: %d", ret);
    }

    ret = esp_bt_controller_disable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_disable failed: %d", ret);
    }

    ret = esp_bt_controller_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_deinit failed: %d", ret);
    }

    return ret;
}
#endif

#if CONFIG_BT_NIMBLE_ENABLED
static esp_err_t init_low_level(uint8_t mode)
{
    esp_err_t ret;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32
    bt_cfg.mode = mode;
#endif
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret) {
        ESP_LOGE(TAG, "esp_bt_controller_mem_release failed: %d", ret);
        return ret;
    }
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %d", ret);
        return ret;
    }

    ret = esp_bt_controller_enable(mode);
    if (ret) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %d", ret);
        return ret;
    }

    ret = esp_nimble_init();
    if (ret) {
        ESP_LOGE(TAG, "esp_nimble_init failed: %d", ret);
        return ret;
    }


    return ret;
}

static esp_err_t deinit_low_level(void)
{
    esp_err_t ret;

    ret = esp_nimble_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_deinit failed: %d", ret);
    }

    ret = esp_bt_controller_disable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_disable failed: %d", ret);
    }

    ret = esp_bt_controller_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_deinit failed: %d", ret);
    }

    return ret;
}
#endif

esp_err_t esp_hid_gap_deinit(void)
{
    esp_err_t ret;

    ret = deinit_low_level();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "deinit_low_level failed: %d", ret);
    }

    if (bt_hidh_cb_semaphore != NULL) {
        vSemaphoreDelete(bt_hidh_cb_semaphore);
        bt_hidh_cb_semaphore = NULL;
    }

    if (ble_hidh_cb_semaphore != NULL) {
        vSemaphoreDelete(ble_hidh_cb_semaphore);
        ble_hidh_cb_semaphore = NULL;
    }

    return ESP_OK;
}

esp_err_t esp_hid_gap_init(uint8_t mode)
{
    esp_err_t ret;
    if (!mode || mode > ESP_BT_MODE_BTDM) {
        ESP_LOGE(TAG, "Invalid mode given!");
        return ESP_FAIL;
    }

    if (bt_hidh_cb_semaphore != NULL) {
        ESP_LOGE(TAG, "Already initialised");
        return ESP_FAIL;
    }

    bt_hidh_cb_semaphore = xSemaphoreCreateBinary();
    if (bt_hidh_cb_semaphore == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed!");
        return ESP_FAIL;
    }

    ble_hidh_cb_semaphore = xSemaphoreCreateBinary();
    if (ble_hidh_cb_semaphore == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed!");
        vSemaphoreDelete(bt_hidh_cb_semaphore);
        bt_hidh_cb_semaphore = NULL;
        return ESP_FAIL;
    }

    ret = init_low_level(mode);
    if (ret != ESP_OK) {
        vSemaphoreDelete(bt_hidh_cb_semaphore);
        bt_hidh_cb_semaphore = NULL;
        vSemaphoreDelete(ble_hidh_cb_semaphore);
        ble_hidh_cb_semaphore = NULL;
        return ret;
    }

    return ESP_OK;
}

#if !CONFIG_BT_NIMBLE_ENABLED
esp_err_t esp_hid_scan(uint32_t seconds, size_t *num_results, esp_hid_scan_result_t **results)
{
    if (num_bt_scan_results || bt_scan_results || num_ble_scan_results || ble_scan_results) {
        ESP_LOGE(TAG, "There are old scan results. Free them first!");
        return ESP_FAIL;
    }

#if CONFIG_BT_BLE_ENABLED
    if (start_ble_scan(seconds) == ESP_OK) {
        WAIT_BLE_CB();
    } else {
        return ESP_FAIL;
    }
#endif /* CONFIG_BT_BLE_ENABLED */

#if CONFIG_BT_HID_DEVICE_ENABLED
    if (start_bt_scan(seconds) == ESP_OK) {
        WAIT_BT_CB();
    } else {
        return ESP_FAIL;
    }
#endif

    *num_results = num_bt_scan_results + num_ble_scan_results;
    *results = bt_scan_results;
    if (num_bt_scan_results) {
        while (bt_scan_results->next != NULL) {
            bt_scan_results = bt_scan_results->next;
        }
        bt_scan_results->next = ble_scan_results;
    } else {
        *results = ble_scan_results;
    }

    num_bt_scan_results = 0;
    bt_scan_results = NULL;
    num_ble_scan_results = 0;
    ble_scan_results = NULL;
    return ESP_OK;
}
#endif
