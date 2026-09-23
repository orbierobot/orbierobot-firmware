#include "wifi_portal.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
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
#define PORTAL_IP     "192.168.4.1"

/* Up to five remembered networks, most-recently-used first. Five because a
 * robot realistically moves between a desk, a home, an office and a venue -
 * and because each attempt costs real seconds at boot, so a longer list would
 * just delay the portal for someone who has moved somewhere new entirely. */
#define MAX_NETWORKS  5
#define KEY_COUNT     "net_n"

/* Per-network attempts. Deliberately small: with five stored networks, the old
 * five-retry behaviour would take over a minute to conclude that none of them
 * are here, and the user is stood there watching a robot do nothing. */
#define TRY_RETRIES   2
#define TRY_TIMEOUT_MS 20000

static orbie_wifi_state_t s_state = ORBIE_WIFI_IDLE;
static char s_ssid[33] = "";
static char s_ip[16]  = "";
static int  s_retries = 0;
static int  s_reason  = 0;   /* last wifi_err_reason_t from a disconnect */
static int  s_max_retries = 5;
static bool s_trying_stored = false;   /* boot-time walk through saved networks */

typedef struct {
    char ssid[33];
    char pass[65];
} orbie_net_t;

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

static void key_for(char *out, size_t n, const char *base, int i)
{
    snprintf(out, n, "%s%d", base, i);
}

/* Credentials used to live under a single sta_ssid/sta_pass pair. Carrying
 * them over matters: without this, upgrading the firmware silently forgets the
 * network a robot was already on, and the owner is sent back to the portal for
 * no visible reason. */
static void nets_migrate_legacy(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    int32_t count = 0;
    if (nvs_get_i32(h, KEY_COUNT, &count) == ESP_OK && count > 0) {
        nvs_close(h);
        return;                      /* already on the new layout */
    }

    char ssid[33] = "", pass[65] = "";
    size_t sl = sizeof(ssid), pl = sizeof(pass);
    if (nvs_get_str(h, "sta_ssid", ssid, &sl) == ESP_OK && ssid[0]) {
        if (nvs_get_str(h, "sta_pass", pass, &pl) != ESP_OK) pass[0] = '\0';
        nvs_set_str(h, "ssid0", ssid);
        nvs_set_str(h, "pass0", pass);
        nvs_set_i32(h, KEY_COUNT, 1);
        nvs_commit(h);
        ESP_LOGI(TAG, "migrated stored network \"%s\" to the new format", ssid);
    }
    nvs_close(h);
}

static int nets_load(orbie_net_t *out, int max)
{
    nets_migrate_legacy();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;

    int32_t count = 0;
    nvs_get_i32(h, KEY_COUNT, &count);
    if (count < 0) count = 0;
    if (count > max) count = max;

    int n = 0;
    for (int i = 0; i < count; i++) {
        char k[16];
        size_t len = sizeof(out[n].ssid);
        key_for(k, sizeof(k), "ssid", i);
        if (nvs_get_str(h, k, out[n].ssid, &len) != ESP_OK || !out[n].ssid[0]) continue;
        len = sizeof(out[n].pass);
        key_for(k, sizeof(k), "pass", i);
        if (nvs_get_str(h, k, out[n].pass, &len) != ESP_OK) out[n].pass[0] = '\0';
        n++;
    }
    nvs_close(h);
    return n;
}

/* Store this network at the front, dropping any older copy of the same SSID.
 * Most-recently-used ordering means the network you are actually standing in
 * is tried first next time, which is almost always the right guess. */
