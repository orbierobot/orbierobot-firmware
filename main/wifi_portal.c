#include "wifi_portal.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "wifi_portal";

#define NVS_NS        "orbie"
#define NVS_KEY_SSID  "sta_ssid"
#define NVS_KEY_PASS  "sta_pass"
#define PORTAL_IP     "192.168.4.1"
#define MAX_RETRIES   5

static orbie_wifi_state_t s_state = ORBIE_WIFI_IDLE;
static char s_ssid[33] = "";
static char s_ip[16]  = "";
static int  s_retries = 0;
static int  s_reason  = 0;   /* last wifi_err_reason_t from a disconnect */

/* A failed join is almost always one of two things, and the user cannot tell
 * them apart from the outside - especially on a hidden network, where a
 * mistyped name and a wrong password both just time out. The reason code from
 * the disconnect event does distinguish them, so say which it was. */
static const char *reason_text(int reason)
{
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return "Network not found. Check the name - hidden names are case-sensitive - and that the robot is in range.";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_MIC_FAILURE:
            return "Wrong password.";
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_CONNECTION_FAIL:
            return "The network refused the connection. It may be full, or filtering by MAC address.";
        case 0:
            return "";
        default:
            return "Could not connect.";
    }
}

/* ===================== credential storage ===================== */

static esp_err_t creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool creds_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK && ssid[0];
    if (ok && nvs_get_str(h, NVS_KEY_PASS, pass, &pass_len) != ESP_OK) pass[0] = '\0';
    nvs_close(h);
    return ok;
}

/* ===================== connection ===================== */

static void sta_connect(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass ? pass : "", sizeof(cfg.sta.password));
    /* An open network must not advertise a minimum auth mode. */
    cfg.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    /* A hidden AP puts no SSID in its beacons, so it is only found by a
     * directed probe on the right channel. Fast scan stops at the first
     * beacon match and can miss it entirely; sweep every channel instead. */
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_ip[0]   = '\0';
    s_retries = 0;
    s_state   = ORBIE_WIFI_CONNECTING;

    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_disconnect();
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (d) s_reason = d->reason;

        /* Retrying a name that does not exist just burns 5 attempts and makes
         * the user wait ~15s for a verdict we already have. */
        bool hopeless = (s_reason == WIFI_REASON_NO_AP_FOUND);

        if (s_state == ORBIE_WIFI_CONNECTING && s_retries < MAX_RETRIES && !hopeless) {
            s_retries++;
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d/%d",
                     s_reason, s_retries, MAX_RETRIES);
            esp_wifi_connect();
        } else if (s_state != ORBIE_WIFI_IDLE) {
            s_state = ORBIE_WIFI_FAILED;
            s_ip[0] = '\0';
            ESP_LOGW(TAG, "giving up on \"%s\": reason %d - %s",
                     s_ssid, s_reason, reason_text(s_reason));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_state   = ORBIE_WIFI_CONNECTED;
        s_retries = 0;
        ESP_LOGI(TAG, "connected to \"%s\", IP %s", s_ssid, s_ip);
    }
}

void wifi_portal_init(void)
{
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        on_wifi_event, NULL, NULL);

    char ssid[33] = "", pass[65] = "";
    if (creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "stored credentials found for \"%s\"", ssid);
        sta_connect(ssid, pass);
    } else {
        ESP_LOGI(TAG, "no stored credentials - portal only");
    }
}

orbie_wifi_state_t wifi_portal_state(void) { return s_state; }
const char *wifi_portal_ssid(void)         { return s_ssid; }
const char *wifi_portal_ip(void)           { return s_ip; }

/* ===================== DNS hijack ===================== */
/* Answer every A query with our own address so the phone's captive-portal
 * probe resolves to us instead of the real internet. */

