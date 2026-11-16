"""用捕获 UART/调度队列的夹具执行实际 V2 解析器与命令处理源码，不连接硬件。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class V2RuntimeTests(unittest.TestCase):
    def test_runtime(self):
        source = (ROOT / 'firmware/main/uart0_control.c').read_text(encoding='utf-8')
        def function(name):
            import re
            match = re.search(r'^static [^\n]+\b' + name + r'\([^;]*?\)\n\{', source, re.M)
            self.assertIsNotNone(match, name)
            start = match.start(); end = source.index('\n}\n', match.end()) + 3
            return source[start:end]
        preamble = r'''
#include "makcu_v2_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#define DUAL_ROLE_MOUSE_HOST 2
#define CONTROL_BAUD_MAKCU 115200
static uint8_t s_role=2, s_makcu_v4_transient_buttons;
static uint32_t s_makcu_v4_baud=115200;
static makcu_v4_state_t s_makcu_v4_state;
static uint8_t output[100]; static size_t output_length;
static bool queue_accept=true; static int mx,my,mw,click_button,click_count,click_hold,submitted;
static int makcu_v4_write(const uint8_t *p,size_t n) { memcpy(output,p,n);output_length=n;return 0; }
static uint8_t makcu_v4_software_buttons(void) {return s_makcu_v4_state.injected_buttons | s_makcu_v4_transient_buttons;}
static void makcu_v4_sync_pointer(makcu_v4_state_t *s) {(void)s;}
static uint32_t makcu_v4_now_ms(void) {return 10;}
static void makcu_v4_get_stream_state(bool *a,uint8_t *b,uint16_t *c) {*a=false;*b=0;*c=0;}
static void makcu_v4_set_subscriptions(bool a,uint8_t b,uint16_t c) {(void)a;(void)b;(void)c;}
static void makcu_v4_set_masks(makcu_v4_state_t *s,uint8_t a,uint8_t b,uint8_t c) {(void)s;(void)a;(void)b;(void)c;}
static void makcu_v4_set_lock(makcu_v4_state_t *s,uint8_t a,bool b) {(void)s;(void)a;(void)b;}
static bool makcu_v4_enqueue_move(int32_t x,int32_t y,int32_t w,uint32_t t,bool n) {(void)t;(void)n;mx=x;my=y;mw=w;return queue_accept;}
static bool makcu_v4_enqueue_click(uint8_t b,uint8_t n,uint16_t h,uint32_t t) {(void)t;click_button=b;click_count=n;click_hold=h;return queue_accept;}
static bool makcu_v4_enqueue_silent(int32_t x,int32_t y,uint32_t t) {(void)x;(void)y;(void)t;return queue_accept;}
static bool makcu_v4_has_pending_move(void) {return false;}
static void makcu_v4_projected_pointer(int32_t *x,int32_t *y) {*x=s_makcu_v4_state.pointer_x;*y=s_makcu_v4_state.pointer_y;}
static void makcu_v4_set_screen_and_pointer(uint16_t w,uint16_t h,uint16_t x,uint16_t y) {s_makcu_v4_state.screen_width=w;s_makcu_v4_state.screen_height=h;s_makcu_v4_state.pointer_x=x;s_makcu_v4_state.pointer_y=y;}
static void makcu_v4_submit_mouse_report(uint8_t b,int16_t x,int16_t y,int8_t w,int8_t p,uint8_t n) {(void)b;(void)x;(void)y;(void)w;(void)p;(void)n;submitted++;}
'''
        names=['makcu_v4_read_u16','makcu_v4_read_i16','makcu_v4_write_u16','makcu_v4_write_u32',
               'makcu_v4_send_ascii_echo','makcu_v4_send_error','makcu_v4_send_binary','makcu_v4_send_ascii',
               'makcu_v4_ascii_echo_enabled_for','makcu_v4_send_ascii_query','makcu_v4_finish_ascii_setter','makcu_v4_handle_command']
        # send_ascii_echo 没有依赖其他响应函数，可放在前面。
        checks=r'''
static void callback(const makcu_v4_command_t *c,void *ctx) {makcu_v4_handle_command(c,ctx);}
static void request(uint8_t op,const uint8_t *data,uint16_t n) {
 makcu_v2_parser_t p;makcu_v2_parser_init(&p);uint8_t frame[100];size_t len;
 assert(makcu_v2_encode_frame(op,data,n,frame,sizeof(frame),&len));output_length=0;
 for(size_t i=0;i<len;i++)makcu_v2_parser_feed(&p,frame[i],1,callback,NULL);
 assert(output_length>=5 && output[0]==0x50 && output[1]==op && output_length==(size_t)output[2]+4);
}
static void status(uint8_t value) {assert(output_length==5 && output[2]==1 && output[4]==value);}
int main(void) {
 makcu_v4_state_init(&s_makcu_v4_state);
 uint8_t move[]={20,0,0xFB,0xFF};request(0x0D,move,4);status(0);assert(mx==20 && my==-5);
 queue_accept=false;request(0x0D,move,4);status(1);queue_accept=true;
 uint8_t wheel=0x80;request(0x18,&wheel,1);status(0);assert(mw==-128);
 uint8_t press=1;request(0x08,&press,1);status(0);assert(submitted==1 && s_makcu_v4_state.injected_buttons==1);
 s_makcu_v4_state.physical_buttons=1;request(0x08,NULL,0);assert(output[4]==3);
 uint8_t silent=2;request(0x08,&silent,1);status(0);assert(submitted==1 && s_makcu_v4_state.injected_buttons==0);
 request(0x08,NULL,0);assert(output[4]==1);
 uint8_t release=0;request(0x08,&release,1);status(0);assert(submitted==2);
 uint8_t click[]={5,2,200};request(0x04,click,3);status(0);assert(click_button==4 && click_count==2 && click_hold==200);
 uint8_t screen[]={0,4,0,3};request(0xBD,screen,4);status(0);request(0xBD,NULL,0);assert(output_length==8 && memcmp(output+4,screen,4)==0);
 request(0x05,NULL,0);assert(output_length==8);
 request(0xB1,NULL,0);assert(output_length==8 && output[4]==0 && output[5]==194 && output[6]==1);
 uint8_t unsupported_baud[]={0,9,61,0};request(0xB1,unsupported_baud,4);status(1);
 request(0xB3,NULL,0);assert(output[4]==2);
 request(0xBF,NULL,0);assert(output_length==17 && memcmp(output+4,"HID-Bridge-V2",13)==0);
 request(0xA2,&press,1);status(1);
 s_role=1;request(0x0D,move,4);status(1);
 puts("Makcu V2 runtime fixture: PASS (actual responses, buttons, movement, queue failure, queries, role)");
}
'''
        with tempfile.TemporaryDirectory() as temp:
            c=Path(temp)/'fixture.c';exe=Path(temp)/'fixture.exe'
            c.write_text(preamble+'\n'.join(function(n) for n in names)+checks,encoding='utf-8')
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'firmware/main'),str(c),str(ROOT/'firmware/main/makcu_v2_logic.c'),str(ROOT/'firmware/main/makcu_v4_logic.c'),'-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
            self.assertIn('Makcu V2 runtime fixture: PASS',result.stdout)
            print(result.stdout.strip())
if __name__=='__main__': unittest.main()
