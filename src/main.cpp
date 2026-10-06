/*
  Killbot Controller
  ESP32 firmware: a 46-LED WS2812B strip forms the robot's eye (solid red).
  Runs as its own WiFi AP and serves an admin web UI for control.
*/

#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include "secrets.h"

// ---------------------------------------------------------------------------
// Identity / WiFi
// ---------------------------------------------------------------------------
const char* ap_ssid     = AP_SSID;
const char* ap_password = AP_PASSWORD;
const char* mdns_host   = "killbot";   // reachable at http://killbot.local

// 2.4GHz channel for the AP. Use ONLY 1, 6 or 11 -- these are the three
// non-overlapping channels. An "uncommon" channel like 3 or 9 partially
// overlaps two of them, and partial overlap is worse than sharing: co-channel
// neighbors take turns via CSMA, while an overlapping one is unparseable noise
// that nobody defers to. 12-14 are restricted in the US and some clients will
// not associate at all. 11 is the default here because consumer gear ships on
// 1 and 6, so 11 is usually the quietest of the three.
//
// If this rig ever gains an AP+STA device (as in the springtrap/cupcake pair),
// every radio in the group must share this channel.
#define WIFI_CHANNEL 11

// Set to 1 to scan the band at boot and log how many APs sit on each channel,
// then pick the quietest of 1/6/11 for WIFI_CHANNEL above. Costs ~2s of boot
// time and briefly enables station mode, so leave it off in normal operation.
#define CHANNEL_SCAN_ON_BOOT 0

// ---------------------------------------------------------------------------
// Eye LEDs
// ---------------------------------------------------------------------------
#define LED_PIN     13
#define NUM_LEDS    46

// Master brightness stays at full scale so the animation's white hits maximum
// output; the resting red is dimmed in the color itself instead. Tune RED_LEVEL
// (0-255) to change how bright the idle eye sits without touching the white.
#define BRIGHTNESS  255
#define RED_LEVEL   150
const CRGB EYE_COLOR   = CRGB(RED_LEVEL, 0, 0);
const CRGB FLASH_COLOR = CRGB::White;

CRGB leds[NUM_LEDS];
bool eyeOn = true;

// ---------------------------------------------------------------------------
// Converge animation: two white comets start at the far ends and chase
// toward the center over the red eye, then the whole eye double-flashes
// white and returns to red.
// ---------------------------------------------------------------------------
#define COMET_TAIL      4     // LEDs of fading tail behind each comet head
#define COMET_STEP_MS   25    // time per LED of comet travel
#define FLASH_ON_MS     80
#define FLASH_OFF_MS    80
#define FLASH_COUNT     2

enum AnimPhase { ANIM_IDLE, ANIM_COMET, ANIM_FLASH };
AnimPhase animPhase = ANIM_IDLE;
int animStep = 0;                 // comet head position, or flash half-cycle index
unsigned long animNextMs = 0;

// ---------------------------------------------------------------------------
// Action queue: async web handlers run on the AsyncTCP task, so they only
// enqueue a path here; loop() drains the queue and does the real work.
// ---------------------------------------------------------------------------
#define SSE_HEARTBEAT_MS   3000   // status re-push interval; also reaps dead SSE clients
unsigned long nextHeartbeatMs = 0;

#define ACTION_PATH_MAX    32
#define ACTION_QUEUE_DEPTH 8
struct ActionMsg { char path[ACTION_PATH_MAX]; };
QueueHandle_t actionQueue = NULL;

// Captive-portal DNS: answers every lookup with our own AP address. Tablets
// probe a connectivity-check URL over DNS when they join; if that lookup fails
// the OS flags the network "no internet" and may drop back to cellular, after
// which taps on the admin page go nowhere. Hijacking DNS lets us answer those
// probes ourselves (see the handlers in setupWebServer) so the client stays put.
DNSServer dnsServer;
const IPAddress ap_ip(192, 168, 4, 1);

AsyncWebServer server(80);
AsyncEventSource events("/events");
String pageHtml;

// ---------------------------------------------------------------------------
// Eye control
// ---------------------------------------------------------------------------
void applyEye() {
    fill_solid(leds, NUM_LEDS, eyeOn ? EYE_COLOR : CRGB::Black);
    FastLED.show();
}

void pushStatus();

