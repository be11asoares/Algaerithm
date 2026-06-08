#include <Adafruit_AS7341.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <time.h>

// ── Config ────────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3C
#define NUM_AVERAGES   5
#define BTN_BLANK      1
#define BTN_READING    2
#define MAX_READINGS   100

// LED ring + spectrometer light
#define RING_PIN       3      // WS2812 data line (2x12 chained = 24 LEDs)
#define RING_COUNT     24
#define SPEC_LED_PIN   4      // white spectrometer LED — MOVE LED +ve leg from 3V3 to GPIO4 (keep 220R)
#define SPEC_SETTLE_MS 120    // let LED stabilise before measuring

const char* ssid      = "YOUR_WIFI_NAME";
const char* password  = "YOUR_WIFI_PASSWORD";
const char* ntpServer = "pool.ntp.org";
const char* timezone  = "GMT0BST,M3.5.0/1,M10.5.0";

// ── Globals ───────────────────────────────────────────────────────────────────
Adafruit_AS7341  as7341;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_NeoPixel ring(RING_COUNT, RING_PIN, NEO_GRB + NEO_KHZ800);
WebServer        server(80);

float blankRef[12] = {0};
bool  hasBlank     = false;
bool  timeReady    = false;

// Grow-light state (persisted in RAM, restored after each measurement)
bool    lightsOn         = false;
uint8_t lightR           = 255;
uint8_t lightG           = 255;
uint8_t lightB           = 255;
uint8_t lightBrightness  = 120;   // 0–255

struct Reading {
  char     datetime[24];
  float    A415, A445, A480, A515, A555, A590, A630, A680;
  uint16_t raw[12];
  bool     valid;
};

Reading history[MAX_READINGS];
int historyCount = 0;

// Forward declarations
void  takeBlank();
void  takeReading();
bool  getAveragedReading(uint16_t avg[12]);
void  printBar(const char* label, uint16_t value);
float calcAbsorbance(float sample, float blank);
void  printAbsorbance(const char* label, float sample, float blank);
void  applyLights();
void  ringsOff();

