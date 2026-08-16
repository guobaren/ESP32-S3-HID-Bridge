#include <assert.h>
#include <stdio.h>

#include "esp_hid_connect_policy.h"

int main(void)
{
    assert(esp_hid_nimble_connect_event_is_usable(ESP_HID_NIMBLE_STATUS_SUCCESS, true));
    assert(esp_hid_nimble_connect_event_is_usable(
        ESP_HID_HCI_STATUS_UNSUPPORTED_REMOTE_FEATURE,
        true));

    /* status 是原始 HCI 状态：0x08 超时、0x13 远端终止，均不可继续。 */
    assert(!esp_hid_nimble_connect_event_is_usable(
        ESP_HID_HCI_STATUS_CONNECTION_SUPERVISION_TIMEOUT,
        true));
    assert(!esp_hid_nimble_connect_event_is_usable(
        ESP_HID_HCI_STATUS_REMOTE_USER_TERMINATED,
        true));
    assert(!esp_hid_nimble_connect_event_is_usable(ESP_HID_NIMBLE_STATUS_SUCCESS, false));
    assert(!esp_hid_nimble_connect_event_is_usable(
        ESP_HID_HCI_STATUS_UNSUPPORTED_REMOTE_FEATURE,
        false));

    /* 已满足 7.5-10 ms/latency=0 时，CONNECT 或延迟任务都不得重复更新。 */
    assert(esp_hid_nimble_connection_parameters_match_target(6, 0));
    assert(esp_hid_nimble_connection_parameters_match_target(8, 0));
    assert(!esp_hid_nimble_connection_parameters_match_target(5, 0));
    assert(!esp_hid_nimble_connection_parameters_match_target(9, 0));
    assert(!esp_hid_nimble_connection_parameters_match_target(8, 1));
    assert(!esp_hid_nimble_should_request_connection_update(8, 0, false));
    assert(esp_hid_nimble_should_request_connection_update(5, 0, false));
    assert(esp_hid_nimble_should_request_connection_update(8, 1, false));
    assert(!esp_hid_nimble_should_request_connection_update(5, 0, true));

    assert(esp_hid_nimble_should_use_directed_reconnect(true, 1));
    assert(!esp_hid_nimble_should_use_directed_reconnect(true, 0));
    assert(!esp_hid_nimble_should_use_directed_reconnect(false, 4));
    assert(ESP_HID_DIRECTED_RECONNECT_BURSTS == 24);

    assert(esp_hid_nimble_rejected_connect_should_fall_back_to_undirected(
        ESP_HID_HCI_STATUS_CONNECTION_SUPERVISION_TIMEOUT));
    assert(esp_hid_nimble_rejected_connect_should_fall_back_to_undirected(
        ESP_HID_HCI_STATUS_REMOTE_USER_TERMINATED));
    assert(!esp_hid_nimble_rejected_connect_should_fall_back_to_undirected(
        ESP_HID_NIMBLE_STATUS_SUCCESS));
    assert(!esp_hid_nimble_rejected_connect_should_fall_back_to_undirected(
        ESP_HID_HCI_STATUS_UNSUPPORTED_REMOTE_FEATURE));

    puts("BLE 连接策略测试通过：覆盖 HCI 状态、连接参数更新闸门及定向重连广播次数。");
    return 0;
}
