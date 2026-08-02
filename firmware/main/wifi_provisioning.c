#include "wifi_provisioning.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "wifi_manager.h"

#define DNS_PORT 53
#define DNS_PACKET_MAX 512
#define MAX_SCAN_RESULTS 24
#define DHCPS_OFFER_DNS 0x02

static const char *TAG = "wifi_provisioning";
static const char *CAPTIVE_PORTAL_URI = "http://192.168.4.1/";
static httpd_handle_t s_http_server;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_running;
static bool s_running;

static const char s_index_html[] =
    "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>HID Bridge 配网</title><style>"
    "body{font-family:system-ui,sans-serif;background:#111827;color:#e5e7eb;margin:0;padding:24px}"
    "main{max-width:520px;margin:5vh auto;background:#1f2937;padding:24px;border-radius:16px}"
    "h1{font-size:24px;margin-top:0}label{display:block;margin:16px 0 6px}"
    "select,input,button{box-sizing:border-box;width:100%;padding:12px;border-radius:8px;border:1px solid #4b5563;font-size:16px}"
    "select,input{background:#111827;color:#fff}button{margin-top:18px;background:#22c55e;color:#052e16;border:0;font-weight:700}"
    ".scan-row{display:flex;gap:8px}.scan-row select{flex:1}.scan-row button{width:auto;margin:0;background:#374151;color:#fff;white-space:nowrap}"
    "button:disabled{opacity:.55}.muted{color:#9ca3af;font-size:14px}.status{min-height:24px;margin-top:14px}"
    "</style></head><body><main><h1>HID Bridge Wi-Fi 配置</h1>"
    "<p class=muted>选择新的 2.4 GHz 网络。保存后开发板会自动连接并关闭此临时热点。</p>"
    "<form id=f><label for=n>扫描到的 Wi-Fi 网络</label><div class=scan-row><select id=n><option value=''>正在扫描…</option></select><button id=r type=button>重新扫描</button></div>"
    "<label for=s>网络名称（SSID）</label><input id=s name=ssid maxlength=32 required placeholder='选择上方网络，或在此手动输入'>"
    "<label for=p>Wi-Fi 密码</label><input id=p name=password type=password maxlength=63 autocomplete=current-password placeholder='开放网络请留空'>"
    "<button id=b>保存并连接</button><div class=status id=m></div></form>"
    "<script>const s=document.querySelector('#s'),n=document.querySelector('#n'),m=document.querySelector('#m'),b=document.querySelector('#b'),r=document.querySelector('#r');"
    "n.onchange=()=>{if(n.value)s.value=n.value};r.onclick=()=>scan();"
    "async function scan(){let c=new AbortController(),t=setTimeout(()=>c.abort(),15000);r.disabled=true;n.disabled=true;n.innerHTML='<option value=\"\">正在扫描…</option>';m.textContent='正在扫描附近的 2.4 GHz 网络…';"
    "try{let q=await fetch('/api/networks',{cache:'no-store',signal:c.signal});if(!q.ok)throw new Error(await q.text());let a=await q.json();n.innerHTML='';"
    "let d=document.createElement('option');d.value='';d.textContent=a.length?'请选择网络':'未扫描到网络';n.append(d);"
    "a.forEach(x=>{let o=document.createElement('option');o.value=x.ssid;o.textContent=x.ssid+'（'+x.rssi+' dBm'+(x.open?'，开放网络':'')+'）';n.append(o)});"
    "m.textContent=a.length?'已扫描到 '+a.length+' 个网络。请选择，或手动输入 SSID。':'未扫描到网络，可手动输入 SSID。'}"
    "catch(e){n.innerHTML='<option value=\"\">扫描失败</option>';m.textContent=e.name==='AbortError'?'扫描超时，请点击重新扫描，或手动输入 SSID。':'扫描失败：'+(e.message||'未知错误')+'。也可手动输入 SSID。'}"
    "finally{clearTimeout(t);r.disabled=false;n.disabled=false}}scan();"
    "document.querySelector('#f').onsubmit=async e=>{e.preventDefault();b.disabled=true;m.textContent='正在保存并连接…';"
    "try{let r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(e.target))});"
    "m.textContent=await r.text();if(!r.ok){b.disabled=false;return}for(let i=0;i<12;i++){await new Promise(x=>setTimeout(x,1000));"
    "let q=await fetch('/api/status').then(x=>x.json());if(q.connected){m.textContent='连接成功，新地址：'+q.ip+'。请把它填入主机 wiFiHost。';return}}"
    "m.textContent='已保存，开发板仍在尝试连接。';b.disabled=false}catch(x){m.textContent='连接已切换，请在新网络中查找开发板'}};"
    "</script></main></body></html>";

