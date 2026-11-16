"""以 NVS/UART 夹具运行固件实际保存/恢复函数，不连接板卡。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class PersistenceTests(unittest.TestCase):
    def test_firmware_preferences(self):
        source = (ROOT / "firmware/main/uart0_control.c").read_text(encoding="utf-8")
        helper = source[source.index("static esp_err_t makcu_preference_open"):source.index("static esp_err_t accept_makcu_mode_transition")]
        helper = helper.rsplit("#endif", 1)[0]
        fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define DUAL_PROXY_ENABLE_MAKCU_ASCII_API 1
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define DUAL_ROLE_MOUSE_HOST 2
#define CONTROL_UART 0
#define CONTROL_BAUD_MAKCU 115200
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef int nvs_open_mode_t;
typedef int nvs_handle_t;
static int stored = -1, pending, writes, commits, init_error, commit_error, baud_error;
static int s_role, s_uart0_current_baud, s_makcu_v4_baud;
static bool s_makcu_v2_selected, s_makcu_legacy_selected, s_makcu_mode_enabled, s_makcu_runtime_requested;
static bool output, diag;
static int nvs_flash_init(void) { return init_error; }
static int nvs_open(const char *ns, int mode, int *h) { (void)ns;(void)mode;*h=1;return 0; }
static int nvs_get_u8(int h,const char *key,uint8_t *v) { (void)h;(void)key;if(stored<0)return 1;*v=(uint8_t)stored;return 0; }
static int nvs_set_u8(int h,const char *key,uint8_t v) { (void)h;(void)key;pending=v;writes++;return 0; }
static int nvs_commit(int h) { (void)h;commits++;if(commit_error)return commit_error;stored=pending;return 0; }
static void nvs_close(int h) { (void)h; }
static int uart_set_baudrate(int u,int b) { (void)u;(void)b;return baud_error; }
static void dual_uart0_output_set_makcu_mode(bool v) { output=v; }
static void dual_diag_stream_set_enabled(bool v) { diag=v; }
static void reset_ram(void) {
 s_role=2;s_uart0_current_baud=921600;s_makcu_v4_baud=0;
 s_makcu_mode_enabled=s_makcu_runtime_requested=s_makcu_legacy_selected=s_makcu_v2_selected=output=false;diag=true;
}
"""
        checks = r"""
int main(void) {
 reset_ram();restore_makcu_preference();assert(!output);
 assert(save_makcu_preference(true,false,false)==0);assert(stored==4 && commits==1);
 assert(save_makcu_preference(true,false,false)==0);assert(writes==1 && commits==1);
 reset_ram();restore_makcu_preference();assert(output && !diag && s_makcu_mode_enabled && s_makcu_runtime_requested && !s_makcu_legacy_selected && s_uart0_current_baud==115200);
 assert(save_makcu_preference(true,true,false)==0);reset_ram();restore_makcu_preference();assert(output && s_makcu_legacy_selected);
 reset_ram();s_role=1;restore_makcu_preference();assert(!output && s_uart0_current_baud==921600);
 assert(save_makcu_preference(false,false,false)==0);reset_ram();restore_makcu_preference();assert(!output && stored==0);
 assert(save_makcu_preference(true,false,true)==0);assert(stored==2);reset_ram();restore_makcu_preference();assert(output && s_makcu_v2_selected && !s_makcu_legacy_selected);
 stored=99;reset_ram();restore_makcu_preference();assert(!output);
 init_error=7;assert(save_makcu_preference(true,false,false)==7);restore_makcu_preference();assert(!output);init_error=0;
 commit_error=8;assert(save_makcu_preference(true,false,false)==8);assert(stored==99);commit_error=0;
 stored=4;baud_error=9;reset_ram();restore_makcu_preference();assert(!output && !s_makcu_mode_enabled);
 puts("Makcu persistence fixture: PASS (off/V4/V3/V2, role, invalid, NVS/baud failures, dedup)");
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as temp:
            c = Path(temp) / "test.c"
            exe = Path(temp) / "test.exe"
            c.write_text(fixture + helper + checks, encoding="utf-8")
            subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn("Makcu persistence fixture: PASS", result.stdout)
            print(result.stdout.strip())

if __name__ == "__main__":
    unittest.main()