// Draw one frame of the comets with heads at `head` and its mirror
void drawComets(int head) {
    fill_solid(leds, NUM_LEDS, EYE_COLOR);
    for (int k = 0; k <= COMET_TAIL; k++) {
        int pos = head - k;
        if (pos < 0) break;
        // Head is full white; tail fades back to the red underneath
        uint8_t amt = 255 - (255 * k) / (COMET_TAIL + 1);
        CRGB c = blend(EYE_COLOR, FLASH_COLOR, amt);
        leds[pos] = c;
        leds[NUM_LEDS - 1 - pos] = c;
    }
    FastLED.show();
}

void startAnimation() {
    animPhase  = ANIM_COMET;
    animStep   = 0;
    animNextMs = millis();
    Serial.println("Laser: fire");
}

// Advance the animation state machine; called every loop()
void updateAnimation() {
    if (animPhase == ANIM_IDLE) return;
    unsigned long now = millis();
    if ((long)(now - animNextMs) < 0) return;

    if (animPhase == ANIM_COMET) {
        // Heads meet at the center LED (or the middle pair for even counts)
        const int center = (NUM_LEDS - 1) / 2;
        drawComets(animStep);
        animNextMs = now + COMET_STEP_MS;
        if (++animStep > center) {
            animPhase = ANIM_FLASH;
            animStep  = 0;
        }
    } else {  // ANIM_FLASH: even steps = white, odd steps = red
        if (animStep >= FLASH_COUNT * 2) {
            animPhase = ANIM_IDLE;
            applyEye();
            pushStatus();
            return;
        }
        bool white = (animStep % 2) == 0;
        fill_solid(leds, NUM_LEDS, white ? FLASH_COLOR : EYE_COLOR);
        FastLED.show();
        animNextMs = now + (white ? FLASH_ON_MS : FLASH_OFF_MS);
        animStep++;
    }
}

// ---------------------------------------------------------------------------
// Status push (SSE)
// ---------------------------------------------------------------------------
void buildStatusJson(String &out) {
    out = "{\"eyeOn\":";
    out += eyeOn ? "true" : "false";
    out += ",\"animating\":";
    out += animPhase != ANIM_IDLE ? "true" : "false";
    out += "}";
}

void pushStatus() {
    String json;
    buildStatusJson(json);
    events.send(json.c_str(), "message", millis());
}

// ---------------------------------------------------------------------------
// Action dispatch (runs in loop())
// ---------------------------------------------------------------------------
void dispatchAction(const char *path) {
    if (strcmp(path, "eye") == 0) {
        eyeOn = !eyeOn;
        if (animPhase == ANIM_IDLE) applyEye();   // else applied when animation ends
        Serial.printf("Eye %s\n", eyeOn ? "ON" : "OFF");
    } else if (strcmp(path, "laser") == 0) {
        if (animPhase != ANIM_IDLE) return;       // ignore re-triggers mid-animation
        startAnimation();
    } else {
        Serial.printf("Unknown action: %s\n", path);
        return;
    }
    pushStatus();
}