static void dns_task(void *arg)
{
    uint8_t buf[512];
    struct sockaddr_in server = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0 || bind(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        ESP_LOGE(TAG, "DNS socket setup failed (errno %d)", errno);
        if (sock >= 0) close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS hijack listening on :53 -> " PORTAL_IP);

    while (true) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
        /* 12-byte header + at least a 1-byte name, 2 type, 2 class */
        if (n < 17) continue;

        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if ((buf[2] & 0x80) || qdcount == 0) continue;   /* ignore responses */

        /* Walk the QNAME labels to find where the question ends. */
        int p = 12;
        while (p < n && buf[p]) {
            p += buf[p] + 1;
            if (p >= n) break;
        }
        if (p + 5 > n) continue;          /* need root label + QTYPE + QCLASS */
        uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
        p += 1 + 4;                       /* root label + QTYPE + QCLASS */
        if (p > n || p + 16 > (int)sizeof(buf)) continue;

        buf[2] = 0x81;  buf[3] = 0x80;    /* QR=1, RD=1, RA=1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;  /* NS/AR = 0 */

        /* Only an A query gets an address. Answering AAAA (or anything else)
         * with 4 bytes of IPv4 is malformed; Android's connectivity checks are
         * strict about it and will call the network broken rather than
         * captive. NOERROR with no answers makes the client fall back to IPv4,
         * which is what we want. */
        if (qtype != 1) {
            buf[6] = 0x00; buf[7] = 0x00;             /* ANCOUNT = 0 */
            sendto(sock, buf, p, 0, (struct sockaddr *)&client, clen);
            continue;
        }
        buf[6] = 0x00;  buf[7] = 0x01;    /* ANCOUNT = 1      */

        uint8_t *a = buf + p;
        *a++ = 0xC0; *a++ = 0x0C;                     /* name -> offset 12 */
        *a++ = 0x00; *a++ = 0x01;                     /* type A            */
        *a++ = 0x00; *a++ = 0x01;                     /* class IN          */
        *a++ = 0x00; *a++ = 0x00; *a++ = 0x00; *a++ = 0x3C;  /* TTL 60s    */
        *a++ = 0x00; *a++ = 0x04;                     /* rdlength 4        */
        uint32_t ip = inet_addr(PORTAL_IP);
        memcpy(a, &ip, 4);
        a += 4;

        sendto(sock, buf, a - buf, 0, (struct sockaddr *)&client, clen);
    }
}

void wifi_portal_start_dns(void)
{
    xTaskCreate(dns_task, "dns_hijack", 4096, NULL, 4, NULL);
}

/* ===================== portal UI ===================== */

static const char PORTAL_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Orbie Wi-Fi Setup</title><style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#111;color:#eee;font:16px -apple-system,system-ui,Arial;padding:24px 18px;max-width:460px;margin:0 auto}"
"h1{font-size:22px;margin-bottom:4px}"
"p.sub{color:#888;font-size:14px;margin-bottom:22px}"
"label{display:block;font-size:13px;color:#aaa;margin:16px 0 6px}"
"select,input{width:100%;padding:13px;border-radius:10px;border:1px solid #333;background:#1c1c1c;color:#eee;font-size:16px}"
"button{width:100%;padding:15px;margin-top:22px;border:0;border-radius:10px;background:#e94560;color:#fff;font-size:16px;font-weight:600}"
"button:disabled{opacity:.5}"
"#msg{margin-top:18px;padding:13px;border-radius:10px;font-size:14px;display:none}"
"#msg.ok{display:block;background:#12301c;color:#7ee2a8;border:1px solid #1f5c36}"
"#msg.err{display:block;background:#331519;color:#ff8f9c;border:1px solid #6b2029}"
"#msg.busy{display:block;background:#1a2433;color:#8fb8e6;border:1px solid #294056}"
".row{display:flex;gap:8px}.row select{flex:1}"
".pw{position:relative}.pw input{padding-right:74px}"
"#eye{position:absolute;right:6px;top:6px;width:auto;margin:0;padding:8px 12px;background:#2a2a2a;color:#bbb;font-size:13px;font-weight:500;border-radius:7px}"
".rescan{width:auto;padding:13px 16px;margin:0;background:#2a2a2a;font-size:14px}"
"</style></head><body>"
"<h1>Connect Orbie to Wi-Fi</h1>"
"<p class='sub'>Pick your home network so Orbie can reach the internet.</p>"
"<label>Network</label>"
"<div class='row'><select id='ssid' onchange='pick()'><option>Scanning...</option></select>"
"<button class='rescan' onclick='scan()'>Rescan</button></div>"
"<div id='hidden-wrap' style='display:none'>"
"<label>Network name</label>"
"<input id='hssid' placeholder='Exact name, case-sensitive' autocapitalize='off' autocorrect='off'>"
"</div>"
"<label>Password</label>"
"<div class='pw'><input id='pass' type='password' placeholder='Leave blank if open' autocapitalize='off' autocorrect='off' autocomplete='off' spellcheck='false'>"
"<button type='button' id='eye' onclick='togglePw()' aria-label='Show password'>Show</button></div>"
"<button id='go' onclick='save()'>Connect</button>"
"<div id='msg'></div>"
"<script>"
"function show(c,t){var m=document.getElementById('msg');m.className=c;m.textContent=t;}"
"function togglePw(){var i=document.getElementById('pass'),e=document.getElementById('eye');"
"var on=i.type==='password';i.type=on?'text':'password';e.textContent=on?'Hide':'Show';"
"e.setAttribute('aria-label',on?'Hide password':'Show password');i.focus();}"
"function addHidden(s){var o=document.createElement('option');o.value='__hidden__';"
"o.textContent='Other (hidden network)...';s.appendChild(o);}"
"function pick(){var h=document.getElementById('ssid').value==='__hidden__';"
"document.getElementById('hidden-wrap').style.display=h?'block':'none';"
"if(h)document.getElementById('hssid').focus();}"
"function scan(){var s=document.getElementById('ssid');s.innerHTML='<option>Scanning...</option>';"
"fetch('/api/scan').then(r=>r.json()).then(function(l){s.innerHTML='';"
"l.forEach(function(n){var o=document.createElement('option');o.value=n.ssid;"
"o.textContent=n.ssid+'  ('+n.rssi+' dBm'+(n.open?', open':'')+')';s.appendChild(o);});"
"addHidden(s);pick();})"
".catch(function(){s.innerHTML='';addHidden(s);pick();show('err','Scan failed - you can still enter a network by name.');});}"
"function save(){var sel=document.getElementById('ssid').value;"
"var ssid=sel==='__hidden__'?document.getElementById('hssid').value.trim():sel;"
"if(!ssid){show('err','Enter the network name.');return;}"
"var b=document.getElementById('go');b.disabled=true;"
"show('busy','Connecting...');"
"fetch('/api/connect',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:ssid,pass:document.getElementById('pass').value})})"
".then(function(){poll(0);}).catch(function(){b.disabled=false;show('err','Request failed');});}"
"function poll(n){fetch('/api/wifi-status').then(r=>r.json()).then(function(d){"
"if(d.state=='connected'){show('ok','Connected to '+d.ssid+'  -  IP '+d.ip);"
"document.getElementById('go').disabled=false;return;}"
"if(d.state=='failed'){show('err',d.detail||'Could not connect.');"
"document.getElementById('go').disabled=false;return;}"
"if(n>40){show('err','Timed out.');document.getElementById('go').disabled=false;return;}"
"setTimeout(function(){poll(n+1);},1000);});}"
"scan();"
"</script></body></html>";

