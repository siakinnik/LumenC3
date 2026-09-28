#pragma once
// ================== HARDWARE ==================
#define DATA_PIN        4          // strip data pin (addressable, on GPIO4)
#define NUM_LEDS        30         // number of LEDs in the strip
#define LED_TYPE        WS2812B    // WS2812B / WS2811 / SK6812 ...
#define COLOR_ORDER     GRB        // WS2812 is usually GRB; if colors are swapped, change to RGB/BRG
#define MAX_MILLIAMPS   1000       // PSU current limit (mA), FastLED will throttle brightness itself

// ================== WI-FI (captive portal) ==================
// On first boot (or after a reset) an access point with this name is started.
// Connect to it with your phone, the portal opens, enter your Wi-Fi — and the strip is online.
#define AP_SSID         "LED-Strip-Setup"
#define AP_PASSWORD     "ledsetup123"   // >= 8 characters, or "" for an open network
#define HOSTNAME        "ledstrip"      // network address: http://ledstrip.local
#define PORTAL_TIMEOUT  180             // sec: how long to wait for setup before running offline

// ================== SERIAL RESET ==================
// Send exactly these bytes over Serial to erase the saved Wi-Fi and settings.
// Change them to your own "random" ones — less chance of an accidental reset.
#define RESET_MAGIC     { 0x5A, 0xA5, 0xC3, 0x3C, 0xDE, 0xAD }
#define RESET_MAGIC_LEN 6

// ================== SERIAL PROTOCOL ==================
// Binary control protocol over USB Serial, see PROTOCOL.md.
#define PROTO_VERSION     2
#define PROTO_SIGNATURE   "LMC3"   // sent in PONG so hosts can tell this board from other ESP32s
#define PROTO_SOF         0xAA     // start-of-frame byte (never appears in the ASCII log output)
#define PROTO_MAX_PAYLOAD 16       // longest accepted payload, bytes
#define PROTO_TIMEOUT_MS  100      // max gap between bytes of one frame before it's dropped

// ================== EFFECTS ==================
#define FRAME_MS        16         // one frame every N ms (~60 FPS)
#define DEF_BRIGHTNESS  160        // initial brightness 0..255
#define DEF_SPEED       50         // initial speed 1..100

// list of modes; order = switching order
enum Mode {
  M_SOLID, M_RAINBOW, M_FIRE, M_COMET, M_TWINKLE, M_BREATHE, M_CHASE, M_COUNT
};
#define MODE_NAMES { "solid", "rainbow", "fire", "comet", "twinkle", "breathe", "chase" }
