/*
  Killbot Controller
  ESP32 firmware: a 45-LED WS2812B strip forms the robot's eye (solid red).
  Runs as its own WiFi AP and serves an admin web UI for control.
*/

#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include "secrets.h"

// ---------------------------------------------------------------------------
// Identity / WiFi
// ---------------------------------------------------------------------------
const char* ap_ssid     = AP_SSID;
const char* ap_password = AP_PASSWORD;
const char* mdns_host   = "killbot";   // reachable at http://killbot.local

// ---------------------------------------------------------------------------
// Eye LEDs
// ---------------------------------------------------------------------------
#define LED_PIN     13
#define NUM_LEDS    45
#define BRIGHTNESS  200
const CRGB EYE_COLOR = CRGB::Red;

CRGB leds[NUM_LEDS];
bool eyeOn = true;

// ---------------------------------------------------------------------------
// Action queue: async web handlers run on the AsyncTCP task, so they only
// enqueue a path here; loop() drains the queue and does the real work.
// ---------------------------------------------------------------------------
#define ACTION_PATH_MAX    32
#define ACTION_QUEUE_DEPTH 8
struct ActionMsg { char path[ACTION_PATH_MAX]; };
QueueHandle_t actionQueue = NULL;

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

// ---------------------------------------------------------------------------
// Status push (SSE)
// ---------------------------------------------------------------------------
void buildStatusJson(String &out) {
    out = "{\"eyeOn\":";
    out += eyeOn ? "true" : "false";
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
        applyEye();
        Serial.printf("Eye %s\n", eyeOn ? "ON" : "OFF");
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
           "</div></div>";

    // Embedded JS: SSE for live status pushes, fire-and-forget action triggers
    out += "<script>"
           "function r(d){if(!d)return;"
           "document.getElementById('eye').textContent=d.eyeOn?'On':'Off';"
           "document.getElementById('tog-eye').classList.toggle('on',!!d.eyeOn);}"
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

    IPAddress local_IP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(ap_ssid, ap_password);

    Serial.print("AP started: "); Serial.println(ap_ssid);
    Serial.print("IP: ");         Serial.println(WiFi.softAPIP());

    if (MDNS.begin(mdns_host)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS responder started: http://%s.local\n", mdns_host);
    } else {
        Serial.println("mDNS init failed");
    }

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

    delay(2);
}