static esp_err_t portal_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Any OS connectivity probe gets bounced to the portal, which is what makes
 * the sign-in sheet appear automatically on iOS and Android. */
static esp_err_t probe_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP "/portal");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t not_found_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return probe_redirect(req);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    cJSON *arr = cJSON_CreateArray();

    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 20) n = 20;
        wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                if (!recs[i].ssid[0]) continue;       /* skip hidden */
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "ssid", (char *)recs[i].ssid);
                cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
                cJSON_AddBoolToObject(o, "open", recs[i].authmode == WIFI_AUTH_OPEN);
                cJSON_AddItemToArray(arr, o);
            }
        }
        free(recs);
    } else {
        ESP_LOGW(TAG, "scan failed");
    }

    char *out = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "[]");
    cJSON_free(out);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t connect_post(httpd_req_t *req)
{
    char body[256];
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    const cJSON *js = cJSON_GetObjectItem(root, "ssid");
    const cJSON *jp = cJSON_GetObjectItem(root, "pass");
    if (!cJSON_IsString(js) || !js->valuestring[0]) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    const char *pass = cJSON_IsString(jp) ? jp->valuestring : "";

    /* Save first: a reboot mid-association should still remember the choice. */
    creds_save(js->valuestring, pass);
    sta_connect(js->valuestring, pass);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t wifi_status_get(httpd_req_t *req)
{
    const char *st = "idle";
    switch (s_state) {
        case ORBIE_WIFI_CONNECTING: st = "connecting"; break;
        case ORBIE_WIFI_CONNECTED:  st = "connected";  break;
        case ORBIE_WIFI_FAILED:     st = "failed";     break;
        default: break;
    }
    char out[320];
    snprintf(out, sizeof(out),
             "{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"reason\":%d,\"detail\":\"%s\"}",
             st, s_ssid, s_ip, s_reason, reason_text(s_reason));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

esp_err_t wifi_portal_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/portal",              .method = HTTP_GET,  .handler = portal_get },
        { .uri = "/api/scan",            .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/connect",         .method = HTTP_POST, .handler = connect_post },
        { .uri = "/api/wifi-status",     .method = HTTP_GET,  .handler = wifi_status_get },
        /* OS connectivity probes. The 404 handler below already redirects any
         * unmatched path, so these are belt-and-braces for the common ones. */
        { .uri = "/hotspot-detect.html", .method = HTTP_GET,  .handler = probe_redirect }, /* iOS/macOS      */
        { .uri = "/generate_204",        .method = HTTP_GET,  .handler = probe_redirect }, /* Android        */
        { .uri = "/gen_204",             .method = HTTP_GET,  .handler = probe_redirect }, /* Android        */
        { .uri = "/ncsi.txt",            .method = HTTP_GET,  .handler = probe_redirect }, /* Windows legacy */
        { .uri = "/connecttest.txt",     .method = HTTP_GET,  .handler = probe_redirect }, /* Windows 10/11  */
        { .uri = "/success.txt",         .method = HTTP_GET,  .handler = probe_redirect }, /* Firefox        */
        { .uri = "/canonical.html",      .method = HTTP_GET,  .handler = probe_redirect }, /* Ubuntu/Android */
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    /* Anything else on the AP also lands on the portal. */
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_redirect);
    ESP_LOGI(TAG, "portal registered at http://" PORTAL_IP "/portal");
    return ESP_OK;
}