static esp_err_t nets_remember(const char *ssid, const char *pass)
{
    orbie_net_t list[MAX_NETWORKS];
    int n = nets_load(list, MAX_NETWORKS);

    orbie_net_t merged[MAX_NETWORKS];
    int m = 0;
    strlcpy(merged[m].ssid, ssid, sizeof(merged[m].ssid));
    strlcpy(merged[m].pass, pass ? pass : "", sizeof(merged[m].pass));
    m++;
    for (int i = 0; i < n && m < MAX_NETWORKS; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) continue;   /* de-dupe */
        merged[m++] = list[i];
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    for (int i = 0; i < m; i++) {
        char k[16];
        key_for(k, sizeof(k), "ssid", i); nvs_set_str(h, k, merged[i].ssid);
        key_for(k, sizeof(k), "pass", i); nvs_set_str(h, k, merged[i].pass);
    }
    nvs_set_i32(h, KEY_COUNT, m);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "remembered \"%s\" (%d network%s stored)", ssid, m, m == 1 ? "" : "s");
    return err;
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
    s_reason  = 0;
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

        /* A name that does not resolve cannot succeed, so a typo should fail
         * fast rather than making the user wait. But a HIDDEN network reports
         * exactly the same NO_AP_FOUND on its first attempt - it puts no SSID
         * in its beacons, so it is only found once a directed probe lands.
         * Failing fast on a stored network therefore skipped hidden ones
         * entirely, which is how "Ayodhya" was abandoned in 2.8 seconds. */
        bool hopeless = (s_reason == WIFI_REASON_NO_AP_FOUND) && !s_trying_stored;

        if (s_state == ORBIE_WIFI_CONNECTING && s_retries < s_max_retries && !hopeless) {
            s_retries++;
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d/%d",
                     s_reason, s_retries, s_max_retries);
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

/* Walk the remembered networks in order and stop at the first that answers.
 * Runs in its own task: each attempt can take ten seconds or more, and
 * app_main must not sit blocked while the face and servers come up. */
static void sta_try_stored_task(void *pv)
{
    orbie_net_t list[MAX_NETWORKS];
    int n = nets_load(list, MAX_NETWORKS);
    if (n == 0) {
        ESP_LOGI(TAG, "no stored networks - portal only");
        s_state = ORBIE_WIFI_IDLE;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "trying %d stored network%s", n, n == 1 ? "" : "s");
    s_max_retries   = TRY_RETRIES;
    s_trying_stored = true;

    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  [%d/%d] \"%s\"", i + 1, n, list[i].ssid);
        sta_connect(list[i].ssid, list[i].pass);

        int waited = 0;
        while (waited < TRY_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(250));
            waited += 250;
            if (s_state == ORBIE_WIFI_CONNECTED || s_state == ORBIE_WIFI_FAILED) break;
        }
        if (s_state == ORBIE_WIFI_CONNECTED) {
            ESP_LOGI(TAG, "joined \"%s\" - skipping the rest", s_ssid);
            s_max_retries   = 5;        /* be patient again once we are on */
            s_trying_stored = false;
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "  \"%s\" did not answer", list[i].ssid);
    }

    /* Nothing reachable. Leave the state IDLE rather than FAILED so the portal
     * shows the setup form instead of an error about the last one tried -
     * being somewhere new is not a failure. */
    esp_wifi_disconnect();
    s_state = ORBIE_WIFI_IDLE;
    s_ssid[0] = '\0';
    s_max_retries   = 5;
    s_trying_stored = false;
    ESP_LOGW(TAG, "none of the %d stored networks are in range - portal only", n);
    vTaskDelete(NULL);
}

void wifi_portal_init(void)
{
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        on_wifi_event, NULL, NULL);
    xTaskCreate(sta_try_stored_task, "wifi_try", 4096, NULL, 5, NULL);
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
"<title>Orbie</title><style>"
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
"#openbrowser{display:block;text-align:center;padding:12px;margin-bottom:6px;border:1px solid #333;border-radius:10px;color:#9ab;text-decoration:none;font-size:14px}"
".addr{color:#666;font-size:12px;text-align:center;margin-bottom:18px}"
"#eye{position:absolute;right:6px;top:6px;width:auto;margin:0;padding:8px 12px;background:#2a2a2a;color:#bbb;font-size:13px;font-weight:500;border-radius:7px}"
".rescan{width:auto;padding:13px 16px;margin:0;background:#2a2a2a;font-size:14px}"
"</style></head><body>"
"<h1>Connect Orbie to Wi-Fi</h1>"
"<p class='sub'>Pick your home network so Orbie can reach the internet.</p>"
/* Relative, NOT the fixed AP address. This page is served both from the
   robot's own AP and, via /portal?setup=1, from its address on the owner's
   LAN. Hardcoding 192.168.4.1 made both links dead ends for anyone who
   reached the page over their home network. "/" is correct either way. */
