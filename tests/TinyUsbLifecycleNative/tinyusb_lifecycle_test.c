#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tinyusb.h"
#include "tinyusb_pm.h"
#include "tinyusb_task.h"
#include "tinyusb_usb_wakeup.h"
#include "tinyusb_vbus_monitor.h"
#include "esp_private/usb_phy.h"

static bool s_tud_initialized;
static esp_err_t s_phy_create_result;
static bool s_phy_create_returns_handle_on_error;
static esp_err_t s_phy_delete_error;
static unsigned s_phy_create_calls;
static unsigned s_phy_delete_calls;
static esp_err_t s_task_start_result;
static esp_err_t s_task_stop_error;
static unsigned s_task_start_calls;
static unsigned s_task_stop_calls;
static esp_err_t s_wakeup_init_result;
static esp_err_t s_wakeup_deinit_result;
static unsigned s_wakeup_init_calls;
static unsigned s_wakeup_deinit_calls;
static esp_err_t s_pm_init_result;
static esp_err_t s_pm_deinit_result;
static unsigned s_pm_init_calls;
static unsigned s_pm_deinit_calls;

static void fail(const char *message)
{
    fprintf(stderr, "tinyusb_lifecycle_test: FAIL: %s\n", message);
    exit(1);
}

#define CHECK(condition, message) do { if (!(condition)) fail(message); } while (0)

static void reset_fake(void)
{
    s_tud_initialized = false;
    s_phy_create_result = ESP_OK;
    s_phy_create_returns_handle_on_error = false;
    s_phy_delete_error = ESP_OK;
    s_phy_create_calls = 0;
    s_phy_delete_calls = 0;
    s_task_start_result = ESP_OK;
    s_task_stop_error = ESP_OK;
    s_task_start_calls = 0;
    s_task_stop_calls = 0;
    s_wakeup_init_result = ESP_OK;
    s_wakeup_deinit_result = ESP_OK;
    s_wakeup_init_calls = 0;
    s_wakeup_deinit_calls = 0;
    s_pm_init_result = ESP_OK;
    s_pm_deinit_result = ESP_OK;
    s_pm_init_calls = 0;
    s_pm_deinit_calls = 0;
}

static tinyusb_config_t make_config(void)
{
    tinyusb_config_t config = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = { .skip_setup = false, .self_powered = false, .vbus_monitor_io = -1 },
        .task = { .size = 4096, .priority = 5, .xCoreID = 0 },
    };
    return config;
}

bool tud_inited(void) { return s_tud_initialized; }
bool tud_suspended(void) { return false; }
bool tud_remote_wakeup(void) { return true; }

esp_err_t usb_new_phy(const usb_phy_config_t *config, usb_phy_handle_t *handle)
{
    (void)config;
    ++s_phy_create_calls;
    if (s_phy_create_result == ESP_OK || s_phy_create_returns_handle_on_error) {
        *handle = (void *)(uintptr_t)0x1234U;
    }
    return s_phy_create_result;
}

esp_err_t usb_del_phy(usb_phy_handle_t handle)
{
    CHECK(handle != NULL, "PHY delete received a null handle");
    ++s_phy_delete_calls;
    return s_phy_delete_error;
}

