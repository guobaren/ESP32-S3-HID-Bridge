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

    assert(esp_hid_nimble_should_use_directed_reconnect(true, 1));
    assert(!esp_hid_nimble_should_use_directed_reconnect(true, 0));
    assert(!esp_hid_nimble_should_use_directed_reconnect(false, 4));

    puts("BLE 连接策略测试通过：按 HCI 状态判定连接，并限制定向重连广播次数。");
    return 0;
}