"<a id='openbrowser' href='/' target='_blank' rel='noopener'>Open Orbie in your browser</a>"
"<p class='addr'>No address bar here? Open <b id='host'>" PORTAL_IP "</b> in Safari or Chrome.</p>"
"<p class='addr'><a href='/' target='_blank' rel='noopener' "
"style='color:#9ab'>Open the control panel &rarr;</a></p>"
"<p class='addr' id='fw'></p>"
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
"<button id='done' style='display:none;background:#1f6f43' onclick=\"location.href='/'\">Open Orbie controls</button>"
"<p id='lanhint' class='sub' style='display:none;margin-top:14px'></p>"
"<script>"
"function show(c,t){var m=document.getElementById('msg');m.className=c;m.textContent=t;}"
"fetch('/api/whoami').then(r=>r.json()).then(function(d){"
"document.getElementById('fw').textContent=d.robot+' \u00b7 firmware '+d.version;}).catch(function(){});"
/* Show the address actually in use. On the AP that is 192.168.4.1 anyway;
   over the LAN the baked-in default would send the reader somewhere dead. */
"(function(){var h=document.getElementById('host');if(h)h.textContent=location.host;})();"
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
"if(d.state=='connected'){show('ok','Connected to '+d.ssid+'. Opening controls...');"
"document.getElementById('go').disabled=false;"
"document.getElementById('done').style.display='block';"
"var h=document.getElementById('lanhint');"
"h.textContent='On '+d.ssid+', Orbie is also at http://'+d.ip+' - reachable from any device on that network.';"
"h.style.display='block';"
/* The captive sheet is a cut-down browser and handles MJPEG poorly, so hand
   over to the real control page rather than trying to stream inside it. */
"setTimeout(function(){location.href='/';},1800);return;}"
"if(d.state=='failed'){show('err',d.detail||'Could not connect.');"
"document.getElementById('go').disabled=false;return;}"
"if(n>40){show('err','Timed out.');document.getElementById('go').disabled=false;return;}"
"setTimeout(function(){poll(n+1);},1000);});}"
"scan();"
"</script></body></html>";

/* Shown when the robot is already online. Deliberately tiny: the captive
 * sheet is a cut-down browser that handles an MJPEG stream and a dozen
 * background fetches badly, so redirecting it straight into the control panel
 * looked like the firmware had hung. Hand the user to a real browser instead. */