static esp_err_t send_page(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_handler(httpd_req_t *request)
{
    return send_page(request);
}

static esp_err_t not_found_handler(httpd_req_t *request, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, "请打开配网页面", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_escaped(httpd_req_t *request, const char *value)
{
    char escaped[7];
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (*cursor == '"' || *cursor == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)*cursor;
            escaped[2] = '\0';
        } else if (*cursor < 0x20) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
        } else {
            escaped[0] = (char)*cursor;
            escaped[1] = '\0';
        }
        esp_err_t result = httpd_resp_sendstr_chunk(request, escaped);
        if (result != ESP_OK) {
            return result;
        }
    }
    return ESP_OK;
}

static esp_err_t networks_handler(httpd_req_t *request)
{
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };
    esp_err_t result = esp_wifi_scan_start(&scan_config, true);
    if (result != ESP_OK) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        return httpd_resp_send(request, "Wi-Fi 正忙，请稍后重试", HTTPD_RESP_USE_STRLEN);
    }

    uint16_t count = 0;
    result = esp_wifi_scan_get_ap_num(&count);
    if (result != ESP_OK) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        return httpd_resp_send(request, "无法读取 Wi-Fi 扫描结果", HTTPD_RESP_USE_STRLEN);
    }
    if (count > MAX_SCAN_RESULTS) {
        count = MAX_SCAN_RESULTS;
    }
    wifi_ap_record_t *records = count == 0 ? NULL : calloc(count, sizeof(wifi_ap_record_t));
    if (count > 0 && records == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_ERR_NO_MEM;
    }
    if (count > 0) {
        result = esp_wifi_scan_get_ap_records(&count, records);
        if (result != ESP_OK) {
            free(records);
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "读取扫描结果失败");
            return result;
        }
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(request, "[");
    uint16_t emitted = 0;
    for (uint16_t index = 0; index < count; index++) {
        if (records[index].ssid[0] == '\0') {
            continue;
        }
        if (emitted > 0) {
            httpd_resp_sendstr_chunk(request, ",");
        }
        httpd_resp_sendstr_chunk(request, "{\"ssid\":\"");
        char scanned_ssid[33];
        memcpy(scanned_ssid, records[index].ssid, 32);
        scanned_ssid[32] = '\0';
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            send_json_escaped(request, scanned_ssid));
        char details[64];
        snprintf(
            details,
            sizeof(details),
            "\",\"rssi\":%d,\"open\":%s}",
            records[index].rssi,
            records[index].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        httpd_resp_sendstr_chunk(request, details);
        emitted++;
    }
    free(records);
    httpd_resp_sendstr_chunk(request, "]");
    return httpd_resp_sendstr_chunk(request, NULL);
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = (char)tolower((unsigned char)value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static bool url_decode(char *value)
{
    char *read_cursor = value;
    char *write_cursor = value;
    while (*read_cursor != '\0') {
        if (*read_cursor == '+') {
            *write_cursor++ = ' ';
            read_cursor++;
        } else if (*read_cursor == '%' && read_cursor[1] != '\0' && read_cursor[2] != '\0') {
            int high = hex_value(read_cursor[1]);
            int low = hex_value(read_cursor[2]);
            if (high < 0 || low < 0) {
                return false;
            }
            *write_cursor++ = (char)((high << 4) | low);
            read_cursor += 3;
        } else {
            *write_cursor++ = *read_cursor++;
        }
    }
    *write_cursor = '\0';
    return true;
}

static bool parse_form(char *body, char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    bool found_ssid = false;
    char *field = body;
    while (field != NULL) {
        char *next = strchr(field, '&');
        if (next != NULL) {
            *next++ = '\0';
        }
        char *separator = strchr(field, '=');
        if (separator != NULL) {
            *separator++ = '\0';
            if (!url_decode(field) || !url_decode(separator)) {
                return false;
            }
            if (strcmp(field, "ssid") == 0) {
                if (strlen(separator) >= ssid_size) {
                    return false;
                }
                strlcpy(ssid, separator, ssid_size);
                found_ssid = true;
            } else if (strcmp(field, "password") == 0) {
                if (strlen(separator) >= password_size) {
                    return false;
                }
                strlcpy(password, separator, password_size);
            }
        }
        field = next;
    }
    return found_ssid;
}

static esp_err_t config_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len >= 256) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "配置内容无效");
        return ESP_ERR_INVALID_ARG;
    }
    char body[256];
    size_t received = 0;
    while (received < request->content_len) {
        int chunk = httpd_req_recv(request, body + received, request->content_len - received);
        if (chunk <= 0) {
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "读取配置失败");
            return ESP_FAIL;
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char password[64] = {0};
    if (!parse_form(body, ssid, sizeof(ssid), password, sizeof(password))) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "SSID 或表单编码无效");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = wifi_manager_apply_credentials(ssid, password);
    if (result != ESP_OK) {
        httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "SSID 不能为空；受保护网络密码必须为 8 至 63 字节");
        return result;
    }
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_send(
        request,
        "配置已保存。开发板正在连接新网络，成功后此热点会自动关闭。",
        HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    char ip_address[16] = {0};
    bool connected = wifi_manager_get_ip(ip_address, sizeof(ip_address)) == ESP_OK;
    char response[80];
    snprintf(
        response,
        sizeof(response),
        "{\"connected\":%s,\"ip\":\"%s\"}",
        connected ? "true" : "false",
        ip_address);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static void dns_server_task(void *argument)
{
    (void)argument;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "无法创建 DNS 套接字");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(TAG, "无法监听 DNS 端口");
        close(socket_fd);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t packet[DNS_PACKET_MAX];
    while (s_dns_running) {
        struct sockaddr_in client;
        socklen_t client_length = sizeof(client);
        int length = recvfrom(
            socket_fd,
            packet,
            sizeof(packet) - 16,
            0,
            (struct sockaddr *)&client,
            &client_length);
        if (length < 12) {
            continue;
        }
        size_t question_end = 12;
        while (question_end < (size_t)length && packet[question_end] != 0) {
            question_end += (size_t)packet[question_end] + 1;
        }
        question_end += 5;
        if (question_end > (size_t)length || question_end + 16 > sizeof(packet)) {
            continue;
        }
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = 1;
        packet[8] = packet[9] = packet[10] = packet[11] = 0;
        uint8_t answer[] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
            192, 168, 4, 1,
        };
        memcpy(packet + question_end, answer, sizeof(answer));
        sendto(
            socket_fd,
            packet,
            question_end + sizeof(answer),
            0,
            (struct sockaddr *)&client,
            client_length);
    }
    close(socket_fd);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void configure_captive_portal_dhcp(void)
{
    esp_netif_t *access_point = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (access_point == NULL) {
        return;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(access_point, &ip_info) != ESP_OK) {
        return;
    }
    esp_netif_dns_info_t dns = {0};
    dns.ip.u_addr.ip4.addr = ip_info.ip.addr;
    dns.ip.type = IPADDR_TYPE_V4;
    uint8_t offer_dns = DHCPS_OFFER_DNS;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(access_point));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_netif_dhcps_option(
            access_point,
            ESP_NETIF_OP_SET,
            ESP_NETIF_DOMAIN_NAME_SERVER,
            &offer_dns,
            sizeof(offer_dns)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_netif_set_dns_info(access_point, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_netif_dhcps_option(
            access_point,
            ESP_NETIF_OP_SET,
            ESP_NETIF_CAPTIVEPORTAL_URI,
            (void *)CAPTIVE_PORTAL_URI,
            strlen(CAPTIVE_PORTAL_URI)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(access_point));
}

esp_err_t wifi_provisioning_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    configure_captive_portal_dhcp();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    esp_err_t result = httpd_start(&s_http_server, &config);
    if (result != ESP_OK) {
        return result;
    }
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
    const httpd_uri_t networks = {
        .uri = "/api/networks",
        .method = HTTP_GET,
        .handler = networks_handler,
    };
    const httpd_uri_t credentials = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = config_handler,
    };
    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(s_http_server, &root));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(s_http_server, &networks));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(s_http_server, &credentials));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(s_http_server, &status));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND, not_found_handler));

    s_dns_running = true;
    BaseType_t created = xTaskCreate(dns_server_task, "provision_dns", 4096, NULL, 4, &s_dns_task);
    if (created != pdPASS) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        s_dns_running = false;
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    return ESP_OK;
}

void wifi_provisioning_stop(void)
{
    if (!s_running) {
        return;
    }
    s_dns_running = false;
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    s_running = false;
}

bool wifi_provisioning_is_running(void)
{
    return s_running;
}
