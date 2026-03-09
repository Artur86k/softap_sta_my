/*
 * ESP32 SoftAP + STA with Web Configuration Portal
 *
 * Serves a password-protected config page on the AP interface.
 * Login: admin / 123456
 * AP SSID: ESP32-Config  AP Password: esp32config
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define AP_SSID          "ESP32-Config"
#define AP_PASS          "esp32config"
#define AP_CHANNEL       1
#define AP_MAX_CONN      4

#define WEB_USERNAME     "admin"
#define WEB_PASSWORD     "123456"

#define NVS_NAMESPACE    "wifi_cfg"
#define NVS_KEY_SSID     "ssid"
#define NVS_KEY_PASS     "password"

#define MAX_STA_RETRY    5
#define MAX_SCAN_APS     20
#define SESSION_HEX_LEN  33   /* 16 bytes -> 32 hex chars + NUL */

static const char *TAG = "WiFiCfg";

/* --------------------------------------------------------------------------
 * Global state
 * -------------------------------------------------------------------------- */
typedef enum {
    STA_IDLE = 0,
    STA_CONNECTING,
    STA_CONNECTED,
    STA_FAILED,
} sta_state_t;

static sta_state_t      s_sta_state       = STA_IDLE;
static char             s_sta_ssid[33]    = {0};
static char             s_sta_ip[16]      = {0};
static int              s_retry_num       = 0;
static bool             s_internet_ok     = false;

static char             s_session[SESSION_HEX_LEN] = {0};
static bool             s_session_valid   = false;

static wifi_ap_record_t *s_scan_aps       = NULL;
static uint16_t         s_scan_count      = 0;

static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONN_BIT   BIT0
#define WIFI_FAIL_BIT   BIT1

/* AP netif handle — needed to enable NAPT after STA gets an IP */
static esp_netif_t *s_netif_ap = NULL;

/* --------------------------------------------------------------------------
 * WiFi event handler
 * -------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = data;
            ESP_LOGI(TAG, "AP: client joined  " MACSTR, MAC2STR(e->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = data;
            ESP_LOGI(TAG, "AP: client left  " MACSTR, MAC2STR(e->mac));
            break;
        }
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA: started");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = data;
            ESP_LOGI(TAG, "STA: disconnected (reason %d)", e->reason);
            if (s_sta_state == STA_CONNECTING || s_sta_state == STA_CONNECTED) {
                s_sta_state = STA_CONNECTING;
                s_internet_ok = false;
                if (s_netif_ap) esp_netif_napt_disable(s_netif_ap);
                if (s_retry_num < MAX_STA_RETRY) {
                    s_retry_num++;
                    ESP_LOGI(TAG, "STA: retry %d/%d", s_retry_num, MAX_STA_RETRY);
                    esp_wifi_connect();
                } else {
                    ESP_LOGW(TAG, "STA: max retries reached");
                    s_sta_state = STA_FAILED;
                    xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
                }
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "STA: got IP %s", s_sta_ip);
        s_retry_num = 0;
        s_sta_state = STA_CONNECTED;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONN_BIT);
        /* Enable NAPT now that STA has a valid IP and default route */
        if (s_netif_ap) {
            if (esp_netif_napt_enable(s_netif_ap) == ESP_OK) {
                ESP_LOGI(TAG, "NAPT enabled — AP clients can reach internet");
            } else {
                ESP_LOGW(TAG, "NAPT enable failed");
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * NVS helpers
 * -------------------------------------------------------------------------- */
static esp_err_t nvs_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_load_wifi(char *ssid, size_t ssid_sz,
                                char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_sz);
    if (err == ESP_OK)
        err = nvs_get_str(h, NVS_KEY_PASS, pass, &pass_sz);
    nvs_close(h);
    return err;
}

/* --------------------------------------------------------------------------
 * Session management
 * -------------------------------------------------------------------------- */
static void session_generate(void)
{
    uint8_t raw[16];
    for (int i = 0; i < 16; i++) raw[i] = esp_random() & 0xFF;
    for (int i = 0; i < 16; i++)
        snprintf(s_session + i * 2, 3, "%02x", raw[i]);
    s_session_valid = true;
}

static bool session_check(httpd_req_t *req)
{
    if (!s_session_valid) return false;
    char cookies[256];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookies, sizeof(cookies)) != ESP_OK)
        return false;
    char expected[64];
    snprintf(expected, sizeof(expected), "session=%s", s_session);
    return strstr(cookies, expected) != NULL;
}