// ---------------------------------------------------------------------------
// Page HTML — built once at startup into pageHtml
// ---------------------------------------------------------------------------
void buildPageHtml(String &out) {
    out.reserve(4096);
    out = "<!DOCTYPE html><html>"
          "<head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<link rel=\"icon\" href=\"data:,\">"
          "<title>Killbot</title>"
          "<style>"
          "* { margin:0; padding:0; box-sizing:border-box; }"
          "html { font-family:Helvetica,Arial,sans-serif; }"
          "body { background:#1a1a1a; color:#fff; padding:11px; padding-bottom:84px; }"
          "h1 { text-align:center; margin-bottom:15px; font-size:24px; }"
          "h2 { text-align:center; margin:15px 0 8px; font-size:17px; color:#aaa; }"
          ".action-wrap { max-width:800px; margin:0 auto 15px; display:flex; justify-content:center; }"
          ".btn-action { width:200px; height:200px; border:none; border-radius:50%; background:#c0392b;"
          "color:#fff; font-family:inherit; font-size:22px; font-weight:bold; cursor:pointer;"
          "transition:all .2s; box-shadow:0 4px 8px rgba(0,0,0,.4); -webkit-tap-highlight-color:transparent; }"
          ".btn-action:hover { transform:translateY(-2px); box-shadow:0 6px 12px rgba(0,0,0,.5); opacity:.9; }"
          ".btn-action:active { transform:translateY(0); box-shadow:0 2px 4px rgba(0,0,0,.3); }"
          ".btn-action.busy { background:#eee; color:#c0392b; }"
          ".toggle-grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr));"
          "gap:8px; max-width:800px; margin:0 auto 15px; }"
          ".toggle { background-color:#2a2a2a; border:1px solid #444; border-radius:6px; padding:12px 16px;"
          "cursor:pointer; display:flex; align-items:center; justify-content:space-between; gap:12px;"
          "font-size:15px; font-weight:bold; color:white; transition:all .2s;"
          "box-shadow:0 3px 5px rgba(0,0,0,.3); user-select:none; -webkit-tap-highlight-color:transparent; }"
          ".toggle:hover { background-color:#353535; transform:translateY(-2px); box-shadow:0 5px 9px rgba(0,0,0,.4); }"
          ".toggle:active { transform:translateY(0); }"
          ".toggle-switch { width:42px; height:24px; background:#555; border-radius:12px;"
          "position:relative; flex-shrink:0; transition:background .2s; }"
          ".toggle-switch::before { content:''; position:absolute; top:3px; left:3px;"
          "width:18px; height:18px; background:white; border-radius:50%; transition:transform .2s; }"
          ".toggle.on { background-color:#3a1f1f; border-color:#e74c3c; }"
          ".toggle.on .toggle-switch { background:#e74c3c; }"
          ".toggle.on .toggle-switch::before { transform:translateX(18px); }"
          ".status-bar { position:fixed; bottom:0; left:0; right:0; background:#2a2a2a;"
          "border-top:2px solid #444; padding:8px 11px; box-shadow:0 -2px 8px rgba(0,0,0,.5); }"
          ".status-bar h3 { margin:0 0 6px; font-size:11px; color:#888; text-align:center; }"
          ".status-grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(120px,1fr));"
          "gap:6px; max-width:800px; margin:0 auto; font-size:9px; }"
          ".status-item { background:#1a1a1a; padding:5px 8px; border-radius:3px; border:1px solid #444; }"
          ".status-item strong { color:#aaa; margin-right:5px; }"
          "</style></head>"
          "<body><h1>Killbot</h1>";

    out += "<h2>Actions</h2><div class=\"action-wrap\">"
           "<button class=\"btn-action\" id=\"btn-anim\" onclick=\"t('laser')\">LASER</button>"
           "</div>";

    out += "<h2>Toggles</h2><div class=\"toggle-grid\">"
           "<div class=\"toggle on\" id=\"tog-eye\" onclick=\"t('eye')\">"
           "<span>Eye</span><span class=\"toggle-switch\"></span>"
           "</div>"
           "</div>";

    out += "<div class=\"status-bar\"><h3>System Status</h3><div class=\"status-grid\">"
           "<div class=\"status-item\"><strong>Network:</strong> ";
    out += ap_ssid;
    out += " (192.168.4.1)</div>"
           "<div class=\"status-item\"><strong>Eye:</strong> <span id=\"eye\">&mdash;</span></div>"
           "<div class=\"status-item\"><strong>Laser:</strong> <span id=\"anim\">&mdash;</span></div>"
           "</div></div>";

    // Embedded JS: SSE for live status pushes, fire-and-forget action triggers
    out += "<script>"
           "function r(d){if(!d)return;"
           "document.getElementById('eye').textContent=d.eyeOn?'On':'Off';"
           "document.getElementById('tog-eye').classList.toggle('on',!!d.eyeOn);"
           "document.getElementById('anim').textContent=d.animating?'Firing':'Idle';"
           "document.getElementById('btn-anim').classList.toggle('busy',!!d.animating);}"
           "async function t(p){try{await fetch('/a/'+p);}catch(e){}}"
           "const es=new EventSource('/events');"
           "es.onmessage=e=>{try{r(JSON.parse(e.data));}catch(err){}};"
           "</script>"
           "</body></html>";
}