void getDatetime(char* buf, int len) {
  if (!timeReady) { snprintf(buf, len, "No time sync"); return; }
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) { snprintf(buf, len, "Time error"); return; }
  strftime(buf, len, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

// ── LED ring helpers ──────────────────────────────────────────────────────────
void applyLights() {
  ring.setBrightness(lightBrightness);
  if (lightsOn) {
    digitalWrite(SPEC_LED_PIN, LOW);   // rings on => spectrometer LED must be off
    for (int i = 0; i < RING_COUNT; i++) ring.setPixelColor(i, ring.Color(lightR, lightG, lightB));
  } else {
    ring.clear();
  }
  ring.show();
}

void ringsOff() {
  ring.clear();
  ring.show();
}

// ── Measurement interlock ─────────────────────────────────────────────────────
// Rings OFF, spectrometer LED ON, measure, LED OFF, restore rings.
void beginMeasurementLight() {
  ringsOff();                       // kill grow lights so they don't pollute the reading
  digitalWrite(SPEC_LED_PIN, HIGH); // spectrometer light on
  delay(SPEC_SETTLE_MS);
}

void endMeasurementLight() {
  digitalWrite(SPEC_LED_PIN, LOW);  // spectrometer light off
  applyLights();                    // restore whatever the grow lights were doing
}

// ── Webpage ───────────────────────────────────────────────────────────────────
const char webpage[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>AlgaeScope</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=DM+Mono:wght@400;500&family=Fraunces:opsz,wght@9..144,300;9..144,600&display=swap');

  :root {
    --bg:      #0a0f0a;
    --surface: #111811;
    --border:  #1e2e1e;
    --green:   #4ade80;
    --yellow:  #facc15;
    --red:     #f87171;
    --blue:    #60a5fa;
    --muted:   #3d5c3d;
    --text:    #e2f0e2;
    --subtext: #7a9e7a;
    --hover:   #1a2e1a;
    --selected:#162616;
  }

  * { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    background: var(--bg);
    color: var(--text);
    font-family: 'DM Mono', monospace;
    min-height: 100vh;
    padding: 2rem;
    max-width: 1200px;
    margin: 0 auto;
  }

  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 2rem;
    border-bottom: 1px solid var(--border);
    padding-bottom: 1.5rem;
  }

  .header-left { display: flex; align-items: baseline; gap: 1rem; }

  h1 {
    font-family: 'Fraunces', serif;
    font-size: 2.2rem;
    font-weight: 300;
    color: var(--green);
    letter-spacing: -0.03em;
  }

  .subtitle { color: var(--subtext); font-size: 0.75rem; }

  .guide-btn {
    font-family: 'DM Mono', monospace;
    font-size: 0.75rem;
    padding: 0.5rem 1rem;
    border-radius: 6px;
    border: 1px solid var(--border);
    background: transparent;
    color: var(--subtext);
    cursor: pointer;
    transition: all 0.15s;
    display: flex;
    align-items: center;
    gap: 0.4rem;
  }
  .guide-btn:hover { border-color: var(--green); color: var(--green); }

  /* ── Mode toggle ─────────────────────────────────────────── */
  .mode-toggle {
    display: inline-flex;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 0.25rem;
    margin-bottom: 2rem;
    gap: 0.25rem;
  }
  .mode-toggle button {
    font-family: 'DM Mono', monospace;
    font-size: 0.8rem;
    padding: 0.55rem 1.4rem;
    border-radius: 6px;
    border: none;
    background: transparent;
    color: var(--subtext);
    cursor: pointer;
    transition: all 0.15s;
  }
  .mode-toggle button.active {
    background: var(--green);
    color: #0a0f0a;
    font-weight: 500;
  }

  .view { display: none; }
  .view.active { display: block; }

  .status-bar { display: flex; gap: 1rem; margin-bottom: 2rem; flex-wrap: wrap; }

  .stat {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 0.75rem 1.25rem;
    min-width: 130px;
  }

  .stat-label {
    font-size: 0.65rem;
    color: var(--subtext);
    text-transform: uppercase;
    letter-spacing: 0.1em;
    margin-bottom: 0.3rem;
  }

  .stat-value { font-size: 1rem; font-weight: 500; color: var(--green); }

  .grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 1.5rem;
    margin-bottom: 1.5rem;
  }

  @media (max-width: 700px) {
    .grid { grid-template-columns: 1fr; }
    body  { padding: 1rem; }
  }

  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 1.25rem;
    margin-bottom: 1.5rem;
  }

  .card h2 {
    font-family: 'Fraunces', serif;
    font-size: 1rem;
    font-weight: 300;
    color: var(--subtext);
    margin-bottom: 1rem;
  }

  .card-title-row {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    margin-bottom: 1rem;
  }

  .card-title-row h2 { margin-bottom: 0; }

  .viewing-label {
    font-size: 0.65rem;
    color: var(--subtext);
    font-style: italic;
  }

  .summary-box {
    background: var(--bg);
    border: 1px solid var(--border);
    border-left: 3px solid var(--green);
    border-radius: 6px;
    padding: 0.85rem 1rem;
    margin-bottom: 1rem;
    font-size: 0.75rem;
    line-height: 1.7;
    color: var(--text);
  }

  .summary-box.warn  { border-left-color: var(--yellow); }
  .summary-box.alert { border-left-color: var(--red); }
  .summary-box.low   { border-left-color: var(--blue); }

  .inline-stats {
    display: flex;
    gap: 1rem;
    margin-bottom: 1rem;
    flex-wrap: wrap;
  }

  .inline-stat {
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 0.5rem 0.85rem;
    font-size: 0.7rem;
  }

  .inline-stat-label {
    color: var(--subtext);
    font-size: 0.6rem;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    margin-bottom: 0.2rem;
  }

  .inline-stat-value { color: var(--text); font-weight: 500; }

  .trend-up   { color: var(--green); }
  .trend-down { color: var(--red); }
  .trend-flat { color: var(--subtext); }

  .channel {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    margin-bottom: 0.55rem;
    position: relative;
  }

  .channel-label {
    font-size: 0.7rem;
    color: var(--subtext);
    width: 95px;
    flex-shrink: 0;
    cursor: help;
  }

  .bar-track {
    flex: 1; height: 8px;
    background: var(--border);
    border-radius: 4px;
    overflow: visible;
    position: relative;
  }

  .bar-fill { height: 100%; border-radius: 4px; transition: width 0.5s ease; }

  .channel-value { font-size: 0.72rem; color: var(--text); width: 52px; text-align: right; }
  .channel-raw   { font-size: 0.65rem; color: var(--muted); width: 52px; text-align: right; }

  .tooltip {
    display: none;
    position: absolute;
    left: 0;
    top: 120%;
    background: #1a2e1a;
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 0.6rem 0.8rem;
    font-size: 0.68rem;
    color: var(--text);
    line-height: 1.5;
    width: 240px;
    z-index: 100;
    pointer-events: none;
    box-shadow: 0 4px 16px rgba(0,0,0,0.4);
  }

  .tooltip strong { color: var(--green); display: block; margin-bottom: 0.2rem; }
  .channel:hover .tooltip { display: block; }

  .density {
    display: inline-block;
    padding: 0.4rem 1rem;
    border-radius: 20px;
    font-size: 0.8rem;
    font-weight: 500;
    margin-top: 0.75rem;
  }
  .density.ok     { background: #14532d; color: var(--green); }
  .density.dense  { background: #713f12; color: var(--yellow); }
  .density.dilute { background: #7f1d1d; color: var(--red); }
  .density.low    { background: #1e3a5f; color: var(--blue); }

  canvas { width: 100%; height: 200px; display: block; }

  .table-wrap { overflow-x: auto; }

  table { width: 100%; border-collapse: collapse; font-size: 0.68rem; }

  th {
    color: var(--subtext);
    text-align: left;
    padding: 0.5rem 0.6rem;
    border-bottom: 1px solid var(--border);
    font-weight: 400;
    text-transform: uppercase;
    letter-spacing: 0.07em;
    font-size: 0.6rem;
    white-space: nowrap;
  }

  td {
    padding: 0.45rem 0.6rem;
    border-bottom: 1px solid var(--border);
    color: var(--text);
    white-space: nowrap;
  }

  tr:last-child td { border-bottom: none; }
  tbody tr { cursor: pointer; transition: background 0.1s; }
  tbody tr:hover td { background: var(--hover); }
  tbody tr.selected td { background: var(--selected); border-left: 2px solid var(--green); }
  tbody tr.selected td:first-child { padding-left: calc(0.6rem - 2px); }

  .pill {
    display: inline-block;
    padding: 0.15rem 0.5rem;
    border-radius: 10px;
    font-size: 0.62rem;
  }
  .pill.ok     { background: #14532d; color: var(--green); }
  .pill.dense  { background: #713f12; color: var(--yellow); }
  .pill.dilute { background: #7f1d1d; color: var(--red); }
  .pill.low    { background: #1e3a5f; color: var(--blue); }

  .actions { display: flex; gap: 0.75rem; margin-bottom: 1.5rem; flex-wrap: wrap; }

  button {
    font-family: 'DM Mono', monospace;
    font-size: 0.75rem;
    padding: 0.6rem 1.25rem;
    border-radius: 6px;
    border: 1px solid var(--border);
    cursor: pointer;
    transition: all 0.15s;
  }

  .btn-primary   { background: var(--green); color: #0a0f0a; border-color: var(--green); font-weight: 500; }
  .btn-primary:hover   { opacity: 0.85; }
  .btn-secondary { background: transparent; color: var(--subtext); }
  .btn-secondary:hover { border-color: var(--subtext); color: var(--text); }
  .btn-measure   { background: var(--blue); color: #0a0f0a; border-color: var(--blue); font-weight: 500; }
  .btn-measure:hover { opacity: 0.85; }
  button:disabled { opacity: 0.4; cursor: not-allowed; }

  .live-dot {
    display: inline-block;
    width: 6px; height: 6px;
    background: var(--green);
    border-radius: 50%;
    margin-right: 0.4rem;
    animation: pulse 2s infinite;
  }

  @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.3} }

  .no-data { color: var(--muted); font-size: 0.8rem; text-align: center; padding: 2rem; }

  .click-hint {
    font-size: 0.62rem;
    color: var(--muted);
    margin-top: 0.5rem;
    text-align: right;
  }

  .toast {
    position: fixed;
    bottom: 1.5rem; left: 50%;
    transform: translateX(-50%) translateY(150%);
    background: var(--surface);
    border: 1px solid var(--green);
    color: var(--green);
    padding: 0.7rem 1.4rem;
    border-radius: 8px;
    font-size: 0.78rem;
    z-index: 2000;
    transition: transform 0.3s ease;
    box-shadow: 0 6px 24px rgba(0,0,0,0.5);
  }
  .toast.show { transform: translateX(-50%) translateY(0); }
  .toast.busy { border-color: var(--blue); color: var(--blue); }

  /* ── Grow light controls ─────────────────────────────────── */
  .light-controls { display: flex; flex-direction: column; gap: 1.25rem; max-width: 460px; }

  .light-presets { display: flex; gap: 0.6rem; flex-wrap: wrap; margin-bottom: 0.25rem; }
  .preset-btn {
    flex: 1; min-width: 90px;
    font-family: 'DM Mono', monospace;
    font-size: 0.78rem;
    padding: 0.7rem 0.5rem;
    border-radius: 8px;
    border: 1px solid var(--border);
    background: var(--bg);
    color: var(--text);
    cursor: pointer;
    transition: all 0.15s;
  }
  .preset-btn:hover { border-color: var(--green); color: var(--green); }
  .preset-btn.off:hover { border-color: var(--subtext); color: var(--subtext); }

  .light-row { display: flex; align-items: center; justify-content: space-between; gap: 1rem; }

  .light-label { font-size: 0.78rem; color: var(--text); }

  .switch { position: relative; width: 52px; height: 28px; flex-shrink: 0; }
  .switch input { opacity: 0; width: 0; height: 0; }
  .slider-sw {
    position: absolute; inset: 0;
    background: var(--border);
    border-radius: 28px;
    cursor: pointer;
    transition: 0.2s;
  }
  .slider-sw::before {
    content: ''; position: absolute;
    height: 20px; width: 20px; left: 4px; bottom: 4px;
    background: var(--subtext); border-radius: 50%; transition: 0.2s;
  }
  .switch input:checked + .slider-sw { background: #14532d; }
  .switch input:checked + .slider-sw::before { transform: translateX(24px); background: var(--green); }

  input[type=range] {
    -webkit-appearance: none; appearance: none;
    width: 220px; height: 6px;
    background: var(--border); border-radius: 4px; outline: none;
  }
  input[type=range]::-webkit-slider-thumb {
    -webkit-appearance: none; appearance: none;
    width: 18px; height: 18px; border-radius: 50%;
    background: var(--green); cursor: pointer;
  }
  input[type=range]::-moz-range-thumb {
    width: 18px; height: 18px; border-radius: 50%;
    background: var(--green); cursor: pointer; border: none;
  }

  input[type=color] {
    -webkit-appearance: none; appearance: none;
    width: 52px; height: 32px;
    border: 1px solid var(--border); border-radius: 6px;
    background: transparent; cursor: pointer; padding: 2px;
  }
  input[type=color]::-webkit-color-swatch-wrapper { padding: 0; }
  input[type=color]::-webkit-color-swatch { border: none; border-radius: 4px; }

  .light-value { font-size: 0.72rem; color: var(--subtext); width: 44px; text-align: right; }

  .light-note {
    font-size: 0.66rem; color: var(--muted); line-height: 1.6; margin-top: 0.5rem;
    border-top: 1px solid var(--border); padding-top: 0.85rem;
  }

  .overlay {
    display: none;
    position: fixed;
    inset: 0;
    background: rgba(0,0,0,0.85);
    z-index: 1000;
    overflow-y: auto;
    padding: 2rem;
  }

  .overlay.open { display: block; }

  .overlay-inner {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 16px;
    max-width: 900px;
    margin: 0 auto;
    padding: 2rem;
    position: relative;
  }

  .overlay-header {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    margin-bottom: 2rem;
    padding-bottom: 1rem;
    border-bottom: 1px solid var(--border);
  }

  .overlay-header h2 {
    font-family: 'Fraunces', serif;
    font-size: 1.6rem;
    font-weight: 300;
    color: var(--green);
    margin-bottom: 0;
  }

  .close-btn {
    background: transparent;
    border: 1px solid var(--border);
    color: var(--subtext);
    font-size: 0.8rem;
    padding: 0.4rem 0.8rem;
    border-radius: 6px;
    cursor: pointer;
  }
  .close-btn:hover { border-color: var(--red); color: var(--red); }

  .ref-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
    gap: 1rem;
  }

  .ref-block {
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 1rem;
  }

  .ref-block h3 {
    font-family: 'Fraunces', serif;
    font-size: 0.9rem;
    font-weight: 300;
    color: var(--text);
    margin-bottom: 0.6rem;
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }

  .ref-dot {
    display: inline-block;
    width: 8px; height: 8px;
    border-radius: 50%;
    flex-shrink: 0;
  }

  .ref-block ul { list-style: none; padding: 0; }

  .ref-block ul li {
    font-size: 0.68rem;
    color: var(--subtext);
    line-height: 1.7;
    padding-left: 0.75rem;
    position: relative;
  }

  .ref-block ul li::before {
    content: '→';
    position: absolute;
    left: 0;
    color: var(--muted);
  }

  .ref-block ul li strong { color: var(--text); }

  .ref-block p {
    font-size: 0.68rem;
    color: var(--subtext);
    line-height: 1.6;
    margin-bottom: 0.5rem;
  }

  .ratio-table { width: 100%; border-collapse: collapse; margin-top: 0.4rem; }

  .ratio-table td {
    font-size: 0.65rem;
    padding: 0.3rem 0.4rem;
    border-bottom: 1px solid var(--border);
    color: var(--subtext);
    white-space: normal;
  }

  .ratio-table td:first-child { color: var(--text); width: 30%; }
  .ratio-table tr:last-child td { border-bottom: none; }
</style>
</head>
<body>

<!-- ── Guide overlay ──────────────────────────────────────────────────────── -->
<div class="overlay" id="guide-overlay" onclick="closeGuideOnBg(event)">
  <div class="overlay-inner">
    <div class="overlay-header">
      <h2>Reference Guide</h2>
      <button class="close-btn" onclick="closeGuide()">✕ Close</button>
    </div>
    <div class="ref-grid">

      <div class="ref-block">
        <h3><span class="ref-dot" style="background:#4ade80"></span> Signs of healthy growth</h3>
        <ul>
          <li><strong>A680 rising over time</strong> — chlorophyll a increasing, culture is growing</li>
          <li><strong>A445 rising alongside A680</strong> — both chlorophyll peaks increasing together is the classic healthy signature</li>
          <li><strong>A680 between 0.1 – 0.8</strong> — readings in the reliable measurement range</li>
          <li><strong>Stress ratio below 2.5</strong> — yellow pigments not dominating, cells not stressed</li>
          <li><strong>Consistent readings</strong> between sessions — no flask movement issues</li>
        </ul>
      </div>

      <div class="ref-block">
        <h3><span class="ref-dot" style="background:#818cf8"></span> What each channel measures</h3>
        <ul>
          <li><strong>415nm Violet</strong> — near-UV edge, minor pigment absorption</li>
          <li><strong>445nm Blue — Chl a</strong> — primary chlorophyll a absorption. Key growth indicator</li>
          <li><strong>480nm Cyan — Carotenoids</strong> — protective and accessory pigments</li>
          <li><strong>515nm Green</strong> — algae reflect green, so this stays low in healthy cultures</li>
          <li><strong>555nm / 590nm Yellow</strong> — carotenoid region. Rises under stress</li>
          <li><strong>630nm Red — Chl b</strong> — accessory chlorophyll, supports light harvesting</li>
          <li><strong>680nm Deep Red — Chl a</strong> — main chlorophyll a peak. Primary growth metric</li>
        </ul>
      </div>

      <div class="ref-block">
        <h3><span class="ref-dot" style="background:#f87171"></span> Warning signs</h3>
        <ul>
          <li><strong>A680 not rising</strong> — culture may be in lag phase, or growth has stalled</li>
          <li><strong>A590 high but A680 low</strong> — stress response. Check light, temperature, nutrients</li>
          <li><strong>Negative absorbance</strong> — sample clearer than blank. Culture too dilute or blank needs retaking</li>
          <li><strong>All values above 1.0</strong> — too dense. Dilute and retake blank</li>
          <li><strong>Inconsistent readings</strong> — flask not in fixed position</li>
        </ul>
      </div>

      <div class="ref-block">
        <h3><span class="ref-dot" style="background:#facc15"></span> Stress ratio (A590 ÷ A680)</h3>
        <p>How much yellow protective pigment vs green chlorophyll. Only shown when A680 is above 0.1:</p>
        <table class="ratio-table">
          <tr><td>&lt; 1.5</td><td>Very healthy — strong chlorophyll dominance</td></tr>
          <tr><td>1.5 – 2.5</td><td>Normal healthy range</td></tr>
          <tr><td>2.5 – 3.5</td><td>Mild stress — monitor closely</td></tr>
          <tr><td>&gt; 3.5</td><td>Significant stress — check light, temperature, nutrients</td></tr>
        </table>
      </div>

      <div class="ref-block">
        <h3><span class="ref-dot" style="background:#60a5fa"></span> Culture density guide (A680)</h3>
        <table class="ratio-table">
          <tr><td>&lt; 0.05</td><td>Very low — early stage, just inoculated</td></tr>
          <tr><td>0.05 – 0.1</td><td>Low — early growth, readings becoming reliable</td></tr>
          <tr><td>0.1 – 0.6</td><td>Ideal range — accurate, reliable measurements</td></tr>
          <tr><td>0.6 – 0.8</td><td>Dense — consider diluting for next reading</td></tr>
          <tr><td>0.8 – 1.0</td><td>Too dense — dilute 1:2 with fresh media</td></tr>
          <tr><td>&gt; 1.0</td><td>Saturated — dilute 1:4 or more, retake blank</td></tr>
        </table>
      </div>

      <div class="ref-block">
        <h3><span class="ref-dot" style="background:#94a3b8"></span> How to dilute your sample</h3>
        <ul>
          <li><strong>1:2 dilution</strong> — 1 part algae + 1 part fresh media. Halves all readings</li>
          <li><strong>1:4 dilution</strong> — 1 part algae + 3 parts fresh media. Quarters all readings</li>
          <li><strong>Always retake the blank</strong> after diluting, using the same diluted media</li>
          <li><strong>Multiply A680</strong> by the dilution factor to get the true culture density (×2 for 1:2, ×4 for 1:4)</li>
        </ul>
      </div>

    </div>
  </div>
</div>

<!-- ── Main page ──────────────────────────────────────────────────────────── -->
<header>
  <div class="header-left">
    <h1>AlgaeScope</h1>
    <span class="subtitle"><span class="live-dot"></span>live · AS7341</span>
  </div>
  <button class="guide-btn" onclick="openGuide()">📖 Reference guide</button>
</header>

<!-- Mode toggle -->
<div class="mode-toggle">
  <button id="tab-spec"  class="active" onclick="setMode('spec')">🔬 Spectrometer</button>
  <button id="tab-light"           onclick="setMode('light')">💡 Grow Lights</button>
</div>

<!-- ══════════════════ SPECTROMETER VIEW ══════════════════ -->
<div class="view active" id="view-spec">

  <div class="status-bar">
    <div class="stat">
      <div class="stat-label">Readings</div>
      <div class="stat-value" id="count">—</div>
    </div>
    <div class="stat">
      <div class="stat-label">Blank set</div>
      <div class="stat-value" id="blank-status">—</div>
    </div>
    <div class="stat">
      <div class="stat-label">Last A680</div>
      <div class="stat-value" id="last-a680">—</div>
    </div>
    <div class="stat">
      <div class="stat-label">Device time</div>
      <div class="stat-value" id="device-time" style="font-size:0.75rem">—</div>
    </div>
  </div>

  <div class="actions">
    <button class="btn-measure" id="btn-blank"   onclick="takeBlank()">◎ Take Blank</button>
    <button class="btn-measure" id="btn-reading" onclick="takeReading()">▶ Take Reading</button>
    <button class="btn-primary"   onclick="downloadCSV()">⬇ Download CSV</button>
    <button class="btn-secondary" onclick="clearHistory()">Clear history</button>
  </div>

  <!-- 1. Reading history -->
  <div class="card">
    <h2>Reading history</h2>
    <div class="table-wrap">
      <table>
        <thead>
          <tr>
            <th>#</th>
            <th>Date &amp; Time</th>
            <th>A415 Violet</th>
            <th>A445 Chl a (blue)</th>
            <th>A480 Carotenoids</th>
            <th>A515 Green</th>
            <th>A555 Yellow-Green</th>
            <th>A590 Yellow</th>
            <th>A630 Chl b (red)</th>
            <th>A680 Chl a (deep red)</th>
            <th>Stress ratio</th>
            <th>Status</th>
          </tr>
        </thead>
        <tbody id="history-body">
          <tr><td colspan="12" class="no-data">No readings yet</td></tr>
        </tbody>
      </table>
    </div>
    <div class="click-hint">Click any row to inspect that reading in the panel below →</div>
  </div>

  <!-- 2. Bar chart + growth curve -->
  <div class="grid">

    <div class="card">
      <div class="card-title-row">
        <h2 id="panel-title">Latest reading</h2>
        <span class="viewing-label" id="viewing-label"></span>
      </div>
      <div id="panel-time" style="font-size:0.65rem;color:var(--subtext);margin-bottom:0.75rem"></div>

      <div class="summary-box" id="summary-box" style="display:none"></div>

      <div class="inline-stats" id="inline-stats" style="display:none">
        <div class="inline-stat">
          <div class="inline-stat-label">Stress ratio (A590 ÷ A680)</div>
          <div class="inline-stat-value" id="stress-ratio-val">—</div>
        </div>
        <div class="inline-stat">
          <div class="inline-stat-label">A680 vs previous reading</div>
          <div class="inline-stat-value" id="trend-val">—</div>
        </div>
      </div>

      <div style="font-size:0.62rem;color:var(--muted);margin-bottom:0.75rem;text-transform:uppercase;letter-spacing:0.08em">
        ← Absorbance &nbsp;|&nbsp; Raw count →
      </div>

      <div id="latest-channels"><div class="no-data">Waiting for first reading…</div></div>
      <div id="density-badge"></div>
    </div>

    <div class="card">
      <h2>Culture growth over time</h2>
      <p style="font-size:0.68rem;color:var(--subtext);margin-bottom:1rem;line-height:1.5">
        Tracks chlorophyll a (deep red, 680nm) — the most reliable indicator of algae biomass.
        A rising line means your culture is actively growing.
        The shaded band is the ideal measurement range (0.1–0.8).
      </p>
      <canvas id="chart"></canvas>
    </div>

  </div>
</div>

<!-- ══════════════════ GROW LIGHTS VIEW ══════════════════ -->
<div class="view" id="view-light">
  <div class="card">
    <h2>Grow light control</h2>
    <div class="light-controls">

      <div class="light-presets">
        <button class="preset-btn" onclick="applyPreset('grow')">🌱 Grow</button>
        <button class="preset-btn" onclick="applyPreset('boost')">🔵 Boost</button>
        <button class="preset-btn" onclick="applyPreset('maintain')">🌙 Maintain</button>
        <button class="preset-btn off" onclick="applyPreset('off')">○ Off</button>
      </div>

      <div class="light-row">
        <span class="light-label">Ring lights</span>
        <label class="switch">
          <input type="checkbox" id="light-power" onchange="pushLights()">
          <span class="slider-sw"></span>
        </label>
      </div>

      <div class="light-row">
        <span class="light-label">Colour</span>
        <input type="color" id="light-color" value="#ffffff" oninput="pushLights()">
      </div>

      <div class="light-row">
        <span class="light-label">Brightness</span>
        <input type="range" id="light-bright" min="0" max="255" value="120" oninput="onBrightInput()" onchange="pushLights()">
        <span class="light-value" id="bright-val">120</span>
      </div>

      <div class="light-note">
        24 LEDs (2×12 rings) on GPIO3. The grow lights switch off automatically
        during every blank and reading so they don't interfere with the spectrometer,
        then return to these settings afterwards.
      </div>
    </div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
let allReadings = [];
let selectedIdx = null;
let mode = 'spec';
let busy = false;          // true while a blank/reading is in progress
let lightState = { on:false, r:255, g:255, b:255, brightness:120 };
let lightDirty = false;    // user touched a control; don't overwrite from /data poll

const tooltips = {
  '415nm Violet':  { title:'415nm Violet', text:'Near the edge of visible light. Minor absorption by some pigments. Low values are normal.' },
  '445nm Chl a':   { title:'445nm Blue — Chlorophyll a', text:'Primary absorption peak of chlorophyll a. This is your key growth indicator. Rising values mean more chlorophyll, more growth.' },
  '480nm Caro':    { title:'480nm Cyan — Carotenoids', text:'Carotenoids are yellow-orange protective pigments. Some level is healthy and normal. Very high values relative to A680 can indicate stress.' },
  '515nm Green':   { title:'515nm Green', text:'Healthy algae reflect green light rather than absorbing it, so this channel typically stays low. Rising values can indicate pigment changes.' },
  '555nm Y-Green': { title:'555nm Yellow-Green', text:'Transition zone between green and yellow. Rises under stress or when carotenoids increase.' },
  '590nm Yellow':  { title:'590nm Yellow', text:'Used in the stress ratio (A590 ÷ A680). High values relative to deep red chlorophyll suggest the culture is under stress.' },
  '630nm Chl b':   { title:'630nm Red — Chlorophyll b', text:'Chlorophyll b is an accessory pigment that helps harvest light and pass energy to chlorophyll a. Healthy cultures have both Chl a and Chl b.' },
  '680nm Chl a':   { title:'680nm Deep Red — Chlorophyll a', text:'The main absorption peak of chlorophyll a and your most important number. This is what the growth curve tracks. A rising value means your culture is growing.' },
  'Clear':         { title:'Clear — Broadband light', text:'Captures all light with no filter. Used as a reference for overall signal strength. Should be in the 10,000–50,000 range for accurate readings.' },
  'NIR':           { title:'NIR — Near Infrared', text:'Light just beyond human vision. White LEDs produce very little NIR so this value is typically low. Not used in algae growth calculations.' },
};

// ── Mode toggle ───────────────────────────────────────────────────────────────
function setMode(m) {
  mode = m;
  document.getElementById('tab-spec').classList.toggle('active',  m === 'spec');
  document.getElementById('tab-light').classList.toggle('active', m === 'light');
  document.getElementById('view-spec').classList.toggle('active',  m === 'spec');
  document.getElementById('view-light').classList.toggle('active', m === 'light');
}

// ── Toast ───────────────────────────────────────────────────────────────────
let toastTimer = null;
function toast(msg, kind) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.className = 'toast show' + (kind === 'busy' ? ' busy' : '');
  clearTimeout(toastTimer);
  if (kind !== 'busy') toastTimer = setTimeout(() => t.className = 'toast', 2200);
}
function hideToast() { document.getElementById('toast').className = 'toast'; }

// ── Colour helpers ────────────────────────────────────────────────────────────
function hexToRgb(hex) {
  const n = parseInt(hex.slice(1), 16);
  return { r:(n>>16)&255, g:(n>>8)&255, b:n&255 };
}
function rgbToHex(r,g,b) {
  return '#' + [r,g,b].map(v => v.toString(16).padStart(2,'0')).join('');
}

// ── Blank / Reading (dashboard control) ───────────────────────────────────────
function setMeasureButtons(disabled) {
  document.getElementById('btn-blank').disabled   = disabled;
  document.getElementById('btn-reading').disabled = disabled;
}

async function takeBlank() {
  if (busy) return;
  busy = true; setMeasureButtons(true);
  toast('Taking blank… lights off, measuring', 'busy');
  try {
    await fetch('/blank', { method:'POST' });
    await fetchData();
    toast('Blank saved ✓');
  } catch(e) {
    toast('Blank failed — try again');
  } finally {
    busy = false; setMeasureButtons(false);
  }
}

async function takeReading() {
  if (busy) return;
  busy = true; setMeasureButtons(true);
  toast('Taking reading… lights off, measuring', 'busy');
  try {
    const res = await fetch('/reading', { method:'POST' });
    const txt = await res.text();
    await fetchData();
    if (txt.indexOf('noblank') !== -1) toast('No blank set — take a blank first');
    else toast('Reading saved ✓');
  } catch(e) {
    toast('Reading failed — try again');
  } finally {
    busy = false; setMeasureButtons(false);
  }
}

// ── Grow lights ───────────────────────────────────────────────────────────────
const PRESETS = {
  grow:     { on:true,  hex:'#ff3866', br:90 },  // red-dominant: everyday active growth
  boost:    { on:true,  hex:'#cc44ff', br:120 }, // blue-rich: biomass building / log phase
  maintain: { on:true,  hex:'#ff5a3c', br:45 },  // dim warm red: low-energy upkeep
  off:      { on:false, hex:'#ff3866', br:90 },  // off
};

function applyPreset(name) {
  const p = PRESETS[name];
  if (!p) return;
  document.getElementById('light-power').checked = p.on;
  document.getElementById('light-color').value   = p.hex;
  document.getElementById('light-bright').value  = p.br;
  document.getElementById('bright-val').textContent = p.br;
  pushLights();
}

function onBrightInput() {
  document.getElementById('bright-val').textContent = document.getElementById('light-bright').value;
}

let lightPushTimer = null;
function pushLights() {
  lightDirty = true;
  const on   = document.getElementById('light-power').checked;
  const hex  = document.getElementById('light-color').value;
  const br   = parseInt(document.getElementById('light-bright').value, 10);
  const { r, g, b } = hexToRgb(hex);
  lightState = { on, r, g, b, brightness: br };

  clearTimeout(lightPushTimer);
  lightPushTimer = setTimeout(async () => {
    const q = `/lights?on=${on?1:0}&r=${r}&g=${g}&b=${b}&br=${br}`;
    try { await fetch(q, { method:'POST' }); } catch(e) { console.warn('light push failed', e); }
    lightDirty = false;
  }, 150);
}

function syncLightUI(s) {
  if (lightDirty) return;   // don't stomp on the user mid-adjust
  document.getElementById('light-power').checked = s.on;
  document.getElementById('light-color').value   = rgbToHex(s.r, s.g, s.b);
  document.getElementById('light-bright').value  = s.brightness;
  document.getElementById('bright-val').textContent = s.brightness;
}

// ── Stress ratio helper ───────────────────────────────────────────────────────
function stressRatio(A590, A680) {
  if (A680 <= 0.1) return { value: null, display: 'n/a', reason: 'A680 too low' };
  const r = A590 / A680;
  return { value: r, display: r.toFixed(4), reason: null };
}

// ── Fetch ─────────────────────────────────────────────────────────────────────
async function fetchData() {
  try {
    const res  = await fetch('/data');
    const json = await res.json();
    update(json);
  } catch(e) { console.warn('Fetch failed', e); }
}

function update(json) {
  document.getElementById('count').textContent        = json.count ?? '0';
  document.getElementById('blank-status').textContent = json.hasBlank ? 'Yes ✓' : 'No';
  document.getElementById('device-time').textContent  = json.datetime ?? '—';
  allReadings = json.history ?? [];

  if (json.lights) { lightState = json.lights; syncLightUI(json.lights); }

  if (json.latest && json.latest.valid) {
    document.getElementById('last-a680').textContent = json.latest.A680.toFixed(4);
    if (selectedIdx === null) showReading(json.latest, 'Latest reading', '', allReadings.length - 1);
  }

  renderTable();
  drawChart();
}

// ── Plain English summary ─────────────────────────────────────────────────────
function buildSummary(r, idx) {
  const sr     = stressRatio(r.A590, r.A680);
  const stress = sr.value;
  const parts  = [];
  let   cls    = 'ok';

  if (r.A680 > 1.0 || r.A445 > 1.0) {
    parts.push('The culture is <strong>too dense to measure accurately</strong> — dilute 1:4 with fresh media and retake the blank before your next reading.');
    cls = 'alert';
  } else if (r.A680 > 0.6 || r.A445 > 0.8) {
    parts.push('The culture is <strong>on the dense side</strong> — readings are still usable but consider diluting 1:2 for your next measurement.');
    cls = 'warn';
  } else if (r.A680 < 0.05) {
    parts.push('Chlorophyll levels are <strong>very low</strong> — the culture may be at an early stage, or the sample needs to be more concentrated.');
    cls = 'low';
  } else {
    parts.push('The culture is at a <strong>good density</strong> for accurate measurement.');
  }

  if (r.A680 > 0.05 && r.A445 > 0.05) {
    parts.push('Both chlorophyll peaks (blue 445nm and deep red 680nm) are detectable, confirming this is a <strong>real algae signal</strong>.');
  } else if (r.A680 < 0 || r.A445 < 0) {
    parts.push('<strong>Negative values detected</strong> — the sample appears clearer than the blank at some wavelengths. Try retaking the blank with fresh media.');
    cls = 'warn';
  }

  if (stress !== null) {
    if (stress < 1.5) {
      parts.push(`Stress ratio is <strong>${stress.toFixed(2)}</strong> — chlorophyll is strongly dominant, the culture is <strong>not stressed</strong>.`);
    } else if (stress < 2.5) {
      parts.push(`Stress ratio is <strong>${stress.toFixed(2)}</strong> — within the normal healthy range.`);
    } else if (stress < 3.5) {
      parts.push(`Stress ratio is <strong>${stress.toFixed(2)}</strong> — mild stress detected. Check light intensity, temperature, and nutrient levels.`);
      if (cls === 'ok') cls = 'warn';
    } else {
      parts.push(`Stress ratio is <strong>${stress.toFixed(2)}</strong> — <strong>significant stress</strong>. Review light, temperature, and nutrient supply urgently.`);
      if (cls !== 'alert') cls = 'warn';
    }
  }

  if (idx > 0 && allReadings[idx - 1]) {
    const prev  = allReadings[idx - 1].A680;
    const delta = r.A680 - prev;
    if (delta > 0.01) {
      parts.push(`A680 is up <strong>+${delta.toFixed(4)}</strong> since the last reading — the culture is <strong>growing</strong> 📈`);
    } else if (delta < -0.01) {
      parts.push(`A680 is down <strong>${delta.toFixed(4)}</strong> since the last reading — chlorophyll has decreased. This may be normal dilution or a sign of decline.`);
    } else {
      parts.push(`A680 is <strong>stable</strong> compared to the last reading.`);
    }
  }

  return { html: parts.join(' '), cls };
}

// ── Show reading in panel ─────────────────────────────────────────────────────
function showReading(r, titleText, labelText, idx) {
  document.getElementById('panel-title').textContent   = titleText;
  document.getElementById('viewing-label').textContent = labelText;
  document.getElementById('panel-time').textContent    = r.datetime ?? '';

  if (r.A680 !== undefined) {
    const { html, cls } = buildSummary(r, idx);
    const box = document.getElementById('summary-box');
    box.innerHTML     = html;
    box.className     = 'summary-box ' + cls;
    box.style.display = 'block';
  }

  const statsDiv = document.getElementById('inline-stats');
  if (r.A680 > 0) {
    const sr = stressRatio(r.A590, r.A680);
    let stressLabel = '';
    if (sr.value === null) {
      stressLabel = `<span style="color:var(--muted)">n/a — A680 too low to calculate</span>`;
    } else if (sr.value < 1.5) {
      stressLabel = `<span class="trend-up">${sr.display} — not stressed ✓</span>`;
    } else if (sr.value < 2.5) {
      stressLabel = `<span style="color:var(--text)">${sr.display} — normal range</span>`;
    } else if (sr.value < 3.5) {
      stressLabel = `<span class="trend-flat">${sr.display} — mild stress</span>`;
    } else {
      stressLabel = `<span class="trend-down">${sr.display} — stressed ⚠</span>`;
    }
    document.getElementById('stress-ratio-val').innerHTML = stressLabel;

    let trendHtml = '<span style="color:var(--muted)">First reading</span>';
    if (idx > 0 && allReadings[idx - 1]) {
      const delta = r.A680 - allReadings[idx - 1].A680;
      const sign  = delta >= 0 ? '+' : '';
      if      (delta > 0.01)  trendHtml = `<span class="trend-up">↑ ${sign}${delta.toFixed(4)} — growing</span>`;
      else if (delta < -0.01) trendHtml = `<span class="trend-down">↓ ${delta.toFixed(4)} — declining</span>`;
      else                    trendHtml = `<span class="trend-flat">→ ${sign}${delta.toFixed(4)} — stable</span>`;
    }
    document.getElementById('trend-val').innerHTML = trendHtml;
    statsDiv.style.display = 'flex';
  } else {
    statsDiv.style.display = 'none';
  }

  const channels = [
    { label:'415nm Violet',  A: r.A415, raw: r.raw[0],  color:'#a78bfa' },
    { label:'445nm Chl a',   A: r.A445, raw: r.raw[1],  color:'#818cf8' },
    { label:'480nm Caro',    A: r.A480, raw: r.raw[2],  color:'#38bdf8' },
    { label:'515nm Green',   A: r.A515, raw: r.raw[3],  color:'#34d399' },
    { label:'555nm Y-Green', A: r.A555, raw: r.raw[6],  color:'#a3e635' },
    { label:'590nm Yellow',  A: r.A590, raw: r.raw[7],  color:'#fbbf24' },
    { label:'630nm Chl b',   A: r.A630, raw: r.raw[8],  color:'#fb7185' },
    { label:'680nm Chl a',   A: r.A680, raw: r.raw[9],  color:'#4ade80' },
    { label:'Clear',         A: null,   raw: r.raw[10], color:'#94a3b8' },
    { label:'NIR',           A: null,   raw: r.raw[11], color:'#6b7280' },
  ];

  const maxA = 1.5;
  document.getElementById('latest-channels').innerHTML = channels.map(ch => {
    const tt  = tooltips[ch.label] || {};
    const pct = ch.A !== null
      ? Math.min(Math.max(ch.A / maxA * 100, 0), 100)
      : Math.min(ch.raw / 65535 * 100, 100);
    return `
      <div class="channel">
        <span class="channel-label" title="${tt.title || ch.label}">${ch.label}</span>
        <div class="bar-track">
          <div class="bar-fill" style="width:${pct}%;background:${ch.color}"></div>
        </div>
        <span class="channel-value">${ch.A !== null ? ch.A.toFixed(4) : '—'}</span>
        <span class="channel-raw">${ch.raw}</span>
        <div class="tooltip">
          <strong>${tt.title || ch.label}</strong>
          ${tt.text || ''}
        </div>
      </div>`;
  }).join('');

  const badge = densityBadge(r.A680, r.A445);
  document.getElementById('density-badge').innerHTML =
    `<span class="density ${badge.cls}">${badge.label}</span>`;
}

// ── Table ─────────────────────────────────────────────────────────────────────
function renderTable() {
  const tbody = document.getElementById('history-body');
  if (!allReadings.length) {
    tbody.innerHTML = '<tr><td colspan="12" class="no-data">No readings yet</td></tr>';
    return;
  }
  tbody.innerHTML = [...allReadings].reverse().map((r, i) => {
    const realIdx = allReadings.length - 1 - i;
    const sel     = realIdx === selectedIdx ? 'selected' : '';
    const sr      = stressRatio(r.A590, r.A680);
    return `
      <tr class="${sel}" onclick="selectRow(${realIdx})">
        <td>${realIdx + 1}</td>
        <td>${r.datetime}</td>
        <td>${r.A415.toFixed(4)}</td>
        <td>${r.A445.toFixed(4)}</td>
        <td>${r.A480.toFixed(4)}</td>
        <td>${r.A515.toFixed(4)}</td>
        <td>${r.A555.toFixed(4)}</td>
        <td>${r.A590.toFixed(4)}</td>
        <td>${r.A630.toFixed(4)}</td>
        <td>${r.A680.toFixed(4)}</td>
        <td>${sr.display}</td>
        <td>${pillFor(r.A680, r.A445)}</td>
      </tr>`;
  }).join('');
}

function selectRow(idx) {
  if (selectedIdx === idx) {
    selectedIdx = null;
    renderTable();
    if (allReadings.length) showReading(allReadings[allReadings.length - 1], 'Latest reading', '', allReadings.length - 1);
    return;
  }
  selectedIdx = idx;
  renderTable();
  showReading(allReadings[idx], `Reading #${idx + 1}`, allReadings[idx].datetime, idx);
}

// ── Chart ─────────────────────────────────────────────────────────────────────
function drawChart() {
  const canvas = document.getElementById('chart');
  const ctx    = canvas.getContext('2d');
  const W = canvas.offsetWidth;
  const H = canvas.offsetHeight;
  canvas.width  = W;
  canvas.height = H;
  ctx.clearRect(0, 0, W, H);

  const data = allReadings.map((r, i) => ({ x: i + 1, y: r.A680 }));

  if (data.length < 2) {
    ctx.fillStyle = '#3d5c3d';
    ctx.font = '12px DM Mono, monospace';
    ctx.textAlign = 'center';
    ctx.fillText('Need 2+ readings to plot', W/2, H/2);
    return;
  }

  const pad   = { top:16, right:16, bottom:28, left:44 };
  const maxY  = Math.max(...data.map(d => d.y), 1.0);
  const xStep = (W - pad.left - pad.right) / (data.length - 1);
  const toX   = i => pad.left + i * xStep;
  const toY   = v => pad.top + (1 - v / maxY) * (H - pad.top - pad.bottom);

  ctx.strokeStyle = '#1e2e1e'; ctx.lineWidth = 1;
  [0.25, 0.5, 0.75, 1.0].forEach(v => {
    if (v > maxY) return;
    const y = toY(v);
    ctx.beginPath(); ctx.moveTo(pad.left, y); ctx.lineTo(W - pad.right, y); ctx.stroke();
    ctx.fillStyle = '#3d5c3d'; ctx.font = '9px DM Mono,monospace'; ctx.textAlign = 'right';
    ctx.fillText(v.toFixed(2), pad.left - 6, y + 3);
  });

  ctx.fillStyle = 'rgba(74,222,128,0.05)';
  ctx.fillRect(pad.left, toY(0.8), W - pad.left - pad.right, toY(0.1) - toY(0.8));
  ctx.fillStyle = '#2d5c2d'; ctx.font = '8px DM Mono,monospace'; ctx.textAlign = 'left';
  ctx.fillText('ideal range', pad.left + 4, toY(0.8) + 10);

  ctx.beginPath(); ctx.strokeStyle = '#4ade80'; ctx.lineWidth = 2; ctx.lineJoin = 'round';
  data.forEach((d, i) => i === 0 ? ctx.moveTo(toX(i), toY(d.y)) : ctx.lineTo(toX(i), toY(d.y)));
  ctx.stroke();

  data.forEach((d, i) => {
    const isSelected = i === selectedIdx;
    ctx.beginPath();
    ctx.arc(toX(i), toY(d.y), isSelected ? 5 : 3, 0, Math.PI * 2);
    ctx.fillStyle = isSelected ? '#ffffff' : '#4ade80';
    ctx.fill();
    if (isSelected) { ctx.strokeStyle = '#4ade80'; ctx.lineWidth = 2; ctx.stroke(); }
  });

  ctx.fillStyle = '#3d5c3d'; ctx.font = '9px DM Mono,monospace'; ctx.textAlign = 'center';
  data.forEach((d, i) => {
    if (data.length <= 10 || i % Math.ceil(data.length / 10) === 0)
      ctx.fillText(d.x, toX(i), H - 8);
  });
}

// ── CSV ───────────────────────────────────────────────────────────────────────
function downloadCSV() {
  if (!allReadings.length) { alert('No readings to download yet.'); return; }

  const headers = [
    'Reading #',
    'Date & Time',
    'A415 - 415nm Violet (Absorbance vs Blank)',
    'A445 - 445nm Blue - Chlorophyll a (Absorbance vs Blank)',
    'A480 - 480nm Cyan - Carotenoids (Absorbance vs Blank)',
    'A515 - 515nm Green (Absorbance vs Blank)',
    'A555 - 555nm Yellow-Green (Absorbance vs Blank)',
    'A590 - 590nm Yellow (Absorbance vs Blank)',
    'A630 - 630nm Red - Chlorophyll b (Absorbance vs Blank)',
    'A680 - 680nm Deep Red - Chlorophyll a (Absorbance vs Blank)',
    'Stress Ratio (A590 / A680)',
    'Raw Count - 415nm Violet',
    'Raw Count - 445nm Blue - Chlorophyll a',
    'Raw Count - 480nm Cyan - Carotenoids',
    'Raw Count - 515nm Green',
    'Raw Count - 555nm Yellow-Green',
    'Raw Count - 590nm Yellow',
    'Raw Count - 630nm Red - Chlorophyll b',
    'Raw Count - 680nm Deep Red - Chlorophyll a',
    'Raw Count - Clear (broadband)',
    'Raw Count - NIR (near infrared)',
    'Density Status'
  ].join(',');

  const rows = allReadings.map((r, i) => {
    const sr = stressRatio(r.A590, r.A680);
    return [
      i + 1,
      `"${r.datetime}"`,
      r.A415.toFixed(4), r.A445.toFixed(4), r.A480.toFixed(4), r.A515.toFixed(4),
      r.A555.toFixed(4), r.A590.toFixed(4), r.A630.toFixed(4), r.A680.toFixed(4),
      sr.display,
      r.raw[0], r.raw[1], r.raw[2], r.raw[3],
      r.raw[6], r.raw[7], r.raw[8], r.raw[9],
      r.raw[10], r.raw[11],
      `"${statusText(r.A680, r.A445)}"`
    ].join(',');
  }).join('\n');

  const blob = new Blob(['\uFEFF' + headers + '\n' + rows], { type: 'text/csv;charset=utf-8;' });
  const a    = document.createElement('a');
  a.href     = URL.createObjectURL(blob);
  a.download = `algaescope_${new Date().toISOString().slice(0,10)}.csv`;
  a.click();
}

// ── Guide overlay ─────────────────────────────────────────────────────────────
function openGuide()  { document.getElementById('guide-overlay').classList.add('open'); }
function closeGuide() { document.getElementById('guide-overlay').classList.remove('open'); }
function closeGuideOnBg(e) { if (e.target === document.getElementById('guide-overlay')) closeGuide(); }
document.addEventListener('keydown', e => { if (e.key === 'Escape') closeGuide(); });

// ── Helpers ───────────────────────────────────────────────────────────────────
function densityBadge(A680, A445) {
  if (A680 > 1.0 || A445 > 1.0) return { cls:'dilute', label:'!! Dilute sample !!' };
  if (A680 > 0.6 || A445 > 0.8) return { cls:'dense',  label:'Dense — consider diluting' };
  if (A680 < 0.05)               return { cls:'low',    label:'Very low density' };
  return { cls:'ok', label:'Density: OK ✓' };
}

function pillFor(A680, A445) {
  if (A680 > 1.0 || A445 > 1.0) return '<span class="pill dilute">Dilute</span>';
  if (A680 > 0.6 || A445 > 0.8) return '<span class="pill dense">Dense</span>';
  if (A680 < 0.05)               return '<span class="pill low">Low</span>';
  return '<span class="pill ok">OK</span>';
}

function statusText(A680, A445) {
  if (A680 > 1.0 || A445 > 1.0) return 'Dilute sample';
  if (A680 > 0.6 || A445 > 0.8) return 'Dense — consider diluting';
  if (A680 < 0.05)               return 'Very low density';
  return 'OK';
}

function clearHistory() {
  if (!confirm('Clear all reading history?')) return;
  fetch('/clear').then(() => { selectedIdx = null; fetchData(); });
}

fetchData();
setInterval(fetchData, 3000);
window.addEventListener('resize', drawChart);
</script>
</body>
</html>
)rawhtml";

// ── Web server handlers ───────────────────────────────────────────────────────
void handleRoot()  { server.send(200, "text/html", webpage); }
void handleClear() { historyCount = 0; server.send(200, "text/plain", "ok"); }

void handleBlank() {
  takeBlank();
  server.send(200, "text/plain", "ok");
}

void handleReading() {
  if (!hasBlank) { server.send(200, "text/plain", "noblank"); return; }
  takeReading();
  server.send(200, "text/plain", "ok");
}

void handleLights() {
  if (server.hasArg("on")) lightsOn = (server.arg("on").toInt() != 0);
  if (server.hasArg("r"))  lightR   = (uint8_t) constrain(server.arg("r").toInt(),  0, 255);
  if (server.hasArg("g"))  lightG   = (uint8_t) constrain(server.arg("g").toInt(),  0, 255);
  if (server.hasArg("b"))  lightB   = (uint8_t) constrain(server.arg("b").toInt(),  0, 255);
  if (server.hasArg("br")) lightBrightness = (uint8_t) constrain(server.arg("br").toInt(), 0, 255);
  applyLights();
  server.send(200, "text/plain", "ok");
}

void handleData() {
  char nowStr[24];
  getDatetime(nowStr, sizeof(nowStr));

  StaticJsonDocument<12288> doc;
  doc["count"]    = historyCount;
  doc["hasBlank"] = hasBlank;
  doc["uptime"]   = millis();
  doc["datetime"] = nowStr;

  JsonObject lights = doc.createNestedObject("lights");
  lights["on"]         = lightsOn;
  lights["r"]          = lightR;
  lights["g"]          = lightG;
  lights["b"]          = lightB;
  lights["brightness"] = lightBrightness;

  if (historyCount > 0) {
    Reading& r = history[historyCount - 1];
    JsonObject latest = doc.createNestedObject("latest");
    latest["valid"]    = r.valid;
    latest["datetime"] = r.datetime;
    latest["A415"] = r.A415; latest["A445"] = r.A445;
    latest["A480"] = r.A480; latest["A515"] = r.A515;
    latest["A555"] = r.A555; latest["A590"] = r.A590;
    latest["A630"] = r.A630; latest["A680"] = r.A680;
    JsonArray raw = latest.createNestedArray("raw");
    for (int i = 0; i < 12; i++) raw.add(r.raw[i]);
  } else {
    doc["latest"] = nullptr;
  }

  JsonArray hist = doc.createNestedArray("history");
  for (int i = 0; i < historyCount; i++) {
    JsonObject entry = hist.createNestedObject();
    entry["datetime"] = history[i].datetime;
    entry["A415"] = history[i].A415; entry["A445"] = history[i].A445;
    entry["A480"] = history[i].A480; entry["A515"] = history[i].A515;
    entry["A555"] = history[i].A555; entry["A590"] = history[i].A590;
    entry["A630"] = history[i].A630; entry["A680"] = history[i].A680;
    JsonArray raw = entry.createNestedArray("raw");
    for (int j = 0; j < 12; j++) raw.add(history[i].raw[j]);
  }

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);   // brief settle; do NOT wait for Serial — device must boot on power bank (no USB host)

  pinMode(BTN_BLANK,   INPUT_PULLUP);
  pinMode(BTN_READING, INPUT_PULLUP);

  pinMode(SPEC_LED_PIN, OUTPUT);
  digitalWrite(SPEC_LED_PIN, LOW);   // spectrometer LED off until a measurement

  ring.begin();
  ring.setBrightness(lightBrightness);
  ring.clear();
  ring.show();                       // rings off at boot

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("OLED not found");
    while (1) { delay(10); }
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("Starting...");
  display.display();

  if (!as7341.begin()) {
    Serial.println("AS7341 not found");
    display.println("AS7341 not found!");
    display.display();
    while (1) { delay(10); }
  }
  as7341.setATIME(200);
  as7341.setASTEP(999);
  as7341.setGain(AS7341_GAIN_256X);

  display.println("Connecting WiFi...");
  display.display();
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi failed");
    display.println("WiFi failed!");
    display.println("K1=blank K2=read");
    display.display();
  } else {
    Serial.println("\nWiFi: " + WiFi.localIP().toString());

    configTzTime(timezone, ntpServer);
    Serial.print("Syncing time");
    struct tm timeinfo;
    int tries = 0;
    while (!getLocalTime(&timeinfo) && tries < 20) {
      delay(500); Serial.print("."); tries++;
    }
    timeReady = getLocalTime(&timeinfo);
    Serial.println(timeReady ? "\nTime synced!" : "\nTime sync failed");

    server.on("/",        handleRoot);
    server.on("/data",    handleData);
    server.on("/clear",   handleClear);
    server.on("/blank",   HTTP_POST, handleBlank);
    server.on("/reading", HTTP_POST, handleReading);
    server.on("/lights",  HTTP_POST, handleLights);
    server.begin();

    display.clearDisplay();
    display.setCursor(0,0);
    display.println("AlgaeScope ready!");
    display.println("");
    display.println("Dashboard:");
    display.println(WiFi.localIP().toString());
    if (timeReady) {
      char buf[24]; getDatetime(buf, sizeof(buf));
      display.println(buf);
    }
    display.println("Control via web");
    display.display();

    Serial.println("Dashboard: http://" + WiFi.localIP().toString());
  }
  Serial.println("Commands: b = blank, r = reading");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'b') takeBlank();
    else if (cmd == 'r') takeReading();
  }

  if (digitalRead(BTN_BLANK) == LOW) {
    delay(50);
    if (digitalRead(BTN_BLANK) == LOW) {
      takeBlank();
      while (digitalRead(BTN_BLANK) == LOW) { delay(10); }
    }
  }

  if (digitalRead(BTN_READING) == LOW) {
    delay(50);
    if (digitalRead(BTN_READING) == LOW) {
      takeReading();
      while (digitalRead(BTN_READING) == LOW) { delay(10); }
    }
  }
}

// ── Averaged reading ──────────────────────────────────────────────────────────
bool getAveragedReading(uint16_t avg[12]) {
  uint32_t sums[12] = {0};
  for (int i = 0; i < NUM_AVERAGES; i++) {
    uint16_t readings[12];
    if (!as7341.readAllChannels(readings)) {
      Serial.println("Read error, retrying...");
      i--; delay(100); continue;
    }
    for (int ch = 0; ch < 12; ch++) sums[ch] += readings[ch];
    delay(20);
  }
  for (int ch = 0; ch < 12; ch++) avg[ch] = sums[ch] / NUM_AVERAGES;
  return true;
}

// ── Blank ─────────────────────────────────────────────────────────────────────
void takeBlank() {
  Serial.println("\nTaking blank...");
  display.clearDisplay(); display.setCursor(0,0);
  display.println("Taking blank..."); display.display();

  beginMeasurementLight();        // rings off, spectrometer LED on
  uint16_t avg[12];
  bool ok = getAveragedReading(avg);
  endMeasurementLight();          // spectrometer LED off, rings restored
  if (!ok) return;

  for (int ch = 0; ch < 12; ch++) blankRef[ch] = avg[ch];
  hasBlank = true;

  Serial.println("=== Blank Saved ===");
  printBar("415nm violet ", avg[0]);  printBar("445nm blue   ", avg[1]);
  printBar("480nm cyan   ", avg[2]);  printBar("515nm green  ", avg[3]);
  printBar("555nm y-green", avg[6]);  printBar("590nm yellow ", avg[7]);
  printBar("630nm red    ", avg[8]);  printBar("680nm deepRed", avg[9]);
  printBar("Clear        ", avg[10]); printBar("NIR          ", avg[11]);
  Serial.println("-----------------------------");
  Serial.println("Blank saved.");

  display.clearDisplay(); display.setCursor(0,0);
  display.println("Blank saved!");
  display.println("");
  display.print("Clear: "); display.println(avg[10]);
  display.print("680nm: "); display.println(avg[9]);
  display.println(""); display.println("Take reading on web");
  display.display();
}

// ── Reading ───────────────────────────────────────────────────────────────────
void takeReading() {
  Serial.println("\nTaking reading...");
  display.clearDisplay(); display.setCursor(0,0);
  display.println("Taking reading..."); display.display();

  beginMeasurementLight();        // rings off, spectrometer LED on
  uint16_t avg[12];
  bool ok = getAveragedReading(avg);
  endMeasurementLight();          // spectrometer LED off, rings restored
  if (!ok) return;

  Serial.println("=== Reading ===");
  printBar("415nm violet ", avg[0]);  printBar("445nm blue   ", avg[1]);
  printBar("480nm cyan   ", avg[2]);  printBar("515nm green  ", avg[3]);
  printBar("555nm y-green", avg[6]);  printBar("590nm yellow ", avg[7]);
  printBar("630nm red    ", avg[8]);  printBar("680nm deepRed", avg[9]);
  printBar("Clear        ", avg[10]); printBar("NIR          ", avg[11]);
  Serial.println("-----------------------------");

  if (!hasBlank) {
    Serial.println("No blank set — take a blank first");
    display.clearDisplay(); display.setCursor(0,0);
    display.println("No blank set!"); display.println("Take blank on web");
    display.display(); return;
  }

  float A415 = calcAbsorbance(avg[0],  blankRef[0]);
  float A445 = calcAbsorbance(avg[1],  blankRef[1]);
  float A480 = calcAbsorbance(avg[2],  blankRef[2]);
  float A515 = calcAbsorbance(avg[3],  blankRef[3]);
  float A555 = calcAbsorbance(avg[6],  blankRef[6]);
  float A590 = calcAbsorbance(avg[7],  blankRef[7]);
  float A630 = calcAbsorbance(avg[8],  blankRef[8]);
  float A680 = calcAbsorbance(avg[9],  blankRef[9]);

  Serial.println("=== Absorbance vs Blank ===");
  printAbsorbance("415nm violet  ", avg[0],  blankRef[0]);
  printAbsorbance("445nm (Chl a) ", avg[1],  blankRef[1]);
  printAbsorbance("480nm (Caro)  ", avg[2],  blankRef[2]);
  printAbsorbance("515nm green   ", avg[3],  blankRef[3]);
  printAbsorbance("555nm y-green ", avg[6],  blankRef[6]);
  printAbsorbance("590nm yellow  ", avg[7],  blankRef[7]);
  printAbsorbance("630nm (Chl b) ", avg[8],  blankRef[8]);
  printAbsorbance("680nm (Chl a) ", avg[9],  blankRef[9]);
  Serial.println("-----------------------------");

  float stress = (A680 > 0.1) ? A590 / A680 : 0;
  if (A680 > 0.1) {
    Serial.print("Stress ratio (A590/A680): "); Serial.println(stress, 4);
  } else {
    Serial.println("Stress ratio: n/a (A680 too low)");
  }

  if (A680>1.0||A445>1.0)      Serial.println("!! DILUTE SAMPLE !!");
  else if (A680>0.6||A445>0.8) Serial.println("Sample is dense — consider diluting");
  else if (A680<0.05)          Serial.println("Very low density");
  else                         Serial.println("Density: OK");
  Serial.println("-----------------------------");

  if (historyCount < MAX_READINGS) {
    Reading& r = history[historyCount++];
    getDatetime(r.datetime, sizeof(r.datetime));
    r.A415=A415; r.A445=A445; r.A480=A480; r.A515=A515;
    r.A555=A555; r.A590=A590; r.A630=A630; r.A680=A680;
    for (int ch = 0; ch < 12; ch++) r.raw[ch] = avg[ch];
    r.valid = true;
    Serial.print("Saved at: "); Serial.println(r.datetime);
  }

  display.clearDisplay(); display.setCursor(0,0);
  display.println("== Absorbance ==");
  display.print("680 Chl a: "); display.println(A680, 4);
  display.print("445 Chl a: "); display.println(A445, 4);
  display.print("630 Chl b: "); display.println(A630, 4);
  if (A680 > 0.1) {
    display.print("Stress:    "); display.println(stress, 2);
  } else {
    display.println("Stress:    n/a");
  }
  display.println("");
  if (A680>1.0||A445>1.0)      display.println("!! DILUTE SAMPLE !!");
  else if (A680>0.6||A445>0.8) { display.println("Dense — consider"); display.println("diluting"); }
  else if (A680<0.05)          display.println("Very low density");
  else                         display.println("Density: OK");
  display.display();
}

// ── Helpers ───────────────────────────────────────────────────────────────────
void printBar(const char* label, uint16_t value) {
  Serial.print(label); Serial.print(" | ");
  int bars = map(value, 0, 65535, 0, 40);
  for (int i=0; i<bars; i++) Serial.print("#");
  Serial.print(" "); Serial.println(value);
}

float calcAbsorbance(float sample, float blank) {
  if (blank<=0||sample<=0) return 0;
  return -log10(sample/blank);
}

void printAbsorbance(const char* label, float sample, float blank) {
  float A = calcAbsorbance(sample, blank);
  Serial.print(label);
  Serial.print(" A="); Serial.print(A, 4);
  Serial.print("  T="); Serial.print((sample/blank)*100.0, 1);
  Serial.println("%");
}