static esp_err_t connected_handoff(httpd_req_t *req)
{
    /* Grew when the copy-link and second hyperlink were added; -Werror=
     * format-truncation catches this the moment it is too small. */
    char page[2600];
    snprintf(page, sizeof(page),
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Orbie</title><style>"
        "body{background:#111;color:#eee;font:16px -apple-system,system-ui,Arial;"
        "padding:34px 20px;max-width:440px;margin:0 auto;text-align:center}"
        "h1{font-size:22px;margin-bottom:6px}p{color:#8a8a8a;font-size:14px;line-height:1.55}"
        "a{display:block;padding:15px;margin:20px 0 10px;border-radius:11px;background:#e94560;"
        "color:#fff;text-decoration:none;font-weight:600}"
        "a.sec{background:#242424;color:#9ab;font-weight:400;font-size:14px}"
        "code{color:#7ee2a8;font-size:15px;user-select:all;-webkit-user-select:all}"
        ".url{display:flex;align-items:center;gap:8px;justify-content:center;"
        "background:#1a1a1a;border:1px solid #2a2a2a;border-radius:10px;padding:11px;margin:10px 0}"
        ".url button{background:#2f2f2f;color:#ddd;border:0;border-radius:7px;"
        "padding:7px 13px;font-size:13px;font-family:inherit}"
        "#cpd{color:#7ee2a8;font-size:12px;min-height:16px}</style></head><body>"
        "<h1>Orbie is online</h1>"
        "<p>Connected to <b>%s</b><br>Firmware <b>%s</b></p>"
        /* Relative links: whatever host reached this page can reach these,
         * whether that is the AP address or the robot's address on the
         * owner's LAN. A fixed 192.168.4.1 is wrong for the latter. */
        "<a href='/'>Open the controls</a>"
        "<a class='sec' href='/' target='_blank' rel='noopener'>"
        "Open in Safari / Chrome</a>"
        "<p>This sign-in window has no address bar and is a limited browser, so "
        "the camera may not load here.</p>"
        "<div class='url'><code id='u'></code>"
        "<button onclick='cp()'>Copy</button></div>"
        "<p id='cpd'></p>"
        "<p>On <b>%s</b> the same page is at <code>http://%s</code>.</p>"
        "<script>document.getElementById('u').textContent=location.origin+'/';"
        "function cp(){var t=document.getElementById('u').textContent;"
        "function done(){document.getElementById('cpd').textContent='Copied - paste it into Safari.';}"
        "if(navigator.clipboard&&navigator.clipboard.writeText){"
        "navigator.clipboard.writeText(t).then(done).catch(sel);}else{sel();}"
        "function sel(){var r=document.createRange();r.selectNode(document.getElementById('u'));"
        "var s=window.getSelection();s.removeAllRanges();s.addRange(r);"
        "try{document.execCommand('copy');done();}catch(e){"
        "document.getElementById('cpd').textContent='Select the address above and copy it.';}}}"
        "</script>"
        "<a class='sec' href='/portal?setup=1'>Change Wi-Fi network</a>"
        "</body></html>",
        /* The link MUST be the AP address: whoever is reading this joined
         * ORBIE_xxxx to get here, and the robot's address on the owner's LAN
         * is on a different subnet they cannot route to. Pointing at it made
         * the button do nothing at all. The LAN address is still worth showing
         * - it is how they reach the robot later, from their own network. */
        s_ssid, esp_app_get_description()->version, s_ssid,
        s_ip[0] ? s_ip : "(not connected)");

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_get(httpd_req_t *req)
{
    /* Reached by a probe redirect or by hand. Either way, an already-connected
     * robot should show its controls rather than ask for Wi-Fi again. A
     * ?setup=1 query forces the form, for changing networks on purpose. */
    char query[32], force[8] = {0};
    bool wants_setup = (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                        httpd_query_key_value(query, "setup", force, sizeof(force)) == ESP_OK);

    if (s_state == ORBIE_WIFI_CONNECTED && !wants_setup) {
        return connected_handoff(req);
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Any OS connectivity probe gets bounced to the portal, which is what makes
 * the sign-in sheet appear automatically on iOS and Android. */
static esp_err_t probe_redirect(httpd_req_t *req)
{
    /* The phone's probe fails whenever it is on the robot's AP, because the
     * robot is an access point and not a gateway - it has no route to give.
     * That is true even when the robot itself is happily on Wi-Fi, so without
     * this check the setup form reappears every single time. If we are already
     * online there is nothing to set up: go straight to the controls. */
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
    nets_remember(js->valuestring, pass);
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
    /* Keep going if one probe route cannot be registered. Returning early
     * used to skip the 404 handler below, which is the single most important
     * registration here - it is what makes an unlisted probe URL land on the
     * portal, and losing it silently stops the captive page appearing. */
    int failed = 0;
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(err));
            failed++;
        }
    }

    /* Anything else on the AP also lands on the portal. */
    esp_err_t err404 = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND,
                                                  not_found_redirect);
    if (err404 != ESP_OK) {
        ESP_LOGE(TAG, "404 catch-all NOT registered (%s) - the captive portal "
                      "will not pop up on its own", esp_err_to_name(err404));
    }
    if (failed) {
        ESP_LOGW(TAG, "%d probe route(s) unregistered; the 404 catch-all covers them", failed);
    }
    ESP_LOGI(TAG, "portal registered at http://" PORTAL_IP "/portal");
    return ESP_OK;
}