esp_err_t tinyusb_task_check_config(const tinyusb_task_config_t *config)
{
    return config != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t tinyusb_task_start(tinyusb_port_t port,
                             const tinyusb_task_config_t *config,
                             const tinyusb_desc_config_t *descriptor)
{
    (void)port;
    (void)config;
    (void)descriptor;
    ++s_task_start_calls;
    if (s_task_start_result == ESP_OK) {
        s_tud_initialized = true;
    }
    return s_task_start_result;
}

esp_err_t tinyusb_task_stop(void)
{
    ++s_task_stop_calls;
    if (s_task_stop_calls == 1U && s_task_stop_error != ESP_OK) {
        return s_task_stop_error;
    }
    s_tud_initialized = false;
    return ESP_OK;
}

esp_err_t tinyusb_usb_wakeup_init(tinyusb_port_t port)
{
    (void)port;
    ++s_wakeup_init_calls;
    return s_wakeup_init_result;
}

esp_err_t tinyusb_usb_wakeup_deinit(void)
{
    ++s_wakeup_deinit_calls;
    if (s_wakeup_deinit_calls <= 2U && s_wakeup_deinit_result != ESP_OK) {
        return s_wakeup_deinit_result;
    }
    return ESP_OK;
}

esp_err_t tinyusb_pm_init(const tinyusb_pm_config_t *config)
{
    (void)config;
    ++s_pm_init_calls;
    return s_pm_init_result;
}

esp_err_t tinyusb_pm_deinit(void)
{
    ++s_pm_deinit_calls;
    return s_pm_deinit_result;
}

esp_err_t tinyusb_pm_lock_get(bool *acquired)
{
    *acquired = false;
    return ESP_OK;
}

esp_err_t tinyusb_pm_remote_wake(void) { return ESP_OK; }
void tinyusb_pm_on_event(tinyusb_event_t *event) { (void)event; }

esp_err_t tinyusb_vbus_monitor_init(const tinyusb_vbus_monitor_config_t *config)
{
    (void)config;
    return ESP_OK;
}

void tinyusb_vbus_monitor_deinit(void) {}

static void test_normal_install_uninstall_is_idempotent(void)
{
    reset_fake();
    tinyusb_config_t config = make_config();
    CHECK(tinyusb_driver_install(&config) == ESP_OK, "normal install failed");
    CHECK(s_tud_initialized, "successful install did not start TinyUSB");
    CHECK(tinyusb_driver_uninstall() == ESP_OK, "normal uninstall failed");
    CHECK(!s_tud_initialized && s_phy_delete_calls == 1U,
          "normal uninstall did not release stack and PHY");
    CHECK(tinyusb_driver_uninstall() == ESP_OK, "repeated uninstall was not idempotent");
    CHECK(s_phy_delete_calls == 1U, "repeated uninstall deleted PHY twice");
}

static void test_task_start_failure_rolls_back_and_allows_retry(void)
{
    reset_fake();
    s_task_start_result = ESP_ERR_TIMEOUT;
    tinyusb_config_t config = make_config();
    CHECK(tinyusb_driver_install(&config) == ESP_ERR_TIMEOUT,
          "clean task-start failure did not return original error");
    CHECK(s_task_stop_calls == 1U && s_phy_delete_calls == 1U,
          "task-start failure did not run unified rollback");
    s_task_start_result = ESP_OK;
    CHECK(tinyusb_driver_install(&config) == ESP_OK,
          "clean rollback did not allow a later install");
    CHECK(tinyusb_driver_uninstall() == ESP_OK, "retry install cleanup failed");
}

static void test_task_stop_failure_retains_phy_until_retry(void)
{
    reset_fake();
    s_task_start_result = ESP_ERR_TIMEOUT;
    s_task_stop_error = ESP_ERR_NOT_FINISHED;
    tinyusb_config_t config = make_config();
    CHECK(tinyusb_driver_install(&config) == ESP_ERR_NOT_FINISHED,
          "cleanup error did not override failed start");
    CHECK(s_phy_delete_calls == 0U, "PHY was deleted after task-stop failure");
    CHECK(tinyusb_driver_uninstall() == ESP_OK, "task cleanup retry failed");
    CHECK(s_phy_delete_calls == 1U, "PHY was not released after successful retry");
}

static void test_invalid_state_with_initialized_stack_keeps_phy(void)
{
    reset_fake();
    tinyusb_config_t config = make_config();
    CHECK(tinyusb_driver_install(&config) == ESP_OK,
          "initialized-stack test install failed");
    s_task_stop_error = ESP_ERR_INVALID_STATE;
    CHECK(tinyusb_driver_uninstall() == ESP_ERR_INVALID_STATE,
          "INVALID_STATE task-stop was accepted while stack remained initialized");
    CHECK(s_tud_initialized && s_phy_delete_calls == 0U,
          "PHY was deleted while TinyUSB still reported initialized");
    s_task_stop_error = ESP_OK;
    CHECK(tinyusb_driver_uninstall() == ESP_OK,
          "initialized-stack cleanup retry failed");
    CHECK(!s_tud_initialized && s_phy_delete_calls == 1U,
          "successful cleanup retry did not release PHY");
}

static void test_pm_rollback_failure_blocks_handoff_until_retry(void)
{
    reset_fake();
    s_pm_init_result = ESP_FAIL;
    s_wakeup_deinit_result = ESP_ERR_TIMEOUT;
    tinyusb_config_t config = make_config();
    CHECK(tinyusb_driver_install(&config) == ESP_ERR_TIMEOUT,
          "PM rollback failure was not surfaced");
    CHECK(s_phy_delete_calls == 0U,
          "PHY was released while wakeup cleanup still failed");
    CHECK(tinyusb_driver_install(&config) == ESP_ERR_INVALID_STATE,
          "new install was allowed with PM ownership pending");
    CHECK(tinyusb_driver_uninstall() == ESP_OK, "PM ownership retry failed");
    CHECK(s_phy_delete_calls == 1U, "PHY was not released after PM retry");
}

static void test_phy_delete_failure_blocks_reinstall_until_retry(void)
{
    reset_fake();
    tinyusb_config_t config = make_config();
    CHECK(tinyusb_driver_install(&config) == ESP_OK, "PHY test install failed");
    s_phy_delete_error = ESP_FAIL;
    CHECK(tinyusb_driver_uninstall() == ESP_FAIL,
          "PHY delete error was not surfaced");
    CHECK(tinyusb_driver_install(&config) == ESP_ERR_INVALID_STATE,
          "new install was allowed while PHY deletion was pending");
    s_phy_delete_error = ESP_OK;
    CHECK(tinyusb_driver_uninstall() == ESP_OK, "PHY deletion retry failed");
    CHECK(s_phy_delete_calls == 2U, "PHY deletion retry was not attempted");
}

static void test_task_start_and_phy_cleanup_failures_keep_pending_owner(void)
{
    reset_fake();
    s_task_start_result = ESP_ERR_TIMEOUT;
    s_phy_delete_error = ESP_FAIL;
    tinyusb_config_t config = make_config();
    CHECK(tinyusb_driver_install(&config) == ESP_FAIL,
          "PHY cleanup error did not override task-start failure");
    CHECK(s_phy_delete_calls == 1U,
          "partial task-start rollback did not attempt PHY cleanup");
    CHECK(tinyusb_driver_install(&config) == ESP_ERR_INVALID_STATE,
          "reinstall proceeded with failed PHY cleanup pending");
    s_phy_delete_error = ESP_OK;
    CHECK(tinyusb_driver_uninstall() == ESP_OK,
          "partial task-start cleanup retry failed");
    CHECK(s_phy_delete_calls == 2U,
          "partial task-start cleanup retry did not retry PHY deletion");
}

static void test_phy_error_with_partial_handle_runs_rollback(void)
{
    reset_fake();
    s_phy_create_result = ESP_FAIL;
    s_phy_create_returns_handle_on_error = true;
    tinyusb_config_t config = make_config();
    CHECK(tinyusb_driver_install(&config) == ESP_FAIL,
          "PHY allocation error was not returned after clean rollback");
    CHECK(s_task_start_calls == 0U && s_phy_delete_calls == 1U,
          "partial PHY handle was not cleaned before returning");
}

int main(void)
{
    test_normal_install_uninstall_is_idempotent();
    test_task_start_failure_rolls_back_and_allows_retry();
    test_task_stop_failure_retains_phy_until_retry();
    test_invalid_state_with_initialized_stack_keeps_phy();
    test_pm_rollback_failure_blocks_handoff_until_retry();
    test_phy_delete_failure_blocks_reinstall_until_retry();
    test_task_start_and_phy_cleanup_failures_keep_pending_owner();
    test_phy_error_with_partial_handle_runs_rollback();
    puts("tinyusb_lifecycle_test: PASS (firmware overlay source with injected PHY/task/PM failures)");
    return 0;
}