// ---------------------------------------------------------------------------
// Web server setup
// ---------------------------------------------------------------------------
void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html; charset=utf-8", pageHtml);
    });

    // SSE endpoint — clients open once, receive pushes
    events.onConnect([](AsyncEventSourceClient *client) {
        String json;
        buildStatusJson(json);
        client->send(json.c_str(), "message", millis());
    });
    server.addHandler(&events);

    // OS connectivity probes. Answering these the way each platform expects
    // marks the network as working, so the client keeps using it instead of
    // switching to cellular or nagging with a sign-in sheet.
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *req) {   // Android
        req->send(204);
    });
    server.on("/gen_204", HTTP_GET, [](AsyncWebServerRequest *req) {        // Android (older)
        req->send(204);
    });
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *req) {   // iOS/macOS
        req->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    });
    server.on("/library/test/success.html", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    });
    server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *req) {       // Windows
        req->send(200, "text/plain", "Microsoft NCSI");
    });
    server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/plain", "Microsoft Connect Test");
    });

    // /a/<path> action dispatcher — enqueue for loop() to execute
    server.onNotFound([](AsyncWebServerRequest *req) {
        const String &url = req->url();
        if (req->method() == HTTP_GET && url.startsWith("/a/")) {
            if (actionQueue) {
                ActionMsg msg;
                strncpy(msg.path, url.c_str() + 3, ACTION_PATH_MAX - 1);
                msg.path[ACTION_PATH_MAX - 1] = '\0';
                xQueueSend(actionQueue, &msg, 0);  // non-blocking; drop if full
            }
            req->send(200, "text/plain", "OK");
        } else if (req->method() == HTTP_GET) {
            // Anything else that reached us via the DNS catch-all: point it at
            // the admin page instead of a dead end.
            req->redirect("http://192.168.4.1/");
        } else {
            req->send(404, "text/plain", "Not Found");
        }
    });

    server.begin();
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("Killbot starting...");

    actionQueue = xQueueCreate(ACTION_QUEUE_DEPTH, sizeof(ActionMsg));

    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);
    applyEye();   // eye comes up solid red

    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(ap_ip, ap_ip, subnet);
#if CHANNEL_SCAN_ON_BOOT
    // Survey the band before bringing up the AP: counts per channel, plus the
    // strongest neighbor on each, so a crowded venue can be judged on site.
    Serial.println("Scanning 2.4GHz band (~2s)...");
    WiFi.mode(WIFI_STA);
    int found = WiFi.scanNetworks();
    int perChannel[14] = {0};
    int strongest[14];
    for (int i = 0; i < 14; i++) strongest[i] = -127;
    for (int i = 0; i < found; i++) {
        int ch = WiFi.channel(i);
        if (ch >= 1 && ch <= 13) {
            perChannel[ch]++;
            if (WiFi.RSSI(i) > strongest[ch]) strongest[ch] = WiFi.RSSI(i);
        }
    }
    Serial.printf("%d networks found\n", found);
    for (int ch = 1; ch <= 13; ch++) {
        Serial.printf("  ch %2d: %2d AP(s)%s%s\n", ch, perChannel[ch],
                      perChannel[ch] ? String(", strongest " + String(strongest[ch]) + " dBm").c_str() : "",
                      (ch == 1 || ch == 6 || ch == 11) ? "   <- non-overlapping" : "");
    }
    WiFi.scanDelete();
    WiFi.mode(WIFI_AP);
#endif

    WiFi.softAP(ap_ssid, ap_password, WIFI_CHANNEL);

    // Modem sleep adds tens to hundreds of ms of latency to inbound packets;
    // this rig is mains/battery powered with no need to save radio power.
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    Serial.printf("AP started: %s (channel %d)\n", ap_ssid, WIFI_CHANNEL);
    Serial.print("IP: ");         Serial.println(WiFi.softAPIP());

    if (MDNS.begin(mdns_host)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS responder started: http://%s.local\n", mdns_host);
    } else {
        Serial.println("mDNS init failed");
    }

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", ap_ip);
    Serial.println("Captive-portal DNS started on port 53");

    buildPageHtml(pageHtml);   // static content, built once
    setupWebServer();

    Serial.println("Ready.");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
    // Drain any actions queued by the async HTTP task
    if (actionQueue) {
        ActionMsg msg;
        while (xQueueReceive(actionQueue, &msg, 0) == pdTRUE) {
            dispatchAction(msg.path);
        }
    }

    updateAnimation();
    dnsServer.processNextRequest();

    unsigned long now = millis();
    if ((long)(now - nextHeartbeatMs) >= 0) {
        nextHeartbeatMs = now + SSE_HEARTBEAT_MS;
        pushStatus();
    }

    delay(2);
}