/* --------------------------------------------------------------------------
 * Internet check task  (runs every 5 s; TCP connect to 8.8.8.8:53)
 * -------------------------------------------------------------------------- */
static void internet_check_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (s_sta_state != STA_CONNECTED) {
            s_internet_ok = false;
            continue;
        }
        struct sockaddr_in addr = {
            .sin_family      = AF_INET,
            .sin_port        = htons(53),
            .sin_addr.s_addr = inet_addr("8.8.8.8"),
        };
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { s_internet_ok = false; continue; }
        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int r = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
        close(sock);
        s_internet_ok = (r == 0);
        ESP_LOGD(TAG, "Internet: %s", s_internet_ok ? "OK" : "no");
    }
}

/* --------------------------------------------------------------------------
 * STA connection helper
 * -------------------------------------------------------------------------- */
static esp_err_t sta_connect(const char *ssid, const char *pass)
{
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    s_retry_num   = 0;
    s_sta_state   = STA_CONNECTING;
    s_internet_ok = false;
    memset(s_sta_ip, 0, sizeof(s_sta_ip));

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.scan_method       = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.threshold.authmode =
        (strlen(pass) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);

    return esp_wifi_connect();
}

/* --------------------------------------------------------------------------
 * URL / form helpers
 * -------------------------------------------------------------------------- */
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t si = 0, di = 0;
    while (src[si] && di < dst_size - 1) {
        if (src[si] == '+') {
            dst[di++] = ' '; si++;
        } else if (src[si] == '%' && src[si+1] && src[si+2]) {
            char hex[3] = { src[si+1], src[si+2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 3;
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = 0;
}

static void form_value(const char *body, const char *key,
                       char *dst, size_t dst_size)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { dst[0] = 0; return; }
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= dst_size) len = dst_size - 1;
    char enc[512]; memset(enc, 0, sizeof(enc));
    strncpy(enc, p, len < sizeof(enc)-1 ? len : sizeof(enc)-1);
    url_decode(dst, enc, dst_size);
}

/* --------------------------------------------------------------------------
 * HTML assets
 * -------------------------------------------------------------------------- */

/* ---- Login page (split into two chunks so no snprintf touches % in CSS) ---- */
static const char LOGIN_PRE[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 Login</title>"
"<style>"
"body{font-family:Arial,sans-serif;background:#1a1a2e;display:flex;"
"justify-content:center;align-items:center;min-height:100vh;margin:0}"
".box{background:#16213e;padding:2rem;border-radius:12px;"
"box-shadow:0 8px 32px rgba(0,0,0,.5);width:300px;max-width:90vw}"
"h2{color:#e94560;text-align:center;margin:0 0 1.5rem}"
"label{color:#aaa;font-size:.85rem;display:block;margin-bottom:.25rem}"
"input{width:100%;padding:.65rem;border:1px solid #0f3460;border-radius:6px;"
"background:#0f3460;color:#eee;box-sizing:border-box;margin-bottom:1rem}"
"button{width:100%;padding:.75rem;background:#e94560;color:#fff;"
"border:none;border-radius:6px;cursor:pointer;font-size:1rem}"
"button:hover{background:#c73652}"
".err{color:#e94560;text-align:center;margin-bottom:.75rem;font-size:.9rem}"
"</style></head><body><div class='box'>"
"<h2>&#x1F4F6; ESP32 Config</h2>";
/* optional error div is sent between PRE and POST */
static const char LOGIN_POST[] =
"<form method='POST' action='/login'>"
"<label>Username</label>"
"<input type='text' name='username' autocomplete='username'>"
"<label>Password</label>"
"<input type='password' name='password' autocomplete='current-password'>"
"<button type='submit'>Login</button>"
"</form></div></body></html>";

/* ---- Config page CSS + header ---- */
static const char CFG_HEAD[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>WiFi Config</title><style>"
"*{box-sizing:border-box}"
"body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;"
"margin:0;padding:1rem}"
"h1{color:#e94560;text-align:center;margin:0 0 1.5rem}"
"h3{color:#4fc3f7;margin:0 0 .75rem;font-size:1rem;text-transform:uppercase;"
"letter-spacing:.05em}"
".card{background:#16213e;border-radius:12px;padding:1.25rem;"
"margin-bottom:1rem;box-shadow:0 4px 16px rgba(0,0,0,.35)}"
"label{font-size:.82rem;color:#aaa;display:block;margin-bottom:.2rem}"
"input[type=text],input[type=password]{width:100%;padding:.6rem;"
"border:1px solid #0f3460;border-radius:6px;background:#0f3460;"
"color:#eee;margin-bottom:.65rem}"
".btn{padding:.6rem 1.1rem;border:none;border-radius:6px;cursor:pointer;"
"font-size:.9rem;transition:background .15s}"
".btn-primary{background:#e94560;color:#fff}.btn-primary:hover{background:#c73652}"
".btn-sec{background:#0f3460;color:#eee}.btn-sec:hover{background:#1a4a8a}"
".btn-scan{background:#00b4d8;color:#fff;width:100%;margin-bottom:.75rem}"
".btn-scan:hover{background:#0096b4}"
".nets{max-height:220px;overflow-y:auto;margin-bottom:.75rem}"
".net{display:flex;justify-content:space-between;align-items:center;"
"padding:.45rem .75rem;border:1px solid #0f3460;border-radius:6px;"
"margin-bottom:.35rem;cursor:pointer;transition:background .15s}"
".net:hover,.net.sel{background:#0f3460;border-color:#4fc3f7}"
".nname{font-weight:bold}.nrssi{font-size:.78rem;color:#aaa}"
".nlock{color:#ffd700;margin-left:.35rem;font-size:.8rem}"
".sbox{border-radius:8px;padding:.65rem .9rem;margin-bottom:.5rem}"
".s-ok{background:#0d4f2e;border:1px solid #2d9e5f}"
".s-no{background:#4f0d0d;border:1px solid #9e2d2d}"
".s-wait{background:#4f3d0d;border:1px solid #9e7d2d}"
".slbl{font-size:.72rem;color:#aaa;text-transform:uppercase;"
"letter-spacing:.04em;margin-bottom:.2rem}"
".badge{display:inline-block;padding:.2rem .55rem;border-radius:20px;"
"font-size:.78rem;font-weight:bold;margin-right:.4rem}"
".b-ok{background:#2d9e5f;color:#fff}"
".b-no{background:#9e2d2d;color:#fff}"
".b-wait{background:#9e7d2d;color:#fff}"
"#spin{display:none;text-align:center;color:#aaa;padding:.75rem;"
"font-size:.85rem}"
"#cmsg{margin-top:.6rem;font-size:.88rem;display:none}"
".row{display:flex;gap:.5rem}"
".logout{font-size:.8rem;color:#aaa;text-align:right;margin-bottom:.5rem}"
".logout a{color:#e94560;text-decoration:none}"
"</style></head><body>";

/* ---- Config page JavaScript ---- */
static const char CFG_JS[] =
"<script>"
"var nets=[];"
"function doScan(){"
"document.getElementById('spin').style.display='block';"
"document.getElementById('nlist').innerHTML='';"
"fetch('/api/scan',{method:'POST'})"
".then(function(r){return r.json();})"
".then(function(d){"
"nets=d;"
"var h='';"
"for(var i=0;i<d.length;i++){"
"var n=d[i];"
"h+='<div class=\"net\" onclick=\"selNet('+i+')\">';"
"h+='<span class=\"nname\">'+n.ssid+'</span>';"
"h+='<span><span class=\"nrssi\">'+n.rssi+' dBm</span>';"
"if(n.auth>0)h+='<span class=\"nlock\">&#x1F512;</span>';"
"h+='</span></div>';"
"}"
"document.getElementById('nlist').innerHTML=h||'<div style=\"color:#aaa;padding:.5rem\">No networks found</div>';"
"document.getElementById('spin').style.display='none';"
"})"
".catch(function(e){"
"document.getElementById('spin').style.display='none';"
"alert('Scan failed: '+e);"
"});"
"}"
"function selNet(i){"
"var els=document.querySelectorAll('.net');"
"for(var j=0;j<els.length;j++)els[j].classList.remove('sel');"
"if(els[i])els[i].classList.add('sel');"
"document.getElementById('ssid').value=nets[i].ssid;"
"document.getElementById('pass').focus();"
"}"
"function doConn(){"
"var ssid=document.getElementById('ssid').value.trim();"
"var pass=document.getElementById('pass').value;"
"if(!ssid){alert('Please enter or select an SSID');return;}"
"var msg=document.getElementById('cmsg');"
"msg.style.display='block';msg.style.color='#ffd700';"
"msg.textContent='Connecting\u2026';"
"fetch('/api/connect',{"
"method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass)"
"})"
".then(function(r){return r.json();})"
".then(function(d){"
"msg.style.color=d.success?'#2d9e5f':'#e94560';"
"msg.textContent=d.message;"
"if(d.success)setTimeout(function(){location.reload();},4000);"
"})"
".catch(function(e){msg.style.color='#e94560';msg.textContent='Error: '+e;});"
"}"
"function poll(){"
"fetch('/api/status')"
".then(function(r){return r.json();})"
".then(function(d){"
"var wb=document.getElementById('wbadge');"
"var wt=document.getElementById('wtext');"
"var ib=document.getElementById('ibadge');"
"var it=document.getElementById('itext');"
"var ip=document.getElementById('wip');"
"if(d.wifi==='connected'){"
"wb.className='badge b-ok';wb.textContent='Connected';"
"wt.textContent=d.ssid;"
"ip.textContent=d.ip?'IP: '+d.ip:'';ip.style.display=d.ip?'block':'none';"
"}else if(d.wifi==='connecting'){"
"wb.className='badge b-wait';wb.textContent='Connecting\u2026';"
"wt.textContent=d.ssid;"
"ip.style.display='none';"
"}else{"
"wb.className='badge b-no';wb.textContent='Disconnected';"
"wt.textContent='Not connected';ip.style.display='none';"
"}"
"if(d.internet){"
"ib.className='badge b-ok';ib.textContent='Available';"
"it.textContent='';it.style.display='none';"
"}else{"
"ib.className='badge b-no';ib.textContent='Not available';"
"it.textContent='No route to internet';it.style.display='block';"
"}"
"}).catch(function(){});"
"}"
"setInterval(poll,5000);"
"</script>";

/* --------------------------------------------------------------------------
 * HTTP handlers
 * -------------------------------------------------------------------------- */

/* GET / */
static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location",
                       session_check(req) ? "/config" : "/login");
    return httpd_resp_send(req, NULL, 0);
}

/* GET /login */
static esp_err_t h_login_get(httpd_req_t *req)
{
    if (session_check(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/config");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, LOGIN_PRE);
    httpd_resp_sendstr_chunk(req, LOGIN_POST);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* POST /login */
static esp_err_t h_login_post(httpd_req_t *req)
{
    char body[256];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = 0;

    char username[64], password[64];
    form_value(body, "username", username, sizeof(username));
    form_value(body, "password", password, sizeof(password));

    if (strcmp(username, WEB_USERNAME) == 0 &&
        strcmp(password, WEB_PASSWORD) == 0)
    {
        session_generate();
        char cookie[80];
        snprintf(cookie, sizeof(cookie),
                 "session=%s; Path=/; HttpOnly", s_session);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        httpd_resp_set_hdr(req, "Location", "/config");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, LOGIN_PRE);
    httpd_resp_sendstr_chunk(req, "<div class='err'>&#x26A0; Invalid username or password</div>");
    httpd_resp_sendstr_chunk(req, LOGIN_POST);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* GET /logout */
static esp_err_t h_logout(httpd_req_t *req)
{
    s_session_valid = false;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Set-Cookie",
                       "session=; Path=/; Max-Age=0; HttpOnly");
    httpd_resp_set_hdr(req, "Location", "/login");
    return httpd_resp_send(req, NULL, 0);
}

/* GET /config */
static esp_err_t h_config(httpd_req_t *req)
{
    if (!session_check(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, CFG_HEAD);

    /* Logout link */
    httpd_resp_sendstr_chunk(req,
        "<h1>&#x1F4F6; WiFi Configuration</h1>"
        "<div class='logout'><a href='/logout'>Logout</a></div>");

    /* --- Status card --- */
    const char *w_cls, *w_badge_cls, *w_badge_txt;
    switch (s_sta_state) {
    case STA_CONNECTED:
        w_cls = "s-ok"; w_badge_cls = "b-ok"; w_badge_txt = "Connected";
        break;
    case STA_CONNECTING:
        w_cls = "s-wait"; w_badge_cls = "b-wait"; w_badge_txt = "Connecting\xe2\x80\xa6";
        break;
    case STA_FAILED:
        w_cls = "s-no"; w_badge_cls = "b-no"; w_badge_txt = "Failed";
        break;
    default:
        w_cls = "s-no"; w_badge_cls = "b-no"; w_badge_txt = "Disconnected";
    }

    char status[1024];
    snprintf(status, sizeof(status),
        "<div class='card'>"
        "<h3>Connection Status</h3>"

        "<div class='slbl'>WiFi</div>"
        "<div class='sbox %s'>"
        "<span class='badge %s' id='wbadge'>%s</span>"
        "<span id='wtext'>%s</span>"
        "<div id='wip' style='font-size:.8rem;color:#aaa;margin-top:.3rem;display:%s'>%s%s</div>"
        "</div>"

        "<div class='slbl' style='margin-top:.5rem'>Internet</div>"
        "<div class='sbox %s'>"
        "<span class='badge %s' id='ibadge'>%s</span>"
        "<span id='itext' style='font-size:.82rem;display:%s'>%s</span>"
        "</div>"
        "</div>",

        /* WiFi box */
        w_cls, w_badge_cls, w_badge_txt,
        (s_sta_state != STA_IDLE && strlen(s_sta_ssid)) ? s_sta_ssid : "Not connected",
        /* IP line */
        (s_sta_state == STA_CONNECTED && strlen(s_sta_ip)) ? "block" : "none",
        (s_sta_state == STA_CONNECTED && strlen(s_sta_ip)) ? "IP: " : "",
        s_sta_ip,

        /* Internet box */
        s_internet_ok ? "s-ok" : "s-no",
        s_internet_ok ? "b-ok" : "b-no",
        s_internet_ok ? "Available" : "Not available",
        s_internet_ok ? "none" : "block",
        s_internet_ok ? "" : "No route to internet"
    );
    httpd_resp_sendstr_chunk(req, status);

    /* --- Scan & Connect card --- */
    httpd_resp_sendstr_chunk(req,
        "<div class='card'>"
        "<h3>Connect to Office WiFi</h3>"
        "<button class='btn btn-scan' onclick='doScan()'>&#x1F50D; Scan for Networks</button>"
        "<div id='spin'>Scanning, please wait\xe2\x80\xa6</div>"
        "<div id='nlist' class='nets'></div>"
        "<label>Network (SSID)</label>"
        "<input type='text' id='ssid' placeholder='Select from list or type manually'>"
        "<label>Password</label>"
        "<input type='password' id='pass' placeholder='WiFi password'>"
        "<div class='row'>"
        "<button class='btn btn-primary' onclick='doConn()' style='flex:1'>&#x1F517; Connect</button>"
        "<button class='btn btn-sec' onclick='location.reload()'>&#x21BB; Refresh</button>"
        "</div>"
        "<div id='cmsg'></div>"
        "</div>"
    );

    httpd_resp_sendstr_chunk(req, CFG_JS);
    httpd_resp_sendstr_chunk(req, "</body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* POST /api/scan  (blocking scan; returns JSON array) */
static esp_err_t h_api_scan(httpd_req_t *req)
{
    if (!session_check(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    }

    wifi_scan_config_t sc = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false,
    };
    /* Blocking scan (max ~3 s) */
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }

    uint16_t count = MAX_SCAN_APS;
    if (s_scan_aps) { free(s_scan_aps); s_scan_aps = NULL; }
    s_scan_aps = malloc(count * sizeof(wifi_ap_record_t));
    if (!s_scan_aps) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }
    esp_wifi_scan_get_ap_records(&count, s_scan_aps);
    s_scan_count = count;

    /* Build JSON */
    char *json = malloc(4096);
    if (!json) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }
    int pos = 0;
    pos += snprintf(json + pos, 4096 - pos, "[");
    for (int i = 0; i < s_scan_count && pos < 3900; i++) {
        if (i) pos += snprintf(json + pos, 4096 - pos, ",");
        /* Escape SSID */
        char esc[128] = {0};
        const char *src = (const char *)s_scan_aps[i].ssid;
        int ei = 0;
        for (int si = 0; src[si] && ei < 120; si++) {
            if (src[si] == '"' || src[si] == '\\') esc[ei++] = '\\';
            esc[ei++] = src[si];
        }
        pos += snprintf(json + pos, 4096 - pos,
                        "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                        esc, s_scan_aps[i].rssi, s_scan_aps[i].authmode);
    }
    pos += snprintf(json + pos, 4096 - pos, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, pos);
    free(json);
    return ESP_OK;
}

/* POST /api/connect */
static esp_err_t h_api_connect(httpd_req_t *req)
{
    if (!session_check(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    }

    char body[512];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = 0;

    char ssid[33], pass[65];
    form_value(body, "ssid",     ssid, sizeof(ssid));
    form_value(body, "password", pass, sizeof(pass));

    if (strlen(ssid) == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
            "{\"success\":false,\"message\":\"SSID cannot be empty\"}");
    }

    nvs_save_wifi(ssid, pass);
    esp_err_t err = sta_connect(ssid, pass);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req,
            "{\"success\":true,"
            "\"message\":\"Connecting\xe2\x80\xa6 page will update in 4 s\"}");
    } else {
        return httpd_resp_sendstr(req,
            "{\"success\":false,\"message\":\"Failed to start connection\"}");
    }
}

/* GET /api/status */
static esp_err_t h_api_status(httpd_req_t *req)
{
    if (!session_check(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    }

    const char *wifi_str;
    switch (s_sta_state) {
    case STA_CONNECTED:  wifi_str = "connected";    break;
    case STA_CONNECTING: wifi_str = "connecting";   break;
    case STA_FAILED:     wifi_str = "failed";        break;
    default:             wifi_str = "disconnected";  break;
    }

    char json[256];
    snprintf(json, sizeof(json),
             "{\"wifi\":\"%s\",\"ssid\":\"%s\","
             "\"ip\":\"%s\",\"internet\":%s}",
             wifi_str, s_sta_ssid, s_sta_ip,
             s_internet_ok ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* --------------------------------------------------------------------------
 * Start HTTP server
 * -------------------------------------------------------------------------- */
static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 10;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    static const httpd_uri_t uris[] = {
        { "/",            HTTP_GET,  h_root,        NULL },
        { "/login",       HTTP_GET,  h_login_get,   NULL },
        { "/login",       HTTP_POST, h_login_post,  NULL },
        { "/config",      HTTP_GET,  h_config,      NULL },
        { "/logout",      HTTP_GET,  h_logout,      NULL },
        { "/api/scan",    HTTP_POST, h_api_scan,    NULL },
        { "/api/connect", HTTP_POST, h_api_connect, NULL },
        { "/api/status",  HTTP_GET,  h_api_status,  NULL },
    };
    for (int i = 0; i < (int)(sizeof(uris)/sizeof(uris[0])); i++)
        httpd_register_uri_handler(srv, &uris[i]);

    ESP_LOGI(TAG, "Web server ready  ->  http://192.168.4.1/");
    return srv;
}

/* --------------------------------------------------------------------------
 * app_main
 * -------------------------------------------------------------------------- */
void app_main(void)
{
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    /* WiFi init */
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Configure AP */
    s_netif_ap = esp_netif_create_default_wifi_ap();
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = strlen(AP_SSID),
            .channel        = AP_CHANNEL,
            .password       = AP_PASS,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(AP_PASS) == 0) ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* Configure STA (empty; will be set from NVS or web UI) */
    esp_netif_create_default_wifi_sta();
    wifi_config_t sta_cfg = {0};
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    /* Start WiFi */
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP  SSID: \"%s\"  Password: \"%s\"", AP_SSID, AP_PASS);
    ESP_LOGI(TAG, "Web login: %s / %s", WEB_USERNAME, WEB_PASSWORD);

    /* Start web server */
    start_webserver();

    /* Internet check task */
    xTaskCreate(internet_check_task, "inet_chk", 4096, NULL, 5, NULL);

	esp_netif_t *esp_netif = NULL;
	esp_netif_napt_enable(esp_netif);

    /* Try saved credentials */
    char s_ssid[33] = {0}, s_pass[65] = {0};
    if (nvs_load_wifi(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass)) == ESP_OK
        && strlen(s_ssid) > 0)
    {
        ESP_LOGI(TAG, "Saved WiFi found: \"%s\" — connecting", s_ssid);
        sta_connect(s_ssid, s_pass);
    } else {
        ESP_LOGI(TAG, "No saved WiFi. Open http://192.168.4.1/ to configure.");
        s_sta_state = STA_IDLE;
    }
}
