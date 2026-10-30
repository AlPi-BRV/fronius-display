// Fronius-Display – Elecrow CrowPanel 5" (DIS07050H) und 7" (DIS08070H), jeweils V3.0
//
// Liest die Fronius Solar API direkt vom Wechselrichter (getestet: Symo GEN24):
//   - GetPowerFlowRealtimeData  – Energiefluss, Batterie, weitere Wechselrichter
//   - GetMeterRealtimeData      – Smart Meter: Phasen, Zaehlerstaende
// Startseite: Energiefluss (PV, Verbraucher, Netz, Batterie) und Tageswerte.
//   - Tipp auf einen Kreis: Details (PV, Verbraucher, Netz mit Phasen, Batterie)
//   - Zahnrad (oder Kopfzeile 2 s halten): Einstellungen
//
// Unterbau (Display, Touch, Einrichtung, Updates) aus dem Violet Pool-Display:
// https://github.com/AlPi-BRV/violet-display
//
// Projekt: https://github.com/AlPi-BRV/fronius-display  (MIT-Lizenz)

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <time.h>
#include "config.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "display_rgb.h"
#include "fonts.h"

#define FW_VERSION  "0.1.7"

// Board-Kennung: steckt in jedem Dateinamen des Releases. Die Update-Suche
// nimmt NUR Dateien mit der eigenen Kennung, damit ein 5" nie die 7"-Firmware
// laedt und umgekehrt.
#if defined(BOARD_CROWPANEL7)
#define BOARD_NAME   "7 Zoll"
#define OTA_PREFIX   "fronius-display-7zoll-"
#else
#define BOARD_NAME   "5 Zoll"
#define OTA_PREFIX   "fronius-display-5zoll-"
#endif
#define OTA_SUFFIX   "-ota.bin"
#define GITHUB_REPO "AlPi-BRV/fronius-display"
#define UPDATE_CHECK_EVERY_MS (24UL * 60UL * 60UL * 1000UL)
#define UPDATE_FIRST_CHECK_MS (60UL * 1000UL)
#define UPDATE_RETRY_MS       (15UL * 60UL * 1000UL)

// ---------------------------------------------------------------
// Voreinstellungen (koennen in config_local.h ueberschrieben werden)
// ---------------------------------------------------------------
// Wechselrichter antwortet nicht: schneller erneut fragen, Fehlerbild ab dem 2. Fehlversuch
#ifndef FAIL_RETRY_MS
#define FAIL_RETRY_MS  (15UL * 1000UL)
#endif
// WLAN weg: so lange warten, bevor das grosse Symbol erscheint (kurze Aussetzer ignorieren)
#ifndef WIFI_GRACE_MS
#define WIFI_GRACE_MS  (10UL * 1000UL)
#endif
// WLAN weg: so oft neu verbinden
#ifndef WIFI_RETRY_MS
#define WIFI_RETRY_MS  (30UL * 1000UL)
#endif
#ifndef DIM_AFTER_MS
#define DIM_AFTER_MS   (5UL * 60UL * 1000UL)
#endif
#ifndef BRIGHT_DEFAULT_PCT
#if defined(BOARD_CROWPANEL7)
#define BRIGHT_DEFAULT_PCT 70
#else
#define BRIGHT_DEFAULT_PCT 100
#endif
#endif
#ifndef CONTRAST_DEFAULT
#if defined(BOARD_CROWPANEL7)
#define CONTRAST_DEFAULT 2
#else
#define CONTRAST_DEFAULT 0
#endif
#endif
#ifndef BRIGHT_DIM
#define BRIGHT_DIM     30
#endif
#ifndef PAGE_TIMEOUT_MS
#define PAGE_TIMEOUT_MS (60UL * 1000UL)
#endif
#ifndef PV_MAX_DEFAULT_W
#define PV_MAX_DEFAULT_W 10000      // Vollausschlag des PV-Rings, bis kWp eingestellt ist
#endif
#ifndef FLOW_MIN_W
#define FLOW_MIN_W 30               // darunter gilt eine Leitung als "kein Fluss"
#endif
#ifndef ANIM_FRAME_MS
#define ANIM_FRAME_MS 40            // laufende Punkte: ca. 25 Bilder pro Sekunde
#endif
#ifndef DAY_SAVE_MS
#define DAY_SAVE_MS (5UL * 60UL * 1000UL)    // Tageswerte alle 5 min sichern
#endif

#define NVS_NS "fronidisp"

// Unsichtbares Zwischenbild – hier wird gezeichnet, danach ins Panel kopiert.
// "base" ist eine Kopie der fertigen Startseite ohne die laufenden Punkte:
// fuer jedes Animationsbild werden daraus nur die kleinen Bereiche um die
// Leitungen wiederhergestellt, dann die Punkte neu gezeichnet.
static LGFX_Sprite canvas;
static LGFX_Sprite base;
static bool baseValid = false;

static void pushAll() { disp::copyRect((const uint16_t*)canvas.getBuffer(), 0, 0, disp::W, disp::H); }

// ---------------------------------------------------------------
// Schriften (VLW aus fonts.h, nur benoetigte Zeichen)
// ---------------------------------------------------------------
struct MemFont {
  lgfx::PointerWrapper data;
  lgfx::VLWfont font;
  void begin(const uint8_t* p, size_t n) { data.set(p, n); font.loadFont(&data); }
};
static MemFont fXS, fS, fT, fB, fM, fL, fXL;

static void useFont(MemFont& f) {
  canvas.setFont(&f.font);
  canvas.setTextSize(1);
}

// Groesste Schrift aus der Liste, mit der der Text in maxW passt
static void fitFont(const String& s, int maxW, MemFont* a, MemFont* b = nullptr,
                    MemFont* c = nullptr, MemFont* d = nullptr) {
  MemFont* list[] = { a, b, c, d };
  MemFont* last = a;
  for (MemFont* f : list) {
    if (!f) break;
    last = f;
    useFont(*f);
    if (canvas.textWidth(s) <= maxW) return;
  }
  useFont(*last);
}

// ---------------------------------------------------------------
// Farben (RGB565) – wie beim Pool-Display, dazu Sonne und Leitungen
// ---------------------------------------------------------------
static const uint16_t C_BG       = 0x08A4;
static const uint16_t C_TEXT     = 0xF7BF;
static const uint16_t C_DIM      = 0xA554;
static const uint16_t C_TILE     = 0x1945;
static const uint16_t C_ACC_BLUE = 0x8D9D;
static const uint16_t C_ACC_GRN  = 0x9E2B;
static const uint16_t C_ACC_ORG  = 0xF4CF;
static const uint16_t C_ACC_RED  = 0xF9A6;
static const uint16_t C_BTN      = 0x2A0B;
static const uint16_t C_BTN_SEL  = 0x2D0A;
static const uint16_t C_BTN_RED  = 0x7104;
static const uint16_t C_OFF      = 0x4208;   // ausgegraut
static const uint16_t C_SEL_TXT  = 0x0861;   // fast schwarz: Schrift auf gruenen (gewaehlten) Tasten
static const uint16_t C_SUN      = 0xF625;   // PV
static const uint16_t C_LINE     = 0x320A;   // Leitung ohne Fluss

// Farbe zwischen a (t = 0) und b (t = 1)
static uint16_t mix565(uint16_t a, uint16_t b, float t) {
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (int)lroundf((br - ar) * t);
  int g = ag + (int)lroundf((bg - ag) * t);
  int bl = ab + (int)lroundf((bb - ab) * t);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// ---------------------------------------------------------------
// Einstellungen (Flash)
// ---------------------------------------------------------------
static Preferences prefs;
static String   froniusHost = FRONIUS_HOST;
static String   wifiSsid   = WIFI_SSID;
static String   wifiPass   = WIFI_PASS;
static bool     ipFixed    = false;
static String   ipAddr, ipMask = "255.255.255.0", ipGw, ipDns;
static uint32_t pollMs     = POLL_INTERVAL_MS;
static uint32_t pvMaxW     = 0;       // installierte PV-Leistung in W (0 = nicht eingestellt)
static uint32_t battWh     = 0;       // Batteriekapazitaet in Wh (0 = unbekannt)
static bool     rot180     = false;   // Ausrichtung: um 180° gedreht
static bool     animOn     = false;   // Energiefluss: false = Pfeile, true = laufende Punkte
static uint8_t  brightPct  = BRIGHT_DEFAULT_PCT;
static uint8_t  contrastLvl = CONTRAST_DEFAULT;
static String   fwUpdatedFrom;

static const uint32_t POLL_CHOICES[] = { 2000, 5000, 10000, 30000, 60000 };
static const char* const POLL_LABEL[] = { "2 s", "5 s", "10 s", "30 s", "1 min" };
static const uint8_t BRIGHT_CHOICES[] = { 10, 20, 40, 70, 100 };
static const float   CONTRAST_GAMMA[] = { 1.0f, 1.35f, 1.7f, 2.1f };
static const char* const CONTRAST_LABEL[] = { "Normal", "Mehr", "Stark", "Max" };

static uint8_t brightDuty(uint8_t pct) { return (uint8_t)((uint16_t)pct * 255 / 100); }
static uint8_t dimDuty() { uint8_t d = brightDuty(brightPct); return d < BRIGHT_DIM ? d : BRIGHT_DIM; }

static void loadSettings() {
  prefs.begin(NVS_NS, true);
  froniusHost = prefs.getString("host", froniusHost);
  wifiSsid   = prefs.getString("ssid", wifiSsid);
  wifiPass   = prefs.getString("wpass", wifiPass);
  ipFixed    = prefs.getBool("ipfix", false);
  ipAddr     = prefs.getString("ip", "");
  ipMask     = prefs.getString("mask", ipMask);
  ipGw       = prefs.getString("gw", "");
  ipDns      = prefs.getString("dns", "");
  pollMs     = prefs.getUInt("poll", pollMs);
  pvMaxW     = prefs.getUInt("pvmax", 0);
  battWh     = prefs.getUInt("battwh", 0);
  rot180     = prefs.getBool("rot180", false);
  animOn     = prefs.getBool("anim2", false);
  brightPct  = prefs.getUChar("bright", BRIGHT_DEFAULT_PCT);
  if (brightPct < 10 || brightPct > 100) brightPct = BRIGHT_DEFAULT_PCT;
  contrastLvl = prefs.getUChar("contrast", CONTRAST_DEFAULT);
  if (contrastLvl > 3) contrastLvl = CONTRAST_DEFAULT;
  prefs.end();

  prefs.begin(NVS_NS, false);
  String last = prefs.getString("fwver", "");
  if (last != FW_VERSION) {
    prefs.putString("fwver", FW_VERSION);
    if (last.length()) fwUpdatedFrom = last;
  }
  prefs.end();
  if (pollMs < 2000) pollMs = 2000;
}

// ---------------------------------------------------------------
// Daten vom Wechselrichter
// ---------------------------------------------------------------
// Vorzeichen wie in der Solar API:
//   pPv   > 0 Erzeugung (nur Fronius-Wechselrichter)
//   pAkku > 0 Batterie entlaedt, < 0 laedt
//   pGrid > 0 Netzbezug,         < 0 Einspeisung
//   pLoad < 0 Verbrauch (die API liefert den Verbrauch negativ)
struct SecMeter {
  String label;
  float  p = NAN;
  int    loc = -1;
  bool   gen = false;     // Kategorie Wechselrichter (z. B. Hoymiles)
};

struct FlowData {
  bool   valid = false;
  float  pPv = NAN, pAkku = NAN, pGrid = NAN, pLoad = NAN;
  float  soc = NAN;
  bool   hasBatt = false;
  String battMode;
  bool   battStandby = false;
  double eInvTotal = NAN;          // Zaehler des Fronius-Wechselrichters in Wh
  int    nSec = 0;
  SecMeter sec[4];
  int    nOhm = 0;                 // Ohmpilot (Heizstab)
  float  ohmP[2] = { NAN, NAN };
  float  ohmTemp[2] = { NAN, NAN };
};

struct MeterData {
  bool   valid = false;            // Zaehler am Netzanschluss gefunden
  float  v[3] = { NAN, NAN, NAN }, a[3] = { NAN, NAN, NAN }, p[3] = { NAN, NAN, NAN };
  float  freq = NAN;
  double eImp = NAN, eExp = NAN;   // Netzbezug / Einspeisung gesamt in Wh
  bool   genValid = false;
  double eGen = NAN;               // Erzeugung weiterer Wechselrichter gesamt in Wh
  String model;
};

// Batteriespeicher (GetStorageRealtimeData) – aendert sich langsam, darum selten abfragen
struct StorageData {
  bool   valid = false;
  float  capWh = NAN;              // nutzbare Kapazitaet laut Speicher (Capacity_Maximum)
  float  designWh = NAN;           // Nennkapazitaet (DesignedCapacity)
  float  tempC = NAN;              // Zelltemperatur
  float  voltDc = NAN;
  String model;                    // z. B. "BYD Battery-Box Premium HV"
};

static FlowData    flow;
static MeterData   meter;
static StorageData storage;
static uint32_t    nextStorageFetch = 0;
static uint32_t  lastPoll = 0;
static volatile int failCount = 0;
static String    lastOkTime;           // Uhrzeit der letzten erfolgreichen Abfrage
static int       lastHttp = 0;     // letzter HTTP-Code (fuer Meldungen)

// Batteriekapazitaet in Wh: vom Speicher gemeldet, sonst aus den Einstellungen (0 = unbekannt)
static float battCapWh() {
  if (storage.valid && !isnan(storage.capWh) && storage.capWh > 100) return storage.capWh;
  return (float)battWh;
}
static bool battCapAuto() { return storage.valid && !isnan(storage.capWh) && storage.capWh > 100; }

// Abgeleitete Momentanwerte
static float pvGenW() {            // weitere Wechselrichter (Smart Meter, Kategorie WR)
  float s = 0;
  for (int i = 0; i < flow.nSec; i++)
    if (flow.sec[i].gen && !isnan(flow.sec[i].p)) s += fabsf(flow.sec[i].p);
  return s;
}
static float pvTotalW() {
  if (!flow.valid) return NAN;
  return (isnan(flow.pPv) ? 0 : flow.pPv) + pvGenW();
}
static float loadW() {
  if (!flow.valid || isnan(flow.pLoad)) return NAN;
  return fabsf(flow.pLoad);
}
// Geprueft am GEN24 (28.10.2026, Sonne): P_PV enthaelt NUR den Fronius (DC-Seite),
// ein weiterer Wechselrichter am Smart Meter (Kategorie WR, z. B. Hoymiles) muss
// dazugerechnet werden. P_Load ist der echte Hausverbrauch, der weitere
// Wechselrichter steckt dort schon richtig drin (Bilanz geht bis auf die
// Wandlungsverluste auf; Fronius rechnet den Eigenverbrauch genauso).

// ---------------------------------------------------------------
// Tageswerte – der GEN24 liefert keine E_Day-Werte, darum selbst rechnen:
//   - Netzbezug, Einspeisung, weitere Wechselrichter: Zaehlerstaende der
//     Smart Meter, Stand um Mitternacht gemerkt (genau)
//   - PV des Fronius, Verbrauch, Batterie: Leistung ueber die Zeit
//     aufsummiert, solange das Display laeuft (Naeherung)
// Alles wird im Flash gesichert und uebersteht Neustarts und Updates.
// ---------------------------------------------------------------
struct DayStore {
  uint32_t day = 0;               // JJJJMMTT
  uint16_t startMin = 0;          // Minute des Tages, ab der gezaehlt wird (0 = ganzer Tag)
  uint16_t ver = 1;
  double   eImp = NAN, eExp = NAN, eGen = NAN, eInv = NAN;   // Zaehlerstaende am Tagesbeginn
  float    pvWh = 0, loadWh = 0, battInWh = 0, battOutWh = 0;
};
static DayStore dayS;
static uint32_t lastIntegMs = 0;
static uint32_t lastDaySave = 0;

struct Today {
  bool  valid = false;
  float pv = NAN, pvFr = NAN, pvGen = NAN, load = NAN, imp = NAN, exp = NAN;
  float battIn = NAN, battOut = NAN, invCounter = NAN;
  float autarkie = NAN, eigen = NAN;
};
static Today today;

static bool localTime(struct tm& t) { return getLocalTime(&t, 0) && t.tm_year > 120; }

static void daySave() {
  prefs.begin(NVS_NS, false);
  prefs.putBytes("day", &dayS, sizeof(dayS));
  prefs.end();
  lastDaySave = millis();
}

static void dayLoad() {
  prefs.begin(NVS_NS, true);
  DayStore d;
  if (prefs.getBytes("day", &d, sizeof(d)) == sizeof(d) && d.ver == 1) dayS = d;
  prefs.end();
}

static void computeToday() {
  Today t;
  if (dayS.day == 0) { today = t; return; }
  t.pvFr   = dayS.pvWh;
  t.load   = dayS.loadWh;
  t.battIn = dayS.battInWh;
  t.battOut = dayS.battOutWh;
  if (meter.valid && !isnan(dayS.eImp)) {
    t.imp = (float)(meter.eImp - dayS.eImp);
    t.exp = (float)(meter.eExp - dayS.eExp);
  }
  t.pvGen = (meter.genValid && !isnan(dayS.eGen)) ? (float)(meter.eGen - dayS.eGen) : 0;
  if (!isnan(flow.eInvTotal) && !isnan(dayS.eInv)) t.invCounter = (float)(flow.eInvTotal - dayS.eInv);
  t.pv = t.pvFr + t.pvGen;
  // Verbrauch aus Zaehlerstaenden (stimmt auch, wenn das Display zwischendurch aus war):
  //   Haus = Wechselrichter (AC) + weitere Wechselrichter + Netzbezug - Einspeisung
  if (!isnan(t.invCounter) && !isnan(t.imp)) {
    float l = t.invCounter + t.pvGen + t.imp - t.exp;
    if (l >= 0) t.load = l;
  }
  if (t.load > 1 && !isnan(t.imp)) t.autarkie = constrain((1.0f - t.imp / t.load) * 100.0f, 0.0f, 100.0f);
  if (t.pv > 1 && !isnan(t.exp)) t.eigen = constrain((1.0f - t.exp / t.pv) * 100.0f, 0.0f, 100.0f);
  t.valid = true;
  today = t;
}

// Nach jedem erfolgreichen Abruf: Tageswechsel pruefen, Leistungen aufsummieren
static void dayUpdate() {
  struct tm tm;
  if (!localTime(tm)) return;                       // ohne Uhrzeit keine Tageswerte
  uint32_t d = (tm.tm_year + 1900) * 10000UL + (tm.tm_mon + 1) * 100UL + tm.tm_mday;
  uint32_t now = millis();
  if (dayS.day != d) {
    bool rolled = dayS.day != 0 && lastIntegMs != 0;  // Mitternacht im laufenden Betrieb
    dayS = DayStore();
    dayS.day = d;
    dayS.startMin = rolled ? 0 : (uint16_t)(tm.tm_hour * 60 + tm.tm_min);
    Serial.printf("Tageswerte: neuer Tag %lu (ab %02u:%02u)\n", (unsigned long)d, dayS.startMin / 60,
                  dayS.startMin % 60);
    daySave();
  }
  // Zaehlerstaende des Tagesbeginns einmal setzen, sobald bekannt
  bool changed = false;
  if (isnan(dayS.eImp) && meter.valid) { dayS.eImp = meter.eImp; dayS.eExp = meter.eExp; changed = true; }
  if (isnan(dayS.eGen) && meter.genValid) { dayS.eGen = meter.eGen; changed = true; }
  if (isnan(dayS.eInv) && !isnan(flow.eInvTotal)) { dayS.eInv = flow.eInvTotal; changed = true; }
  if (changed) daySave();

  // Leistung * Zeit; groessere Luecken (Display aus, Einstellungen offen) zaehlen nicht
  if (lastIntegMs) {
    float dtH = (now - lastIntegMs) / 3600000.0f;
    if (dtH > 0 && dtH < 120.0f / 3600.0f) {
      if (!isnan(flow.pPv) && flow.pPv > 0) dayS.pvWh += flow.pPv * dtH;
      float l = loadW();
      if (!isnan(l)) dayS.loadWh += l * dtH;
      if (!isnan(flow.pAkku)) {
        if (flow.pAkku > 0) dayS.battOutWh += flow.pAkku * dtH;
        else dayS.battInWh += -flow.pAkku * dtH;
      }
    }
  }
  lastIntegMs = now;
  if (now - lastDaySave > DAY_SAVE_MS) daySave();
  computeToday();
}

// ---------------------------------------------------------------
// Bildschirme und Bedienung
// ---------------------------------------------------------------
enum Screen { SCR_FLOW, SCR_DETAIL, SCR_SETTINGS };
static Screen   screen = SCR_FLOW;
enum Node { N_PV = 0, N_LOAD = 1, N_GRID = 2, N_BATT = 3 };
static int      detailSel = N_PV;
static uint32_t lastTouchMs = 0;
static bool     dimmed = false;
static bool     needRedraw = true;

enum Action { A_NONE, A_FACTORY, A_OTA };
static Action   confirmAct = A_NONE;
static String   confirmL1, confirmL2;

static String   toastText;
static uint16_t toastColor = C_TEXT;
static uint32_t toastUntil = 0;

static String   updateVersion;
static String   updateUrl;
static uint32_t lastUpdateCheck = 0;
static uint32_t updateWaitMs    = UPDATE_FIRST_CHECK_MS;

static void toast(const String& t, uint16_t col, uint32_t ms = 4000) {
  toastText = t;
  toastColor = col;
  toastUntil = millis() + ms;
  needRedraw = true;
}

static void askConfirm(Action a, const String& l1, const String& l2) {
  confirmAct = a;
  confirmL1 = l1;
  confirmL2 = l2;
  needRedraw = true;
}

// ---------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------
// JSON-Puffer im internen RAM statt im PSRAM: das PSRAM liefert gleichzeitig
// das Bild ans Display – weniger Zugriffe dort = weniger Zucken beim Abruf.
struct InternalAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override {
    return heap_caps_realloc(p, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
};
static InternalAllocator intAlloc;

static double jd(JsonVariantConst v) {
  if (v.isNull()) return NAN;
  if (v.is<const char*>()) return atof(v.as<const char*>());
  return v.as<double>();
}

// Text aus JSON; fehlt das Feld, leerer Text (nicht "null")
static String js(JsonVariantConst v) {
  const char* p = v.as<const char*>();
  return p ? String(p) : String();
}

static String fmt(float v, int nachkomma) {
  if (isnan(v)) return "--";
  char buf[16];
  snprintf(buf, sizeof(buf), "%.*f", nachkomma, v);
  for (char* p = buf; *p; ++p) if (*p == '.') *p = ',';
  return String(buf);
}

// Leistung: unter 1 kW in W, darueber kW mit einer Nachkommastelle
static void powerParts(float w, String& num, String& unit) {
  if (isnan(w)) { num = "--"; unit = ""; return; }
  w = fabsf(w);
  if (w < 999.5f) { num = String((int)lroundf(w)); unit = "W"; }
  else { num = fmt(w / 1000.0f, 1); unit = "kW"; }
}
static String powerText(float w) {
  String n, u;
  powerParts(w, n, u);
  return u.length() ? n + " " + u : n;
}

// Energie in kWh (Wh als Eingabe)
static void energyParts(float wh, String& num, String& unit) {
  if (isnan(wh)) { num = "--"; unit = ""; return; }
  float kwh = fabsf(wh) / 1000.0f;
  if (kwh >= 10000.0f) { num = fmt(kwh / 1000.0f, 1); unit = "MWh"; }
  else if (kwh >= 1000.0f) { num = fmt(kwh, 0); unit = "kWh"; }
  else { num = fmt(kwh, 1); unit = "kWh"; }
}

static String dauerText(float h) {
  if (isnan(h) || h <= 0 || h > 99) return "";
  int m = (int)lroundf(h * 60);
  if (m < 60) return String(m) + " min";
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d h", m / 60, m % 60);
  return String(buf);
}

static int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return -1;
  int r = WiFi.RSSI();
  return r > -60 ? 3 : (r > -70 ? 2 : (r > -80 ? 1 : 0));
}

static String uhrzeit() {
  struct tm t;
  if (!localTime(t)) return "";
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
}

// Batteriebetrieb der API kurz auf Deutsch
static String battModeText(const String& m) {
  if (m.length() == 0) return "--";
  if (m == "normal") return "Normal";
  if (m == "service") return "Service";
  if (m == "charge boost") return "Zwangsladung";
  if (m == "nearly depleted") return "Fast leer";
  if (m == "suspended") return "Pausiert";
  if (m == "calibrate") return "Kalibrierung";
  if (m == "grid support") return "Netzstützung";
  if (m == "deplete recovery") return "Tiefentladeschutz";
  if (m == "non operable (voltage)") return "Spannung außerhalb";
  if (m == "non operable (temperature)") return "Temperatur außerhalb";
  if (m == "preheating") return "Vorheizen";
  if (m == "startup") return "Start";
  if (m == "stopped (temperature)") return "Gestoppt (Temperatur)";
  if (m == "battery full") return "Voll";
  if (m == "disabled") return "Aus";
  return m;
}

// ---------------------------------------------------------------
// Fronius: lesen
// ---------------------------------------------------------------
static bool fetchJson(const String& host, const char* path, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED || host.length() == 0) { lastHttp = -1; return false; }
  HTTPClient http;
  http.useHTTP10(true);   // kein "chunked" – Antwort direkt aus dem Datenstrom lesen
  http.setTimeout(4000);
  http.setConnectTimeout(3000);
  if (!http.begin("http://" + host + path)) { lastHttp = -1; return false; }
  int code = http.GET();
  lastHttp = code;
  if (code != 200) {
    Serial.printf("Fronius %s: HTTP %d\n", path, code);
    http.end();
    return false;
  }
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("Fronius %s: JSON-Fehler %s\n", path, err.c_str());
    lastHttp = -2;
    return false;
  }
  return true;
}

static bool readPowerFlow(const String& host, FlowData& n) {
  JsonDocument doc(&intAlloc);
  if (!fetchJson(host, "/solar_api/v1/GetPowerFlowRealtimeData.fcgi", doc)) return false;
  JsonObjectConst d = doc["Body"]["Data"].as<JsonObjectConst>();
  if (d.isNull()) return false;
  JsonObjectConst site = d["Site"].as<JsonObjectConst>();
  n.pPv   = jd(site["P_PV"]);
  n.pAkku = jd(site["P_Akku"]);
  n.pGrid = jd(site["P_Grid"]);
  n.pLoad = jd(site["P_Load"]);
  n.battStandby = site["BatteryStandby"].as<bool>();
  n.eInvTotal = NAN;
  for (JsonPairConst kv : d["Inverters"].as<JsonObjectConst>()) {
    JsonObjectConst inv = kv.value().as<JsonObjectConst>();
    double soc = jd(inv["SOC"]);
    if (!isnan(soc)) { n.soc = soc; n.hasBatt = true; }
    n.battMode = js(inv["Battery_Mode"]);
    double e = jd(inv["E_Total"]);
    if (!isnan(e)) n.eInvTotal = isnan(n.eInvTotal) ? e : n.eInvTotal + e;
  }
  if (!isnan(n.pAkku)) n.hasBatt = true;
  n.nSec = 0;
  for (JsonPairConst kv : d["SecondaryMeters"].as<JsonObjectConst>()) {
    if (n.nSec >= 4) break;
    JsonObjectConst m = kv.value().as<JsonObjectConst>();
    SecMeter& s = n.sec[n.nSec++];
    s.label = js(m["Label"]);
    s.p = jd(m["P"]);
    double loc = jd(m["MLoc"]);
    s.loc = isnan(loc) ? -1 : (int)lround(loc);
    String cat = js(m["Category"]);
    s.gen = cat == "METER_CAT_WR";
    if (!s.label.length()) s.label = s.gen ? "Wechselrichter" : "Zähler";
  }
  n.nOhm = 0;
  for (JsonPairConst kv : d["Smartloads"]["Ohmpilots"].as<JsonObjectConst>()) {
    if (n.nOhm >= 2) break;
    JsonObjectConst o = kv.value().as<JsonObjectConst>();
    n.ohmP[n.nOhm] = jd(o["P_AC_Total"]);
    n.ohmTemp[n.nOhm] = jd(o["Temperature"]);
    n.nOhm++;
  }
  n.valid = true;
  return true;
}

static bool isGenLoc(const FlowData& f, int loc) {
  for (int i = 0; i < f.nSec; i++) if (f.sec[i].gen && f.sec[i].loc == loc) return true;
  return false;
}

static bool readMeters(const String& host, const FlowData& f, MeterData& n) {
  JsonDocument doc(&intAlloc);
  if (!fetchJson(host, "/solar_api/v1/GetMeterRealtimeData.cgi?Scope=System", doc)) return false;
  JsonObjectConst d = doc["Body"]["Data"].as<JsonObjectConst>();
  if (d.isNull()) return false;
  double eGen = 0;
  bool anyGen = false;
  for (JsonPairConst kv : d) {
    JsonObjectConst m = kv.value().as<JsonObjectConst>();
    double locD = jd(m["Meter_Location_Current"]);
    int loc = isnan(locD) ? -1 : (int)lround(locD);
    if (loc == 0 && !n.valid) {                 // Einspeisepunkt (Netz)
      static const char* const V[3] = { "Voltage_AC_Phase_1", "Voltage_AC_Phase_2", "Voltage_AC_Phase_3" };
      static const char* const A[3] = { "Current_AC_Phase_1", "Current_AC_Phase_2", "Current_AC_Phase_3" };
      static const char* const P[3] = { "PowerReal_P_Phase_1", "PowerReal_P_Phase_2", "PowerReal_P_Phase_3" };
      for (int i = 0; i < 3; i++) {
        n.v[i] = jd(m[V[i]]);
        n.a[i] = jd(m[A[i]]);
        n.p[i] = jd(m[P[i]]);
      }
      n.freq = jd(m["Frequency_Phase_Average"]);
      n.eImp = jd(m["EnergyReal_WAC_Sum_Consumed"]);
      n.eExp = jd(m["EnergyReal_WAC_Sum_Produced"]);
      n.model = js(m["Details"]["Model"]);
      n.valid = !isnan(n.eImp) && !isnan(n.eExp);
    } else if (isGenLoc(f, loc)) {             // weiterer Wechselrichter (z. B. Hoymiles)
      double e = jd(m["EnergyReal_WAC_Sum_Produced"]);
      if (!isnan(e)) { eGen += e; anyGen = true; }
    }
  }
  n.genValid = anyGen;
  n.eGen = anyGen ? eGen : NAN;
  return true;
}

static bool readStorage(const String& host, StorageData& n) {
  JsonDocument doc(&intAlloc);
  if (!fetchJson(host, "/solar_api/v1/GetStorageRealtimeData.cgi?Scope=System", doc)) return false;
  // erster Speicher; mehrere Module (z. B. 2 x BYD) meldet der Controller zusammen
  for (JsonPairConst kv : doc["Body"]["Data"].as<JsonObjectConst>()) {
    JsonObjectConst c = kv.value()["Controller"].as<JsonObjectConst>();
    if (c.isNull()) continue;
    n.capWh    = jd(c["Capacity_Maximum"]);
    n.designWh = jd(c["DesignedCapacity"]);
    n.tempC    = jd(c["Temperature_Cell"]);
    n.voltDc   = jd(c["Voltage_DC"]);
    n.model    = js(c["Details"]["Model"]);
    n.model.trim();
    if (!n.model.length()) n.model = js(c["Details"]["Manufacturer"]);
    n.valid = true;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------
// Abfrage im Hintergrund (zweiter Prozessorkern)
// Die Anfragen an den Wechselrichter dauern je nach Geraet bis zu einigen
// Sekunden. Liefen sie in loop(), waere das Display so lange taub fuer
// Beruehrungen. Darum holt ein eigener Task die Daten und legt sie in
// "pending" ab; loop() uebernimmt sie, sobald sie da sind.
//   netMx  – immer nur ein Netzzugriff gleichzeitig (Abfrage, Test, Update, WLAN-Suche)
//   dataMx – Uebergabe der Daten und der Wechselrichter-Adresse
// ---------------------------------------------------------------
struct PollResult {
  bool ok = false;
  FlowData f;
  bool meterOk = false;
  MeterData m;
  bool storOk = false;
  StorageData st;
};
static SemaphoreHandle_t netMx = nullptr;
static SemaphoreHandle_t dataMx = nullptr;
static PollResult        pending;
static volatile bool     pendingNew = false;
static volatile bool     pollPaused = false;   // in den Einstellungen keine Abfrage
static volatile bool     pollNow = false;      // sofort abfragen (nach dem Start, nach Speichern)

struct NetLock {
  NetLock() { if (netMx) xSemaphoreTake(netMx, portMAX_DELAY); }
  ~NetLock() { if (netMx) xSemaphoreGive(netMx); }
};

static String hostCopy() {
  if (dataMx) xSemaphoreTake(dataMx, portMAX_DELAY);
  String h = froniusHost;
  if (dataMx) xSemaphoreGive(dataMx);
  return h;
}

// Alle Abfragen nacheinander; der Speicher nur jede Minute
static void fetchAll(const String& host, PollResult& r) {
  NetLock lock;
  r.ok = readPowerFlow(host, r.f);
  if (!r.ok) return;
  r.meterOk = readMeters(host, r.f, r.m);       // ohne Smart Meter geht der Energiefluss trotzdem
  if (r.f.hasBatt && millis() >= nextStorageFetch) {
    r.storOk = readStorage(host, r.st);
    nextStorageFetch = millis() + (r.storOk ? 60000UL : 10UL * 60000UL);
  }
}

// Ergebnis uebernehmen (nur aus loop() bzw. dem Haupt-Task)
static void applyPoll(const PollResult& r) {
  if (r.ok) {
    flow = r.f;
    if (r.meterOk) meter = r.m;
    if (r.storOk) storage = r.st;
    failCount = 0;
    lastOkTime = uhrzeit();
    dayUpdate();
  } else {
    failCount++;
  }
  lastPoll = millis();
  needRedraw = true;
}

static void pollTask(void*) {
  uint32_t last = 0;
  for (;;) {
    uint32_t iv = (failCount > 0 && pollMs > FAIL_RETRY_MS) ? FAIL_RETRY_MS : pollMs;
    bool due = pollNow || last == 0 || millis() - last >= iv;
    String host = hostCopy();
    if (!pollPaused && due && host.length() && WiFi.status() == WL_CONNECTED) {
      pollNow = false;
      last = millis();
      PollResult r;
      fetchAll(host, r);
      xSemaphoreTake(dataMx, portMAX_DELAY);
      pending = r;
      pendingNew = true;
      xSemaphoreGive(dataMx);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Sofort abfragen und warten (Speichern der Einstellungen); true = erreichbar
static bool pollFronius() {
  PollResult r;
  fetchAll(hostCopy(), r);
  applyPoll(r);
  return r.ok;
}

// ---------------------------------------------------------------
// Update: neueste Version auf GitHub abfragen und ggf. installieren
// ---------------------------------------------------------------
static long versionNum(const String& v) {
  int a = 0, b = 0, c = 0;
  const char* p = v.c_str();
  if (*p == 'v' || *p == 'V') p++;
  sscanf(p, "%d.%d.%d", &a, &b, &c);
  return a * 1000000L + b * 1000L + c;
}

static bool otaPossible() { return esp_ota_get_next_update_partition(nullptr) != nullptr; }

static String updateErr;

static bool fetchLatest(JsonDocument& doc) {
  size_t blk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  String info = " (WLAN " + String(WiFi.RSSI()) + " dBm, RAM " + String((unsigned)(blk / 1024)) + " kB)";
  Serial.printf("Update-Pruefung:%s\n", info.c_str());
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(20);
  HTTPClient http;
  http.setTimeout(15000);
  http.setConnectTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, "https://api.github.com/repos/" GITHUB_REPO "/releases/latest")) {
    updateErr = "Verbindung nicht möglich" + info;
    return false;
  }
  http.setUserAgent("fronius-display/" FW_VERSION);
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  if (code != 200) {
    if (code == HTTPC_ERROR_CONNECTION_REFUSED) updateErr = "Verbindung fehlgeschlagen" + info;
    else if (code == HTTPC_ERROR_READ_TIMEOUT) updateErr = "Zeitüberschreitung" + info;
    else if (code < 0) updateErr = HTTPClient::errorToString(code) + info;
    else if (code == 404) updateErr = "noch keine Version veröffentlicht";
    else if (code == 403 || code == 429) updateErr = "GitHub: zu viele Abfragen (HTTP " + String(code) + "), später erneut";
    else updateErr = "GitHub antwortet HTTP " + String(code);
    Serial.printf("Update-Pruefung: HTTP %d %s\n", code, updateErr.c_str());
    http.end();
    return false;
  }
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    updateErr = String("Antwort unvollständig (") + err.c_str() + ")" + info;
    return false;
  }
  return true;
}

static bool checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) { updateErr = "kein WLAN"; return false; }
  NetLock lock;
  JsonDocument doc;
  if (!fetchLatest(doc)) {
    delay(1500);
    doc.clear();
    if (!fetchLatest(doc)) return false;
  }
  updateErr = "";
  String tag = doc["tag_name"] | "";
  if (tag.startsWith("v") || tag.startsWith("V")) tag.remove(0, 1);
  String url;
  for (JsonObject a : doc["assets"].as<JsonArray>()) {
    String name = a["name"] | "";
    if (name.startsWith(OTA_PREFIX) && name.endsWith(OTA_SUFFIX)) url = a["browser_download_url"] | "";
  }
  Serial.printf("Update-Pruefung: installiert %s, aktuell %s\n", FW_VERSION, tag.c_str());
  String neu = (tag.length() && versionNum(tag) > versionNum(FW_VERSION)) ? tag : "";
  if (neu != updateVersion || url != updateUrl) {
    updateVersion = neu;
    updateUrl = neu.length() ? url : "";
    needRedraw = true;
  }
  return true;
}

static int otaLastPct = -1;

static void otaProgressScreen(int pct, const String& title) {
  if (pct >= 0 && pct == otaLastPct) return;
  canvas.setFont(&fT.font);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM);
  if (pct < 0) {
    canvas.fillScreen(C_BG);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(title, 400, 180);
    canvas.drawRoundRect(100, 230, 600, 40, 10, C_DIM);
    pushAll();
    otaLastPct = 0;
    return;
  }
  otaLastPct = pct;
  canvas.fillRoundRect(104, 234, 592 * pct / 100, 32, 8, C_ACC_GRN);
  canvas.fillRect(330, 290, 140, 40, C_BG);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(String(pct) + " %", 400, 310);
  disp::copyRect((const uint16_t*)canvas.getBuffer(), 100, 230, 600, 100);
}

static void doOta() {
  if (!otaPossible()) {
    toast("Update nur per Web-Installer möglich", C_ACC_ORG, 8000);
    return;
  }
  if (updateUrl.length() == 0) {
    checkForUpdate();
    if (updateUrl.length() == 0) {
      toast("Update-Datei noch nicht bereit – später erneut versuchen", C_ACC_ORG, 8000);
      return;
    }
  }
  daySave();                                   // Tageswerte vor dem Neustart sichern
  pollPaused = true;
  NetLock lock;
  baseValid = false;
  otaProgressScreen(-1, "Installiere Update " + updateVersion + " ...");
  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.onProgress([](int cur, int total) {
    if (total > 0) otaProgressScreen((int)((int64_t)cur * 100 / total), "");
  });
  t_httpUpdate_return r = httpUpdate.update(client, updateUrl);
  if (r == HTTP_UPDATE_OK) {
    otaProgressScreen(100, "");
    canvas.fillRect(0, 160, 800, 40, C_BG);
    canvas.setTextColor(C_TEXT);
    canvas.drawString("Update installiert – Neustart", 400, 180);
    pushAll();
    delay(1500);
    ESP.restart();
  }
  Serial.printf("Update fehlgeschlagen: %s\n", httpUpdate.getLastErrorString().c_str());
  toast("Update fehlgeschlagen", C_ACC_ORG, 8000);
  needRedraw = true;
}

// ---------------------------------------------------------------
// Geometrie
// ---------------------------------------------------------------
static const int M = 10;
static const int HEAD = 50;

struct Rect { int x, y, w, h; };
static bool hit(const Rect& r, int px, int py) {
  return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

static const Rect R_HEADER = { 0, 0, 800, HEAD };
static const Rect R_GEAR   = { 112, 0, 60, HEAD };
static const Rect R_BACK   = { 800 - M - 170, 5, 170, HEAD - 8 };
static const Rect R_YES    = { 140, 270, 240, 80 };
static const Rect R_NO     = { 420, 270, 240, 80 };

// Energiefluss: linkes Quadrat 480 x 430 unter der Kopfzeile
static const int CX = 240, CY = HEAD + 215;      // Mitte (Wechselrichter)
static const int R_CENTER = 48;
static const int R_OUT = 66, R_IN = 48;          // Segmentring aussen / innen
static const int NODE_X[4] = { 96, 384, 96, 384 };              // PV, Verbraucher, Netz, Batterie
static const int NODE_Y[4] = { HEAD + 82, HEAD + 82, HEAD + 330, HEAD + 330 };

// Rechte Spalte "Heute"
static const Rect R_TODAY = { 490, HEAD + 10, 300, 480 - HEAD - 20 };

// ---------------------------------------------------------------
// Zeichnen – Bausteine
// ---------------------------------------------------------------
static void text(const String& s, int x, int y, uint16_t col, textdatum_t datum) {
  canvas.setTextColor(col);
  canvas.setTextDatum(datum);
  canvas.drawString(s, x, y);
}

// Zahl gross, Einheit kleiner und grau, gemeinsame Grundlinie.
// align: 0 = mittig um x, 1 = rechtsbuendig an x, 2 = linksbuendig ab x
static void numUnit(const String& num, const String& unit, int x, int yBase, int align, MemFont& fn,
                    MemFont& fu, uint16_t cn, uint16_t cu = C_DIM) {
  useFont(fn);
  int wn = canvas.textWidth(num);
  int wu = 0;
  if (unit.length()) {
    useFont(fu);
    wu = canvas.textWidth(unit) + 4;
  }
  int tot = wn + wu;
  int x0 = align == 0 ? x - tot / 2 : (align == 1 ? x - tot : x);
  useFont(fn);
  text(num, x0, yBase, cn, textdatum_t::baseline_left);
  if (wu) {
    useFont(fu);
    text(unit, x0 + wn + 4, yBase, cu, textdatum_t::baseline_left);
  }
}

static void button(const Rect& r, const String& label, bool selected, uint16_t base = C_BTN,
                   uint16_t txt = C_TEXT, bool small = false) {
  canvas.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 12, selected ? C_BTN_SEL : base);
  if (selected) canvas.drawRoundRect(r.x, r.y, r.w, r.h, 12, C_ACC_GRN);
  if (small) fitFont(label, r.w - 12, &fT, &fS);
  else fitFont(label, r.w - 16, &fB, &fT, &fS);
  text(label, r.x + r.w / 2, r.y + r.h / 2, selected ? C_SEL_TXT : txt, MC_DATUM);
}

static void drawGear(int cx, int cy, uint16_t col, uint16_t hole) {
  for (int i = 0; i < 8; i++) {
    float a = i * 45.0f;
    canvas.fillArc(cx, cy, 11, 17, a - 11, a + 11, col);
  }
  canvas.fillSmoothCircle(cx, cy, 13, col);
  canvas.fillSmoothCircle(cx, cy, 5, hole);
}

static void drawWifi(int cx, int cy, int bars) {
  for (int i = 0; i < 3; i++) {
    int r1 = 8 + i * 7;
    uint16_t col = bars < 0 ? C_ACC_RED : (bars > i ? C_TEXT : C_OFF);
    canvas.fillArc(cx, cy, r1 - 3, r1, 225, 315, col);
  }
  canvas.fillSmoothCircle(cx, cy - 1, 3, bars < 0 ? C_ACC_RED : C_TEXT);
  if (bars < 0) canvas.drawLine(cx - 14, cy - 24, cx + 14, cy + 2, C_ACC_RED);
}

static void pushRegion(const Rect& r) {
  disp::copyRect((const uint16_t*)canvas.getBuffer(), r.x, r.y, r.w, r.h);
}

static void drawDialog() {
  if (confirmAct == A_NONE) return;
  canvas.fillRect(0, 0, 800, 480, 0x0000);
  canvas.fillSmoothRoundRect(110, 100, 580, 280, 16, C_TILE);
  canvas.drawRoundRect(110, 100, 580, 280, 16, C_ACC_ORG);
  fitFont(confirmL1, 540, &fB, &fT);
  text(confirmL1, 400, 160, C_TEXT, MC_DATUM);
  fitFont(confirmL2, 540, &fB, &fT, &fS);
  text(confirmL2, 400, 205, C_TEXT, MC_DATUM);
  button(R_YES, "Ja", false, C_BTN_RED);
  button(R_NO, "Abbrechen", false);
}

static void showMessage(const String& s) {
  baseValid = false;
  canvas.fillScreen(C_BG);
  fitFont(s, 760, &fB, &fT);
  text(s, 400, 240, C_TEXT, MC_DATUM);
  pushAll();
}

// ---------------------------------------------------------------
// Symbole (eigene, schlichte Formen)
// ---------------------------------------------------------------
static void iconSun(int x, int y, uint16_t c) {
  canvas.fillSmoothCircle(x, y, 11, c);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    canvas.drawWideLine(x + 16 * cosf(a), y + 16 * sinf(a), x + 22 * cosf(a), y + 22 * sinf(a), 1.5f, c);
  }
}

static void iconHouse(int x, int y, uint16_t c) {
  canvas.fillTriangle(x - 21, y, x, y - 19, x + 21, y, c);
  canvas.fillRect(x - 14, y, 29, 19, c);
  canvas.fillRect(x - 4, y + 7, 9, 12, C_TILE);
}

static void iconPylon(int x, int y, uint16_t c) {
  canvas.drawWideLine(x - 12, y + 20, x - 3, y - 20, 1.5f, c);
  canvas.drawWideLine(x + 12, y + 20, x + 3, y - 20, 1.5f, c);
  canvas.drawWideLine(x - 16, y - 10, x + 16, y - 10, 1.5f, c);
  canvas.drawWideLine(x - 12, y, x + 12, y, 1.5f, c);
  canvas.drawWideLine(x - 9, y + 6, x + 10, y + 18, 1.0f, c);
  canvas.drawWideLine(x + 9, y + 6, x - 10, y + 18, 1.0f, c);
}

static void iconBattery(int x, int y, float soc, uint16_t c) {
  canvas.fillSmoothRoundRect(x - 13, y - 19, 27, 40, 4, c);
  canvas.fillRect(x - 10, y - 16, 21, 34, C_TILE);
  canvas.fillRect(x - 5, y - 24, 11, 5, c);
  if (!isnan(soc)) {
    int h = (int)lroundf(30 * constrain(soc, 0.0f, 100.0f) / 100.0f);
    if (h > 0) canvas.fillRect(x - 7, y + 15 - h, 15, h, c);
  }
}

static void iconInverter(int x, int y, uint16_t c) {
  canvas.drawRoundRect(x - 24, y - 22, 49, 45, 6, c);
  canvas.drawRoundRect(x - 23, y - 21, 47, 43, 5, c);
  canvas.drawWideLine(x - 23, y - 4, x + 23, y - 4, 1.0f, c);
  // oben: Gleichstrom (zwei Linien), unten: Sinus
  canvas.drawWideLine(x - 10, y - 15, x + 10, y - 15, 1.0f, c);
  for (int i = -10; i <= 10; i += 5) canvas.drawPixel(x + i, y - 10, c);
  float px = x - 14, py = y + 9;
  for (int i = 1; i <= 28; i++) {
    float nx = x - 14 + i, ny = y + 9 - 6 * sinf(i / 28.0f * 2 * PI);
    canvas.drawWideLine(px, py, nx, ny, 1.0f, c);
    px = nx; py = ny;
  }
}

// Segmentring: 20 Kacheln, die ersten "frac" kraeftig, der Rest blass
static void segRing(int cx, int cy, float frac, uint16_t col) {
  const int n = 20;
  uint16_t pale = mix565(C_BG, col, 0.28f);
  int full = isnan(frac) ? 0 : (int)lroundf(constrain(frac, 0.0f, 1.0f) * n);
  for (int i = 0; i < n; i++) {
    float a0 = fmodf(270.0f + i * 18.0f + 2.0f, 360.0f);
    canvas.fillArc(cx, cy, R_IN + 2, R_OUT, a0, a0 + 14.0f, i < full ? col : pale);
  }
  canvas.fillSmoothCircle(cx, cy, R_IN - 6, C_TILE);
}

// ---------------------------------------------------------------
// Startseite: Energiefluss
// ---------------------------------------------------------------
struct FlowLine {
  float x0, y0, x1, y1;   // Knoten-Ende -> Mitte-Ende
  uint16_t col;
  bool active;
  bool toCenter;          // Punkte laufen zur Mitte
  float speed;            // Durchlaeufe pro Sekunde
  float phase;
};
static FlowLine lines[4];

static Rect lineBox(const FlowLine& l) {
  int x0 = (int)floorf(fminf(l.x0, l.x1)) - 8, y0 = (int)floorf(fminf(l.y0, l.y1)) - 8;
  int x1 = (int)ceilf(fmaxf(l.x0, l.x1)) + 8, y1 = (int)ceilf(fmaxf(l.y0, l.y1)) + 8;
  return { x0, y0, x1 - x0, y1 - y0 };
}

static void setupLine(int i, float watt, bool toCenter, uint16_t col, float scaleW) {
  FlowLine& l = lines[i];
  float dx = CX - NODE_X[i], dy = CY - NODE_Y[i];
  float len = sqrtf(dx * dx + dy * dy);
  float ux = dx / len, uy = dy / len;
  float s0 = R_OUT + 6, s1 = len - R_CENTER - 6;
  l.x0 = NODE_X[i] + ux * s0;
  l.y0 = NODE_Y[i] + uy * s0;
  l.x1 = NODE_X[i] + ux * s1;
  l.y1 = NODE_Y[i] + uy * s1;
  bool act = !isnan(watt) && fabsf(watt) >= FLOW_MIN_W;
  l.col = col;
  l.toCenter = toCenter;
  // mehr Leistung = schnellere Punkte
  float rel = isnan(watt) ? 0 : constrain(fabsf(watt) / scaleW, 0.0f, 1.0f);
  l.speed = 0.35f + 0.9f * rel;
  if (act != l.active) l.phase = 0;
  l.active = act;
  canvas.drawWideLine(l.x0, l.y0, l.x1, l.y1, 1.5f, act ? mix565(C_BG, col, 0.45f) : C_LINE);
}

// Laufende Punkte: klein und dicht, damit die Bewegung ruhig wirkt
static const int DOTS = 5;
static void drawDotsOn(const FlowLine& l) {
  for (int k = 0; k < DOTS; k++) {
    float t = fmodf((float)k / DOTS + l.phase, 1.0f);
    if (!l.toCenter) t = 1.0f - t;
    float x = l.x0 + (l.x1 - l.x0) * t, y = l.y0 + (l.y1 - l.y0) * t;
    canvas.fillSmoothCircle((int)lroundf(x), (int)lroundf(y), 3, l.col);
  }
}

// Ruhige Darstellung: zwei feste Pfeile (Winkel) in Flussrichtung
static void drawArrowsOn(const FlowLine& l) {
  float dx = l.x1 - l.x0, dy = l.y1 - l.y0;
  float len = sqrtf(dx * dx + dy * dy);
  float ux = dx / len, uy = dy / len;
  if (!l.toCenter) { ux = -ux; uy = -uy; }
  for (float t : { 0.33f, 0.67f }) {
    float cx = l.x0 + dx * t, cy = l.y0 + dy * t;
    float tipX = cx + ux * 6, tipY = cy + uy * 6;
    // zwei Schenkel, je 35° zur Gegenrichtung
    for (int sgn : { -1, 1 }) {
      float a = 0.61f * sgn;
      float bx = -(ux * cosf(a) - uy * sinf(a)), by = -(ux * sinf(a) + uy * cosf(a));
      canvas.drawWideLine(tipX, tipY, tipX + bx * 11, tipY + by * 11, 1.6f, l.col);
    }
  }
}

// Werte aktuell? Nach 3 fehlgeschlagenen Abrufen grau, Punkte stehen
static bool online() { return flow.valid && failCount < 2; }

// Stoerung fuer das grosse Fehlerbild der Startseite.
// Nur wenn WLAN und Wechselrichter ueberhaupt eingerichtet sind.
enum NetErr { NE_NONE = 0, NE_WIFI, NE_INV };
static uint32_t wifiLostSince = 0;   // 0 = verbunden

static NetErr netError() {
  if (wifiSsid.length() == 0 || froniusHost.length() == 0) return NE_NONE;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLostSince == 0) wifiLostSince = millis() ? millis() : 1;
    if (millis() - wifiLostSince >= WIFI_GRACE_MS) return NE_WIFI;
    return NE_NONE;
  }
  wifiLostSince = 0;
  if (failCount >= 2 || (!flow.valid && failCount >= 1)) return NE_INV;
  return NE_NONE;
}

// Dicker Strich mit dunklem Rand, damit er sich vom Symbol abhebt
static void strike(int cx, int cy, int half) {
  canvas.drawWideLine(cx - half, cy - half, cx + half, cy + half, 13, C_BG);
  canvas.drawWideLine(cx - half, cy - half, cx + half, cy + half, 7, C_ACC_RED);
}

static void bigWifiSymbol(int cx, int cy) {
  int by = cy + 55;
  for (int i = 0; i < 3; i++) {
    int r1 = 52 + i * 38;
    canvas.fillArc(cx, by, r1 - 20, r1, 225, 315, C_ACC_ORG);
  }
  canvas.fillSmoothCircle(cx, by - 4, 14, C_ACC_ORG);
  strike(cx, cy - 10, 95);
}

// Das Wechselrichter-Symbol aus der Mitte des Energieflusses, gross
static void bigInverterSymbol(int cx, int cy) {
  const float k = 2.6f;
  int w = (int)(49 * k), h = (int)(45 * k);
  int x = cx - w / 2, y = cy - h / 2;
  uint16_t c = C_ACC_ORG;
  canvas.fillSmoothRoundRect(x, y, w, h, 16, c);
  canvas.fillSmoothRoundRect(x + 8, y + 8, w - 16, h - 16, 10, C_BG);
  int dy = cy - (int)(4 * k);
  canvas.drawWideLine(x + 8, dy, x + w - 8, dy, 3.0f, c);
  // oben: Gleichstrom (Linie und Punkte), unten: Sinus
  canvas.drawWideLine(cx - 10 * k, cy - 15 * k, cx + 10 * k, cy - 15 * k, 3.0f, c);
  for (int i = -10; i <= 10; i += 5) canvas.fillSmoothCircle(cx + (int)(i * k), cy - (int)(10 * k), 3, c);
  float px = cx - 14 * k, py = cy + 9 * k;
  for (int i = 1; i <= 28; i++) {
    float nx = cx - 14 * k + i * k, ny = cy + 9 * k - 6 * k * sinf(i / 28.0f * 2 * PI);
    canvas.drawWideLine(px, py, nx, ny, 3.0f, c);
    px = nx; py = ny;
  }
  strike(cx, cy, 100);
}

// Fehlerbild statt Energiefluss und Tageswerten – keine veralteten Werte wie aktuelle
static void drawErrorPanel(NetErr e) {
  canvas.fillRect(0, HEAD, 800, 480 - HEAD, C_BG);
  int cy = HEAD + 150;
  if (e == NE_WIFI) bigWifiSymbol(400, cy);
  else bigInverterSymbol(400, cy);
  String t1 = e == NE_WIFI ? String("Kein WLAN") : String("Wechselrichter nicht erreichbar");
  String t2 = e == NE_WIFI ? "Suche Netz \"" + wifiSsid + "\" ..."
                           : "Keine Antwort von " + froniusHost + " – frage weiter nach ...";
  fitFont(t1, 760, &fB, &fT);
  text(t1, 400, HEAD + 290, C_TEXT, MC_DATUM);
  fitFont(t2, 760, &fT, &fS);
  text(t2, 400, HEAD + 335, C_DIM, MC_DATUM);
  useFont(fS);
  if (flow.valid && lastOkTime.length())
    text("Letzte Werte von " + lastOkTime + " – werden ausgeblendet, bis wieder Daten kommen",
         400, HEAD + 372, C_DIM, MC_DATUM);
  text("Einstellungen: Zahnrad antippen", 400, 480 - 22, C_DIM, MC_DATUM);
}

static void nodeLabel(int i, float watt, const String& sub, uint16_t subCol) {
  String n, u;
  powerParts(watt, n, u);
  numUnit(n, u, NODE_X[i], NODE_Y[i] + 34, 0, fT, fXS, online() ? C_TEXT : C_DIM);
  if (sub.length()) {
    useFont(fXS);
    text(sub, NODE_X[i], NODE_Y[i] + R_OUT + 14, subCol, MC_DATUM);
  }
}

static uint16_t gridColor() {
  return (!isnan(flow.pGrid) && flow.pGrid >= FLOW_MIN_W) ? C_ACC_ORG : C_TEXT;
}

static void drawFlow() {
  float pv = pvTotalW(), load = loadW();
  float scalePv = pvMaxW ? (float)pvMaxW : (float)PV_MAX_DEFAULT_W;
  float scaleNet = fmaxf(scalePv, 10000.0f);
  uint16_t gcol = gridColor();

  // Leitungen (unter den Ringen); ohne aktuelle Daten kein Fluss
  bool on = online();
  setupLine(N_PV, on ? pv : NAN, true, C_SUN, scalePv);
  setupLine(N_LOAD, on ? load : NAN, false, C_ACC_BLUE, scaleNet);
  setupLine(N_GRID, on ? flow.pGrid : NAN, !isnan(flow.pGrid) && flow.pGrid > 0, gcol, scaleNet);
  setupLine(N_BATT, (on && flow.hasBatt) ? flow.pAkku : NAN, !isnan(flow.pAkku) && flow.pAkku > 0, C_ACC_GRN,
            fmaxf(battCapWh() > 0 ? battCapWh() / 2.0f : 5000.0f, 3000.0f));

  // Mitte: Wechselrichter
  canvas.drawCircle(CX, CY, R_CENTER, C_DIM);
  canvas.drawCircle(CX, CY, R_CENTER - 1, C_DIM);
  iconInverter(CX, CY, C_TEXT);

  // Ringe
  segRing(NODE_X[N_PV], NODE_Y[N_PV], pv / scalePv, C_SUN);
  segRing(NODE_X[N_LOAD], NODE_Y[N_LOAD], load / scaleNet, C_ACC_BLUE);
  segRing(NODE_X[N_GRID], NODE_Y[N_GRID], isnan(flow.pGrid) ? NAN : fabsf(flow.pGrid) / scaleNet, gcol);
  segRing(NODE_X[N_BATT], NODE_Y[N_BATT], flow.hasBatt ? flow.soc / 100.0f : NAN,
          flow.hasBatt ? C_ACC_GRN : C_OFF);

  iconSun(NODE_X[N_PV], NODE_Y[N_PV] - 10, C_SUN);
  iconHouse(NODE_X[N_LOAD], NODE_Y[N_LOAD] - 8, C_ACC_BLUE);
  iconPylon(NODE_X[N_GRID], NODE_Y[N_GRID] - 10, gcol);
  iconBattery(NODE_X[N_BATT], NODE_Y[N_BATT] - 10, flow.soc, flow.hasBatt ? C_ACC_GRN : C_OFF);

  // Werte und Beschriftungen
  nodeLabel(N_PV, pv, "Erzeugung", C_SUN);
  nodeLabel(N_LOAD, load, "Verbrauch", C_ACC_BLUE);
  String gsub = "Netz";
  if (!isnan(flow.pGrid) && fabsf(flow.pGrid) >= FLOW_MIN_W) gsub = flow.pGrid > 0 ? "Bezug" : "Einspeisung";
  nodeLabel(N_GRID, flow.pGrid, gsub, gcol);

  int bx = NODE_X[N_BATT], by = NODE_Y[N_BATT];
  if (flow.hasBatt) {
    numUnit(isnan(flow.soc) ? String("--") : String((int)lroundf(flow.soc)), "%", bx, by + 34, 0, fT, fXS,
            online() ? C_TEXT : C_DIM);
    String bsub = "Batterie";
    if (!isnan(flow.pAkku) && fabsf(flow.pAkku) >= FLOW_MIN_W)
      bsub = (flow.pAkku < 0 ? "lädt " : "entlädt ") + powerText(flow.pAkku);
    useFont(fXS);
    text(bsub, bx, by + R_OUT + 14, C_ACC_GRN, MC_DATUM);
  } else {
    useFont(fXS);
    text("keine Batterie", bx, by + R_OUT + 14, C_DIM, MC_DATUM);
  }
}

static void drawTodayPanel() {
  const Rect& r = R_TODAY;
  canvas.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 14, C_TILE);
  useFont(fT);
  text("Heute", r.x + 16, r.y + 24, C_SUN, ML_DATUM);
  if (today.valid && dayS.startMin > 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), "seit %02u:%02u", dayS.startMin / 60, dayS.startMin % 60);
    useFont(fXS);
    text(buf, r.x + r.w - 16, r.y + 26, C_DIM, MR_DATUM);
  }
  if (!today.valid) {
    useFont(fS);
    text(WiFi.status() == WL_CONNECTED ? "warte auf Uhrzeit ..." : "kein WLAN", r.x + 16, r.y + 80, C_DIM,
         ML_DATUM);
    return;
  }
  struct Row { const char* name; float wh; uint16_t col; };
  Row rows[5] = {
    { "PV-Ertrag", today.pv, C_SUN },
    { "Verbrauch", today.load, C_ACC_BLUE },
    { "Netzbezug", today.imp, C_ACC_ORG },
    { "Einspeisung", today.exp, C_TEXT },
    { "Batterie geladen", flow.hasBatt ? today.battIn : NAN, C_ACC_GRN },
  };
  int y = r.y + 70;
  for (const Row& row : rows) {
    canvas.fillSmoothRoundRect(r.x + 16, y - 10, 6, 20, 2, row.col);
    useFont(fS);
    text(row.name, r.x + 32, y, C_DIM, ML_DATUM);
    String n, u;
    energyParts(row.wh, n, u);
    numUnit(n, u, r.x + r.w - 16, y + 9, 1, fT, fXS, C_TEXT);
    y += 44;
  }
  y += 8;
  struct Bar { const char* name; float pct; uint16_t col; };
  Bar bars[2] = { { "Autarkie", today.autarkie, C_ACC_GRN }, { "Eigenverbrauch", today.eigen, C_SUN } };
  for (const Bar& b : bars) {
    useFont(fS);
    text(b.name, r.x + 16, y, C_DIM, ML_DATUM);
    numUnit(isnan(b.pct) ? String("--") : String((int)lroundf(b.pct)), "%", r.x + r.w - 16, y + 9, 1, fT, fXS,
            C_TEXT);
    int bw = r.w - 32, bx = r.x + 16, by = y + 20;
    canvas.fillSmoothRoundRect(bx, by, bw, 10, 5, mix565(C_TILE, b.col, 0.25f));
    if (!isnan(b.pct) && b.pct > 0)
      canvas.fillSmoothRoundRect(bx, by, (int)(bw * b.pct / 100.0f) < 10 ? 10 : (int)(bw * b.pct / 100.0f), 10, 5,
                                 b.col);
    y += 58;
  }
}

static void drawFlowHeader() {
  canvas.fillRect(0, 0, 800, HEAD, C_BG);
  useFont(fT);
  text("Fronius", M + 6, HEAD / 2, C_SUN, ML_DATUM);
  drawGear(R_GEAR.x + R_GEAR.w / 2, HEAD / 2, C_DIM, C_BG);

  int wx = 800 - M - 20;
  drawWifi(wx, HEAD / 2 + 12, wifiBars());
  int xr = wx - 34;
  String uz = uhrzeit();
  useFont(fT);
  if (millis() < toastUntil) {
    fitFont(toastText, xr - 200, &fT, &fS);
    text(toastText, xr, HEAD / 2, toastColor, MR_DATUM);
  } else if (WiFi.status() != WL_CONNECTED) {
    text("WLAN getrennt", xr, HEAD / 2, C_ACC_ORG, MR_DATUM);
  } else if (!online()) {
    text("Wechselrichter nicht erreichbar", xr, HEAD / 2, C_ACC_ORG, MR_DATUM);
  } else {
    text(uz, xr, HEAD / 2, C_DIM, MR_DATUM);
    if (updateVersion.length()) {
      int w = canvas.textWidth(uz);
      useFont(fS);
      text("Update " + updateVersion + " verfügbar", xr - w - 20, HEAD / 2, C_ACC_ORG, MR_DATUM);
    }
  }
}

// ---------------------------------------------------------------
// Detailseiten (Tipp auf einen Kreis)
// ---------------------------------------------------------------
static const char* const DETAIL_TITLE[4] = { "Photovoltaik", "Verbraucher", "Netz", "Batterie" };
static const uint16_t DETAIL_COL[4] = { C_SUN, C_ACC_BLUE, C_TEXT, C_ACC_GRN };

static void pageHeader(const char* title, uint16_t col) {
  useFont(fT);
  text(title, M + 6, HEAD / 2, col, ML_DATUM);
  if (millis() < toastUntil) {
    fitFont(toastText, R_BACK.x - 240, &fT, &fS);
    text(toastText, R_BACK.x - 16, HEAD / 2, toastColor, MR_DATUM);
  }
  button(R_BACK, "< Zurück", false, C_BTN, C_TEXT, true);
}

// Zeile: Name links, Wert rechts (Zahl gross, Einheit klein)
static int detailRow(const Rect& box, int y, const String& name, const String& num, const String& unit,
                     uint16_t nameCol = C_DIM, uint16_t valCol = C_TEXT) {
  useFont(fS);
  text(name, box.x + 18, y, nameCol, ML_DATUM);
  numUnit(num, unit, box.x + box.w - 18, y + 10, 1, fB, fS, valCol);
  return y + 52;
}
static int detailPower(const Rect& b, int y, const String& name, float w, uint16_t nameCol = C_DIM) {
  String n, u;
  powerParts(w, n, u);
  return detailRow(b, y, name, n, u, nameCol);
}
static int detailEnergy(const Rect& b, int y, const String& name, float wh) {
  String n, u;
  energyParts(wh, n, u);
  return detailRow(b, y, name, n, u);
}

// Kleiner Hinweis am unteren Rand eines Kastens; bricht bei Bedarf in zwei Zeilen um.
// yBottom = Mitte der untersten Zeile
static void boxNote(const Rect& b, int yBottom, const String& s) {
  useFont(fXS);
  int maxW = b.w - 36;
  String l1 = s, l2;
  if (canvas.textWidth(s) > maxW) {
    // letzte Leerstelle, bis zu der die erste Zeile passt
    int cut = -1;
    for (int i = 0; i < (int)s.length(); i++)
      if (s[i] == ' ' && canvas.textWidth(s.substring(0, i)) <= maxW) cut = i;
    if (cut > 0) { l1 = s.substring(0, cut); l2 = s.substring(cut + 1); }
  }
  if (l2.length()) {
    text(l1, b.x + 18, yBottom - 22, C_DIM, ML_DATUM);
    text(l2, b.x + 18, yBottom, C_DIM, ML_DATUM);
  } else {
    text(l1, b.x + 18, yBottom, C_DIM, ML_DATUM);
  }
}

static void boxTitle(const Rect& b, const char* t, uint16_t col) {
  canvas.fillSmoothRoundRect(b.x, b.y, b.w, b.h, 14, C_TILE);
  useFont(fT);
  text(t, b.x + 18, b.y + 26, col, ML_DATUM);
}

static const Rect R_DL = { M, HEAD + 10, 385, 480 - HEAD - 20 };
static const Rect R_DR = { M + 395, HEAD + 10, 385, 480 - HEAD - 20 };

static void drawDetailPv() {
  boxTitle(R_DL, "Jetzt", C_SUN);
  int y = R_DL.y + 76;
  y = detailPower(R_DL, y, "Fronius Wechselrichter", flow.pPv);
  for (int i = 0; i < flow.nSec; i++)
    if (flow.sec[i].gen) y = detailPower(R_DL, y, flow.sec[i].label, fabsf(flow.sec[i].p));
  float pv = pvTotalW();
  canvas.drawFastHLine(R_DL.x + 18, y - 26, R_DL.w - 36, C_BTN);
  y = detailPower(R_DL, y, "Gesamt", pv, C_TEXT);
  if (pvMaxW) {
    y = detailRow(R_DL, y, "Anlage", fmt(pvMaxW / 1000.0f, 1), "kWp");
    y = detailRow(R_DL, y, "Auslastung", isnan(pv) ? String("--") : String((int)lroundf(pv * 100.0f / pvMaxW)), "%");
  }

  boxTitle(R_DR, "Heute", C_SUN);
  y = R_DR.y + 76;
  y = detailEnergy(R_DR, y, "Fronius (gerechnet)", today.pvFr);
  if (meter.genValid) y = detailEnergy(R_DR, y, "Weitere Wechselrichter", today.pvGen);
  y = detailEnergy(R_DR, y, "Gesamt", today.pv);
  useFont(fXS);
  boxNote(R_DR, R_DR.y + R_DR.h - 52, "Fronius: Leistung × Zeit, solange das Display läuft.");
  text("Zähler Wechselrichter heute: " + (isnan(today.invCounter) ? String("--") : fmt(today.invCounter / 1000.0f, 1) + " kWh"),
       R_DR.x + 18, R_DR.y + R_DR.h - 28, C_DIM, ML_DATUM);
}

static void drawDetailLoad() {
  boxTitle(R_DL, "Jetzt", C_ACC_BLUE);
  int y = R_DL.y + 76;
  y = detailPower(R_DL, y, "Hausverbrauch", loadW(), C_TEXT);
  for (int i = 0; i < flow.nOhm; i++) {
    y = detailPower(R_DL, y, "Heizstab (Ohmpilot)", flow.ohmP[i]);
    if (!isnan(flow.ohmTemp[i])) y = detailRow(R_DL, y, "Wassertemperatur", fmt(flow.ohmTemp[i], 1), "°C");
  }
  for (int i = 0; i < flow.nSec; i++)
    if (!flow.sec[i].gen) y = detailPower(R_DL, y, flow.sec[i].label, fabsf(flow.sec[i].p));
  if (flow.nOhm == 0) boxNote(R_DL, R_DL.y + R_DL.h - 28, "Wallbox und Heizstab erscheinen hier, sobald vorhanden.");

  boxTitle(R_DR, "Heute", C_ACC_BLUE);
  y = R_DR.y + 76;
  y = detailEnergy(R_DR, y, "Verbrauch", today.load);
  y = detailEnergy(R_DR, y, "davon aus dem Netz", today.imp);
  y = detailRow(R_DR, y, "Autarkie", isnan(today.autarkie) ? String("--") : String((int)lroundf(today.autarkie)), "%");
  y = detailRow(R_DR, y, "Eigenverbrauch", isnan(today.eigen) ? String("--") : String((int)lroundf(today.eigen)), "%");
}

static void drawDetailGrid() {
  // links: Phasen als Tabelle
  boxTitle(R_DL, "Phasen", C_TEXT);
  const int c0 = R_DL.x + 18, cw = (R_DL.w - 36 - 70) / 3;
  useFont(fS);
  for (int i = 0; i < 3; i++) text("L" + String(i + 1), c0 + 70 + cw * i + cw - 6, R_DL.y + 70, C_DIM, MR_DATUM);
  const char* rowName[3] = { "V", "A", "W" };
  for (int r = 0; r < 3; r++) {
    int y = R_DL.y + 116 + r * 52;
    useFont(fS);
    text(rowName[r], c0, y, C_DIM, ML_DATUM);
    for (int i = 0; i < 3; i++) {
      float v = r == 0 ? meter.v[i] : (r == 1 ? meter.a[i] : meter.p[i]);
      String s = !meter.valid ? String("--") : (r == 0 ? fmt(v, 0) : (r == 1 ? fmt(fabsf(v), 1) : fmt(fabsf(v), 0)));
      uint16_t col = C_TEXT;
      if (r == 2 && meter.valid && !isnan(v)) col = v > 5 ? C_ACC_ORG : (v < -5 ? C_ACC_GRN : C_TEXT);
      useFont(fT);
      text(s, c0 + 70 + cw * i + cw - 6, y, col, MR_DATUM);
    }
  }
  int y = R_DL.y + 116 + 3 * 52 + 6;
  useFont(fXS);
  text("Leistung: orange = Bezug, grün = Einspeisung", c0, y, C_DIM, ML_DATUM);
  y += 40;
  y = detailRow(R_DL, y, "Frequenz", meter.valid ? fmt(meter.freq, 2) : String("--"), "Hz");

  boxTitle(R_DR, "Netz", C_TEXT);
  y = R_DR.y + 76;
  String gname = "Jetzt";
  if (!isnan(flow.pGrid) && fabsf(flow.pGrid) >= FLOW_MIN_W) gname = flow.pGrid > 0 ? "Bezug jetzt" : "Einspeisung jetzt";
  y = detailPower(R_DR, y, gname, flow.pGrid, gridColor());
  y = detailEnergy(R_DR, y, "Bezug heute", today.imp);
  y = detailEnergy(R_DR, y, "Einspeisung heute", today.exp);
  y = detailEnergy(R_DR, y, "Bezug gesamt", meter.valid ? (float)meter.eImp : NAN);
  y = detailEnergy(R_DR, y, "Einspeisung gesamt", meter.valid ? (float)meter.eExp : NAN);
}

static void drawDetailBatt() {
  float cap = battCapWh();
  boxTitle(R_DL, "Jetzt", C_ACC_GRN);
  int y = R_DL.y + 76;
  y = detailRow(R_DL, y, "Ladestand", isnan(flow.soc) ? String("--") : fmt(flow.soc, 1), "%", C_TEXT);
  // Balken
  int bx = R_DL.x + 18, bw = R_DL.w - 36;
  canvas.fillSmoothRoundRect(bx, y - 22, bw, 14, 7, mix565(C_TILE, C_ACC_GRN, 0.25f));
  if (!isnan(flow.soc) && flow.soc > 0)
    canvas.fillSmoothRoundRect(bx, y - 22, max(14, (int)(bw * flow.soc / 100.0f)), 14, 7, C_ACC_GRN);
  y += 12;
  String pname = "Leistung";
  if (!isnan(flow.pAkku) && fabsf(flow.pAkku) >= FLOW_MIN_W) pname = flow.pAkku < 0 ? "Lädt mit" : "Entlädt mit";
  y = detailPower(R_DL, y, pname, flow.pAkku);
  y = detailRow(R_DL, y, "Betrieb", battModeText(flow.battMode), "");
  if (cap > 0) {
    float drin = isnan(flow.soc) ? NAN : cap * flow.soc / 100.0f;
    String n, u;
    energyParts(drin, n, u);
    y = detailRow(R_DL, y, "Energie im Speicher", n, u);
    // grobe Restzeit bei aktueller Leistung
    if (!isnan(flow.pAkku) && fabsf(flow.pAkku) >= 100 && !isnan(flow.soc)) {
      float h = flow.pAkku > 0 ? drin / flow.pAkku : (cap - drin) / -flow.pAkku;
      String d = dauerText(h);
      if (d.length()) y = detailRow(R_DL, y, flow.pAkku > 0 ? "Leer in ca." : "Voll in ca.", d, "");
    }
  }

  boxTitle(R_DR, "Speicher", C_ACC_GRN);
  if (storage.model.length()) {
    fitFont(storage.model, R_DR.w - 150, &fXS);
    text(storage.model, R_DR.x + R_DR.w - 18, R_DR.y + 26, C_DIM, MR_DATUM);
  }
  y = R_DR.y + 76;
  if (cap > 0) {
    String n, u;
    energyParts(cap, n, u);
    y = detailRow(R_DR, y, battCapAuto() ? "Kapazität" : "Kapazität (eingestellt)", n, u);
  }
  if (storage.valid) {
    y = detailRow(R_DR, y, "Zelltemperatur", fmt(storage.tempC, 0), "°C");
    y = detailRow(R_DR, y, "Spannung", fmt(storage.voltDc, 0), "V");
  }
  y = detailEnergy(R_DR, y, "Heute geladen", today.battIn);
  y = detailEnergy(R_DR, y, "Heute entladen", today.battOut);
  boxNote(R_DR, R_DR.y + R_DR.h - 28, cap <= 0 ? String("Kapazität einstellen: Zahnrad > Fronius")
                                                : String("Heute: Leistung × Zeit, solange das Display läuft."));
}

static void drawDetail() {
  pageHeader(DETAIL_TITLE[detailSel], DETAIL_COL[detailSel]);
  switch (detailSel) {
    case N_PV:   drawDetailPv(); break;
    case N_LOAD: drawDetailLoad(); break;
    case N_GRID: drawDetailGrid(); break;
    default:     drawDetailBatt(); break;
  }
}

// ---------------------------------------------------------------
// Einstellungen – Reiter Fronius | WLAN | IP | Anzeige | System
// ---------------------------------------------------------------
// Felder: 0 IP Wechselrichter, 1 PV-Leistung kWp, 2 Batterie kWh,
//         3 WLAN, 4 WLAN-Passwort, 5 feste IP, 6 Subnetz, 7 Gateway, 8 DNS
static const int NF = 9;
static String   edVal[NF];
static int      edField = 0;
static int      edTab = 0;
static bool     edShowPass = false;
static bool     edIpFixed = false;
static uint32_t edPoll = 5000;
static bool     edAnim = true;
static bool     picPage = false;
enum KbLayer { KB_LOWER, KB_UPPER, KB_SYM };
static KbLayer  kbLayer = KB_LOWER;

static bool   wifiList = false;
static int    scanN = 0;
static String scanSsid[8];
static int    scanRssi[8];

static const char* const FIELD_LABEL[NF] = { "IP Wechselrichter", "PV-Leistung kWp", "Batterie kWh", "WLAN",
                                             "WLAN-Passwort", "IP-Adresse", "Subnetz", "Gateway", "DNS" };
static const int NTAB = 5;
static const char* const TAB_NAME[NTAB] = { "Fronius", "WLAN", "IP", "Anzeige", "System" };

static const int FY0 = HEAD + 4;
static const int FH  = 42;
static const int FG  = 6;
static const int FX  = 230;
static const int SIDE_BTN_W = 130;
static Rect tabRect(int i) { return { M + i * 98, 5, 92, HEAD - 8 }; }
static const Rect R_CANCEL = { 800 - M - 140 - 8 - 140, 5, 140, HEAD - 8 };
static const Rect R_SAVE   = { 800 - M - 140, 5, 140, HEAD - 8 };

static bool isPassField(int i) { return i == 4; }
static bool hasSideBtn(int i) { return i == 3 || i == 4; }
static bool isPadField(int i) { return i <= 2 || i >= 5; }

// Reiter WLAN: Felder 3 und 4 mit Tastatur
static Rect fieldRect(int i) {
  int row = i - 3;
  int w = hasSideBtn(i) ? (800 - M - FX - SIDE_BTN_W - 10) : (800 - M - FX);
  return { FX, FY0 + row * (FH + FG), w, FH };
}
static Rect sideBtn(int i) {
  int row = i - 3;
  return { 800 - M - SIDE_BTN_W, FY0 + row * (FH + FG), SIDE_BTN_W, FH };
}

// Reiter Fronius und IP: links Felder, rechts Ziffernblock
static Rect froField(int i) { return { M + 170, HEAD + 16 + i * 66, 300, 50 }; }
static const Rect R_TEST = { M, HEAD + 16 + 3 * 66, 470, 56 };
static const Rect R_IP_AUTO = { M, HEAD + 10, 225, 56 };
static const Rect R_IP_FIX  = { M + 235, HEAD + 10, 225, 56 };
static Rect ipField(int i) { return { M + 160, HEAD + 86 + (i - 5) * 68, 310, 50 }; }
static const int PAD_X = 490, PAD_Y = HEAD + 10;
static const int PAD_W = (800 - M - PAD_X - 2 * 8) / 3;
static const int PAD_H = (480 - M - PAD_Y - 3 * 8) / 4;
static const char* const PAD_KEYS[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "<-" };
static Rect padKey(int i) { return { PAD_X + (i % 3) * (PAD_W + 8), PAD_Y + (i / 3) * (PAD_H + 8), PAD_W, PAD_H }; }

// Reiter Anzeige
static Rect optBtn(int row, int col) { return { 250 + col * 190, HEAD + 22 + row * 110, 180, 60 }; }

// Reiter System
static Rect pollBtn(int i) { return { 250 + i * 108, HEAD + 22, 100, 60 }; }
static const Rect R_UPD     = { 250, HEAD + 112, 540, 60 };
static const Rect R_PIC     = { 250, HEAD + 202, 540, 60 };
static const Rect R_FACT    = { 250, HEAD + 292, 300, 60 };

// Unterseite Helligkeit & Kontrast
static const int PIC_RAMP_Y = HEAD + 14, PIC_RAMP_H = 34, PIC_RAMP_G = 6, PIC_STEP = 24, PIC_X0 = 16;
static Rect contrastBtn(int i) { return { 250 + i * 136, HEAD + 272, 128, 60 }; }
static Rect brightBtn(int i)   { return { 250 + i * 108, HEAD + 356, 100, 60 }; }

// Tastatur (WLAN)
static const char* const KB_ROWS[3][5][11] = {
  { { "1","2","3","4","5","6","7","8","9","0", nullptr },
    { "q","w","e","r","t","z","u","i","o","p", nullptr },
    { "a","s","d","f","g","h","j","k","l","-", nullptr },
    { "#SHIFT|1.5","y","x","c","v","b","n","m","#BS|1.5", nullptr },
    { "#SYM|2",".","#SPACE|5","@","_", nullptr } },
  { { "1","2","3","4","5","6","7","8","9","0", nullptr },
    { "Q","W","E","R","T","Z","U","I","O","P", nullptr },
    { "A","S","D","F","G","H","J","K","L","-", nullptr },
    { "#SHIFT|1.5","Y","X","C","V","B","N","M","#BS|1.5", nullptr },
    { "#SYM|2",".","#SPACE|5","@","_", nullptr } },
  { { "ä","ö","ü","Ä","Ö","Ü","ß","€","§","°", nullptr },
    { "!","\"","#","$","%","&","/","(",")","=", nullptr },
    { "?","+","*","~","'",":",";","<",">","|", nullptr },
    { "#ABC|1.5","[","]","{","}","\\","^","´","#BS|1.5", nullptr },
    { "#ABC|2",",","#SPACE|5","@","_", nullptr } },
};
static const int KB_Y0 = FY0 + 3 * (FH + FG) + 8;
static const int KB_RH = (480 - KB_Y0 - M) / 5;
static const int KB_GAP = 6;

static bool kbKey(int r, int i, Rect& out, String& t) {
  const char* const* row = KB_ROWS[kbLayer][r];
  float total = 0;
  int n = 0;
  for (; row[n]; n++) {
    const char* bar = strchr(row[n], '|');
    total += bar ? atof(bar + 1) : 1.0f;
  }
  if (i >= n) return false;
  float unit = (800 - 2 * M) / total;
  float x = M;
  for (int k = 0; k < i; k++) {
    const char* bar = strchr(row[k], '|');
    x += unit * (bar ? atof(bar + 1) : 1.0f);
  }
  const char* bar = strchr(row[i], '|');
  float w = unit * (bar ? atof(bar + 1) : 1.0f);
  out = { (int)x + KB_GAP / 2, KB_Y0 + r * KB_RH, (int)w - KB_GAP, KB_RH - KB_GAP };
  t = bar ? String(row[i]).substring(0, bar - row[i]) : String(row[i]);
  return true;
}

static String keyLabel(const String& t) {
  if (t == "#SHIFT") return kbLayer == KB_UPPER ? "ABC" : "abc";
  if (t == "#BS")    return "<-";
  if (t == "#SYM")   return "äö!#";
  if (t == "#ABC")   return "abc";
  if (t == "#SPACE") return "Leerzeichen";
  return t;
}

// kWp / kWh als Text mit Punkt (Eingabe) <-> Zahl
static String numEdit(uint32_t milli) {
  if (milli == 0) return "";
  String s = String(milli / 1000.0f, 1);
  if (s.endsWith(".0")) s.remove(s.length() - 2);
  return s;
}
static bool parseKilo(const String& s, uint32_t& out) {   // "13.3" -> 13300
  String t = s;
  t.trim();
  t.replace(",", ".");
  if (t.length() == 0) { out = 0; return true; }
  char* end = nullptr;
  float v = strtof(t.c_str(), &end);
  if (!end || *end != 0 || v < 0 || v > 1000) return false;
  out = (uint32_t)lroundf(v * 1000.0f);
  return true;
}

static void openSettings(int tab) {
  edVal[0] = froniusHost;
  edVal[1] = numEdit(pvMaxW);
  edVal[2] = numEdit(battWh);
  edVal[3] = wifiSsid;
  edVal[4] = wifiPass;
  edVal[5] = ipAddr;
  edVal[6] = ipMask;
  edVal[7] = ipGw;
  edVal[8] = ipDns;
  edIpFixed = ipFixed;
  edPoll = pollMs;
  edAnim = animOn;
  picPage = false;
  edTab = tab;
  edField = tab == 0 ? 0 : (tab == 1 ? 3 : 5);
  edShowPass = false;
  kbLayer = KB_LOWER;
  wifiList = false;
  confirmAct = A_NONE;
  screen = SCR_SETTINGS;
  needRedraw = true;
}

static void scanWifi() {
  showMessage("Suche WLAN-Netze ...");
  NetLock lock;
  if (WiFi.status() != WL_CONNECTED) { WiFi.disconnect(); delay(300); }
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    WiFi.disconnect();
    delay(500);
    n = WiFi.scanNetworks();
  }
  scanN = 0;
  for (int pass = 0; pass < n && scanN < 8; pass++) {
    int best = -1;
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      bool dup = false;
      for (int k = 0; k < scanN; k++) if (scanSsid[k] == s) dup = true;
      if (dup) continue;
      if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best < 0) break;
    scanSsid[scanN] = WiFi.SSID(best);
    scanRssi[scanN] = WiFi.RSSI(best);
    scanN++;
  }
  WiFi.scanDelete();
  wifiList = true;
  needRedraw = true;
}

static Rect listRow(int i) { return { M, FY0 + 4 + i * 52, 800 - 2 * M, 46 }; }

static void drawWifiList() {
  useFont(fT);
  text("WLAN wählen", M + 6, HEAD / 2, C_ACC_BLUE, ML_DATUM);
  button(R_SAVE, "Abbrechen", false, C_BTN, C_TEXT, true);
  if (scanN == 0) {
    useFont(fB);
    text("Keine Netze gefunden", 400, 240, C_TEXT, MC_DATUM);
    return;
  }
  for (int i = 0; i < scanN; i++) {
    Rect r = listRow(i);
    bool cur = scanSsid[i] == edVal[3];
    canvas.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 8, cur ? C_BTN_SEL : C_BTN);
    useFont(fT);
    text(scanSsid[i], r.x + 14, r.y + r.h / 2, cur ? C_ACC_GRN : C_TEXT, ML_DATUM);
    int q = scanRssi[i];
    const char* stufe = q > -60 ? "stark" : (q > -72 ? "mittel" : "schwach");
    useFont(fS);
    text(String(stufe) + "  " + String(q) + " dBm", r.x + r.w - 14, r.y + r.h / 2, C_DIM, MR_DATUM);
  }
}

static void drawField(const Rect& r, int i, bool active) {
  canvas.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 8, C_TILE);
  canvas.drawRoundRect(r.x, r.y, r.w, r.h, 8, active ? C_ACC_GRN : C_BTN);
  String shown = edVal[i];
  if (isPassField(i) && !edShowPass) {
    shown = "";
    for (size_t k = 0; k < edVal[i].length(); k++)
      if (((uint8_t)edVal[i][k] & 0xC0) != 0x80) shown += "*";
  }
  if (active) shown += "_";
  useFont(fT);
  while (shown.length() > 1 && canvas.textWidth(shown) > r.w - 20) shown.remove(0, 1);
  text(shown, r.x + 10, r.y + r.h / 2, C_TEXT, ML_DATUM);
}

static void drawKeyboard() {
  for (int r = 0; r < 5; r++) {
    for (int i = 0;; i++) {
      Rect k;
      String t;
      if (!kbKey(r, i, k, t)) break;
      bool special = t.startsWith("#");
      bool active = (t == "#SHIFT" && kbLayer == KB_UPPER);
      canvas.fillSmoothRoundRect(k.x, k.y, k.w, k.h, 8, active ? C_BTN_SEL : (special ? C_TILE : C_BTN));
      useFont(special ? fS : fB);
      text(keyLabel(t), k.x + k.w / 2, k.y + k.h / 2, C_TEXT, MC_DATUM);
    }
  }
}

static void drawPad() {
  for (int i = 0; i < 12; i++) {
    Rect k = padKey(i);
    canvas.fillSmoothRoundRect(k.x, k.y, k.w, k.h, 10, i == 11 ? C_TILE : C_BTN);
    useFont(fB);
    text(PAD_KEYS[i], k.x + k.w / 2, k.y + k.h / 2, C_TEXT, MC_DATUM);
  }
}

static void drawSettingsBody();

static void drawSettings() {
  drawSettingsBody();
  if (millis() < toastUntil) {
    canvas.fillRect(0, 0, 800, HEAD, C_TILE);
    canvas.drawFastHLine(0, HEAD - 1, 800, toastColor);
    fitFont(toastText, 760, &fT, &fS);
    text(toastText, 400, HEAD / 2, toastColor, MC_DATUM);
  }
}

static void drawPicture() {
  useFont(fT);
  text("Helligkeit & Kontrast", M + 6, HEAD / 2, C_TEXT, ML_DATUM);
  button(R_SAVE, "Fertig", false, C_BTN, C_ACC_GRN, true);
  for (int row = 0; row < 4; row++) {
    int y = PIC_RAMP_Y + row * (PIC_RAMP_H + PIC_RAMP_G);
    for (int i = 0; i < 32; i++) {
      uint16_t r = (row == 0 || row == 1) ? i : 0;
      uint16_t g = (row == 0 || row == 2) ? (i * 63 + 15) / 31 : 0;
      uint16_t b = (row == 0 || row == 3) ? i : 0;
      canvas.fillRect(PIC_X0 + i * PIC_STEP, y, PIC_STEP, PIC_RAMP_H, (uint16_t)((r << 11) | (g << 5) | b));
    }
  }
  int yh = PIC_RAMP_Y + 4 * (PIC_RAMP_H + PIC_RAMP_G) + 14;
  useFont(fS);
  text("Jede Leiste soll gleichmäßig von Schwarz nach hell verlaufen.", M + 6, yh, C_DIM, ML_DATUM);
  text("Kontrast: Hintergrund dunkel, die linken Stufen noch erkennbar.", M + 6, yh + 26, C_DIM, ML_DATUM);
  useFont(fT);
  text("Kontrast", M + 6, contrastBtn(0).y + 30, C_TEXT, ML_DATUM);
  for (int i = 0; i < 4; i++) button(contrastBtn(i), CONTRAST_LABEL[i], contrastLvl == i);
  useFont(fT);
  text("Helligkeit", M + 6, brightBtn(0).y + 30, C_TEXT, ML_DATUM);
  for (int i = 0; i < 5; i++) button(brightBtn(i), String(BRIGHT_CHOICES[i]) + " %", brightPct == BRIGHT_CHOICES[i]);
}

static void pictureTap(int x, int y) {
  if (hit(R_SAVE, x, y)) { picPage = false; return; }
  for (int i = 0; i < 4; i++)
    if (hit(contrastBtn(i), x, y)) {
      contrastLvl = i;
      disp::setGamma(CONTRAST_GAMMA[i]);
      prefs.begin(NVS_NS, false);
      prefs.putUChar("contrast", contrastLvl);
      prefs.end();
      return;
    }
  for (int i = 0; i < 5; i++)
    if (hit(brightBtn(i), x, y)) {
      brightPct = BRIGHT_CHOICES[i];
      disp::setBrightness(brightDuty(brightPct));
      prefs.begin(NVS_NS, false);
      prefs.putUChar("bright", brightPct);
      prefs.end();
      return;
    }
}

static void drawSettingsBody() {
  if (picPage) { drawPicture(); return; }
  if (wifiList) { drawWifiList(); return; }
  for (int i = 0; i < NTAB; i++) button(tabRect(i), TAB_NAME[i], edTab == i, C_BTN, C_TEXT, true);
  button(R_CANCEL, "Abbrechen", false, C_BTN, C_TEXT, true);
  button(R_SAVE, "Speichern", false, C_BTN, C_ACC_GRN, true);

  if (edTab == 0) {
    for (int i = 0; i <= 2; i++) {
      Rect r = froField(i);
      fitFont(FIELD_LABEL[i], r.x - M - 10, &fS, &fXS);
      text(FIELD_LABEL[i], M + 4, r.y + r.h / 2, i == edField ? C_ACC_GRN : C_DIM, ML_DATUM);
      drawField(r, i, i == edField);
    }
    button(R_TEST, "Verbindung testen", false);
    useFont(fXS);
    int y = R_TEST.y + R_TEST.h + 22;
    text("PV-Leistung: Vollausschlag des PV-Rings (z. B. 13.3)", M + 4, y, C_DIM, ML_DATUM);
    text(battCapAuto() ? "Batterie: meldet " + fmt(storage.capWh / 1000.0f, 1) + " kWh – Feld kann leer bleiben"
                       : String("Batterie: nur nötig, wenn der Speicher nichts meldet"),
         M + 4, y + 22, C_DIM, ML_DATUM);
    text("Solar API am Wechselrichter einschalten!", M + 4, y + 44, C_DIM, ML_DATUM);
    drawPad();
  } else if (edTab == 1) {
    for (int i = 3; i <= 4; i++) {
      Rect r = fieldRect(i);
      useFont(fS);
      text(FIELD_LABEL[i], M + 6, r.y + r.h / 2, i == edField ? C_ACC_GRN : C_DIM, ML_DATUM);
      drawField(r, i, i == edField);
      if (isPassField(i)) button(sideBtn(i), edShowPass ? "Verbergen" : "Zeigen", edShowPass);
      if (i == 3) button(sideBtn(i), "Suchen", false);
    }
    String st = WiFi.status() == WL_CONNECTED
                  ? "Verbunden: " + WiFi.SSID() + "   " + String(WiFi.RSSI()) + " dBm"
                  : String("Nicht verbunden");
    fitFont(st, 800 - 2 * M - 12, &fT, &fS);
    text(st, M + 6, FY0 + 2 * (FH + FG) + FH / 2, WiFi.status() == WL_CONNECTED ? C_DIM : C_ACC_ORG, ML_DATUM);
    drawKeyboard();
  } else if (edTab == 2) {
    button(R_IP_AUTO, "Automatisch", !edIpFixed);
    button(R_IP_FIX, "Feste IP", edIpFixed);
    String cur[4] = { WiFi.localIP().toString(), WiFi.subnetMask().toString(),
                      WiFi.gatewayIP().toString(), WiFi.dnsIP().toString() };
    for (int i = 5; i <= 8; i++) {
      Rect r = ipField(i);
      useFont(fT);
      text(FIELD_LABEL[i], M + 4, r.y + r.h / 2, edIpFixed && i == edField ? C_ACC_GRN : C_DIM, ML_DATUM);
      if (edIpFixed) {
        drawField(r, i, i == edField);
      } else {
        canvas.drawRoundRect(r.x, r.y, r.w, r.h, 8, C_BTN);
        useFont(fT);
        text(cur[i - 5], r.x + 10, r.y + r.h / 2, C_DIM, ML_DATUM);
      }
    }
    useFont(fS);
    text(edIpFixed ? "Feste IP außerhalb des DHCP-Bereichs wählen" : "Aktuelle Werte vom Router (DHCP)",
         M + 4, 480 - M - 4, C_DIM, BL_DATUM);
    if (edIpFixed) drawPad();
  } else if (edTab == 3) {
    // Ausrichtung wirkt sofort (wird direkt gespeichert)
    useFont(fT);
    text("Ausrichtung", M + 6, optBtn(0, 0).y + 16, C_TEXT, ML_DATUM);
    useFont(fXS);
    text("USB-Anschluss links", M + 6, optBtn(0, 0).y + 44, C_DIM, ML_DATUM);
    text("oder rechts", M + 6, optBtn(0, 0).y + 64, C_DIM, ML_DATUM);
    button(optBtn(0, 0), "Normal", !rot180);
    button(optBtn(0, 1), "Gedreht 180°", rot180);
    useFont(fT);
    text("Energiefluss", M + 6, optBtn(1, 0).y + 16, C_TEXT, ML_DATUM);
    useFont(fXS);
    text("Flussrichtung", M + 6, optBtn(1, 0).y + 44, C_DIM, ML_DATUM);
    button(optBtn(1, 0), "Pfeile", !edAnim);
    button(optBtn(1, 1), "Laufende Punkte", edAnim);
    useFont(fXS);
    text("Ausrichtung gilt sofort – Bild und Touch drehen sich mit.", M + 6, 480 - M - 10, C_DIM, BL_DATUM);
  } else {
    useFont(fT);
    text("Abfrage alle", M + 6, pollBtn(0).y + 30, C_TEXT, ML_DATUM);
    for (int i = 0; i < 5; i++) button(pollBtn(i), POLL_LABEL[i], edPoll == POLL_CHOICES[i]);
    useFont(fT);
    text("Firmware " FW_VERSION, M + 6, R_UPD.y + 16, C_TEXT, ML_DATUM);
    useFont(fS);
    text("CrowPanel " BOARD_NAME, M + 6, R_UPD.y + 46, C_DIM, ML_DATUM);
    if (updateVersion.length())
      button(R_UPD, "Update " + updateVersion + " installieren", false, C_BTN_SEL, C_ACC_GRN);
    else
      button(R_UPD, "Nach Update suchen", false);
    useFont(fT);
    text("Bild", M + 6, R_PIC.y + 30, C_TEXT, ML_DATUM);
    button(R_PIC, "Helligkeit & Kontrast ...", false);
    useFont(fT);
    text("Zurücksetzen", M + 6, R_FACT.y + 30, C_TEXT, ML_DATUM);
    button(R_FACT, "Werkseinstellungen", false, C_BTN_RED);
    String info = WiFi.status() == WL_CONNECTED
                    ? "Display-IP " + WiFi.localIP().toString() + "   WLAN " + WiFi.SSID() + "   " +
                        String(WiFi.RSSI()) + " dBm"
                    : String("WLAN nicht verbunden");
    fitFont(info, 800 - 2 * M, &fS);
    text(info, M + 6, 480 - M - 4, C_DIM, BL_DATUM);
  }
}

// ---------------------------------------------------------------
// Einstellungen speichern
// ---------------------------------------------------------------
static bool parseIp(const String& s, IPAddress& out) { return s.length() && out.fromString(s); }

static uint32_t ipU32(const IPAddress& a) {
  return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
}

static bool validMask(const IPAddress& m) {
  uint32_t v = ipU32(m);
  if (v == 0) return false;
  uint32_t inv = ~v;
  return (inv & (inv + 1)) == 0;
}

static bool connectWifi(uint32_t timeoutMs);

static void setRotation(bool on) {
  if (on == rot180) return;
  rot180 = on;
  disp::setRotate180(on);
  prefs.begin(NVS_NS, false);
  prefs.putBool("rot180", rot180);
  prefs.end();
}

static void saveSettings() {
  for (int i = 0; i < NF; i++) if (!isPassField(i)) edVal[i].trim();
  IPAddress hostIp;
  if (!parseIp(edVal[0], hostIp)) { edTab = 0; edField = 0; toast("IP des Wechselrichters fehlt oder ungültig", C_ACC_ORG); return; }
  uint32_t pv, bat;
  if (!parseKilo(edVal[1], pv)) { edTab = 0; edField = 1; toast("PV-Leistung ungültig (z. B. 13.3)", C_ACC_ORG); return; }
  if (!parseKilo(edVal[2], bat)) { edTab = 0; edField = 2; toast("Batterie ungültig (z. B. 12.8)", C_ACC_ORG); return; }
  if (edVal[3].length() == 0) { edTab = 1; edField = 3; toast("WLAN-Name fehlt", C_ACC_ORG); return; }
  if (edIpFixed) {
    IPAddress ip, mask, gw, dns;
    if (!parseIp(edVal[5], ip)) { edTab = 2; edField = 5; toast("IP-Adresse ungültig", C_ACC_ORG); return; }
    if (!parseIp(edVal[6], mask) || !validMask(mask)) { edTab = 2; edField = 6; toast("Subnetz ungültig", C_ACC_ORG); return; }
    if (!parseIp(edVal[7], gw)) { edTab = 2; edField = 7; toast("Gateway ungültig", C_ACC_ORG); return; }
    if (edVal[8].length() == 0) edVal[8] = edVal[7];
    if (!parseIp(edVal[8], dns)) { edTab = 2; edField = 8; toast("DNS ungültig", C_ACC_ORG); return; }
    if ((ipU32(ip) & ipU32(mask)) != (ipU32(gw) & ipU32(mask))) {
      edTab = 2; edField = 7; toast("Gateway liegt nicht im Subnetz", C_ACC_ORG); return;
    }
  }
  static String ipWarnedFor;
  if (edIpFixed && edVal[3] != wifiSsid && ipWarnedFor != edVal[3]) {
    ipWarnedFor = edVal[3];
    edTab = 2;
    edField = 5;
    toast("Feste IP aktiv – passt sie zum neuen WLAN? Dann nochmal Speichern", C_ACC_ORG, 10000);
    return;
  }

  bool netChanged = edVal[3] != wifiSsid || edVal[4] != wifiPass || edIpFixed != ipFixed ||
                    (edIpFixed && (edVal[5] != ipAddr || edVal[6] != ipMask || edVal[7] != ipGw ||
                                   edVal[8] != ipDns));
  bool hostChanged = edVal[0] != froniusHost;
  if (dataMx) xSemaphoreTake(dataMx, portMAX_DELAY);
  froniusHost = edVal[0];
  if (dataMx) xSemaphoreGive(dataMx);
  pvMaxW = pv;
  battWh = bat;
  wifiSsid = edVal[3];
  wifiPass = edVal[4];
  ipFixed  = edIpFixed;
  ipAddr = edVal[5];
  ipMask = edVal[6];
  ipGw   = edVal[7];
  ipDns  = edVal[8];
  pollMs = edPoll;
  animOn = edAnim;

  prefs.begin(NVS_NS, false);
  prefs.putString("host", froniusHost);
  prefs.putUInt("pvmax", pvMaxW);
  prefs.putUInt("battwh", battWh);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("wpass", wifiPass);
  prefs.putBool("ipfix", ipFixed);
  prefs.putString("ip", ipAddr);
  prefs.putString("mask", ipMask);
  prefs.putString("gw", ipGw);
  prefs.putString("dns", ipDns);
  prefs.putUInt("poll", pollMs);
  prefs.putBool("anim2", animOn);
  prefs.end();
  Serial.printf("Einstellungen gespeichert: Fronius %s, WLAN %s, IP %s\n", froniusHost.c_str(),
                wifiSsid.c_str(), ipFixed ? ipAddr.c_str() : "DHCP");

  if (netChanged) {
    daySave();
    showMessage("Gespeichert – Neustart ...");
    delay(1200);
    ESP.restart();
  }
  if (hostChanged) {                    // anderer Wechselrichter: Tageswerte neu beginnen
    dayS = DayStore();
    lastIntegMs = 0;
    daySave();
  }

  screen = SCR_FLOW;
  if (WiFi.status() != WL_CONNECTED) connectWifi(15000);
  showMessage("Teste Verbindung zum Wechselrichter ...");
  if (pollFronius()) {
    toast("Gespeichert – Wechselrichter erreichbar", C_ACC_GRN, 5000);
  } else {
    failCount = 3;
    toast("Gespeichert – Wechselrichter NICHT erreichbar", C_ACC_ORG, 8000);
  }
  needRedraw = true;
}

static void keyInput(const String& t) {
  String& v = edVal[edField];
  if (t == "#SHIFT")      kbLayer = (kbLayer == KB_UPPER) ? KB_LOWER : KB_UPPER;
  else if (t == "#SYM")   kbLayer = KB_SYM;
  else if (t == "#ABC")   kbLayer = KB_LOWER;
  else if (t == "#BS" || t == "<-") {
    while (v.length() && ((uint8_t)v[v.length() - 1] & 0xC0) == 0x80) v.remove(v.length() - 1);
    if (v.length()) v.remove(v.length() - 1);
  }
  else if (t == "#SPACE") { if (v.length() < 63) v += ' '; }
  else {
    if (v.length() < 63) v += t;
    if (kbLayer == KB_UPPER) kbLayer = KB_LOWER;
  }
}

static void testConnection() {
  String h = edVal[0];
  h.trim();
  showMessage("Prüfe Verbindung ...");
  FlowData f;
  bool ok;
  {
    NetLock lock;
    ok = readPowerFlow(h, f);
  }
  if (ok) {
    String s = "Wechselrichter erreichbar – PV " + powerText(f.pPv);
    if (f.hasBatt && !isnan(f.soc)) s += ", Batterie " + String((int)lroundf(f.soc)) + " %";
    toast(s, C_ACC_GRN, 8000);
  } else if (lastHttp == 404) {
    toast("Gerät antwortet, aber keine Solar API (HTTP 404) – Solar API einschalten?", C_ACC_ORG, 12000);
  } else if (lastHttp > 0) {
    toast("Wechselrichter antwortet mit HTTP " + String(lastHttp), C_ACC_ORG, 8000);
  } else {
    toast("Wechselrichter nicht erreichbar – IP prüfen", C_ACC_ORG, 8000);
  }
}

static void settingsTap(int x, int y) {
  needRedraw = true;
  if (picPage) { pictureTap(x, y); return; }
  if (wifiList) {
    if (hit(R_SAVE, x, y)) { wifiList = false; return; }
    for (int i = 0; i < scanN; i++) {
      if (hit(listRow(i), x, y)) {
        edVal[3] = scanSsid[i];
        if (edVal[3] != wifiSsid) edVal[4] = "";
        edField = 4;
        wifiList = false;
        return;
      }
    }
    return;
  }
  for (int i = 0; i < NTAB; i++) {
    if (hit(tabRect(i), x, y)) {
      edTab = i;
      edField = i == 0 ? 0 : (i == 1 ? 3 : 5);
      return;
    }
  }
  if (hit(R_CANCEL, x, y)) { screen = SCR_FLOW; pollNow = true; return; }
  if (hit(R_SAVE, x, y))   { saveSettings(); return; }

  if (edTab == 0 || (edTab == 2 && edIpFixed)) {
    if (edTab == 0) {
      for (int i = 0; i <= 2; i++) if (hit(froField(i), x, y)) { edField = i; return; }
      if (hit(R_TEST, x, y)) { testConnection(); return; }
    }
    for (int i = 0; i < 12; i++) {
      if (hit(padKey(i), x, y)) {
        if (isPadField(edField) && (edVal[edField].length() < 15 || i == 11)) keyInput(PAD_KEYS[i]);
        return;
      }
    }
  }
  if (edTab == 1) {
    for (int i = 3; i <= 4; i++) {
      if (hasSideBtn(i) && hit(sideBtn(i), x, y)) {
        if (i == 3) scanWifi();
        else edShowPass = !edShowPass;
        return;
      }
      if (hit(fieldRect(i), x, y)) { edField = i; return; }
    }
    for (int r = 0; r < 5; r++) {
      for (int i = 0;; i++) {
        Rect k;
        String t;
        if (!kbKey(r, i, k, t)) break;
        if (hit(k, x, y)) { keyInput(t); return; }
      }
    }
  } else if (edTab == 2) {
    if (hit(R_IP_AUTO, x, y)) { edIpFixed = false; return; }
    if (hit(R_IP_FIX, x, y)) {
      if (!edIpFixed && WiFi.status() == WL_CONNECTED) {
        if (edVal[5].length() == 0) edVal[5] = WiFi.localIP().toString();
        if (edVal[6].length() == 0) edVal[6] = WiFi.subnetMask().toString();
        if (edVal[7].length() == 0) edVal[7] = WiFi.gatewayIP().toString();
        if (edVal[8].length() == 0) edVal[8] = WiFi.dnsIP().toString();
      }
      edIpFixed = true;
      edField = 5;
      return;
    }
    if (!edIpFixed) return;
    for (int i = 5; i <= 8; i++) if (hit(ipField(i), x, y)) { edField = i; return; }
  } else if (edTab == 3) {
    if (hit(optBtn(0, 0), x, y)) { setRotation(false); return; }
    if (hit(optBtn(0, 1), x, y)) { setRotation(true); return; }
    if (hit(optBtn(1, 0), x, y)) { edAnim = false; return; }
    if (hit(optBtn(1, 1), x, y)) { edAnim = true; return; }
  } else if (edTab == 4) {
    for (int i = 0; i < 5; i++) if (hit(pollBtn(i), x, y)) { edPoll = POLL_CHOICES[i]; return; }
    if (hit(R_PIC, x, y)) { picPage = true; return; }
    if (hit(R_UPD, x, y)) {
      if (updateVersion.length()) {
        askConfirm(A_OTA, "Update " + updateVersion + " installieren?", "Das Display startet danach neu.");
      } else {
        showMessage("Suche nach Updates ...");
        if (!checkForUpdate()) toast("Keine Update-Info: " + updateErr, C_ACC_ORG, 15000);
        else if (updateVersion.length() == 0) toast("Firmware ist aktuell", C_ACC_GRN, 4000);
        lastUpdateCheck = millis();
        updateWaitMs = UPDATE_CHECK_EVERY_MS;
      }
      return;
    }
    if (hit(R_FACT, x, y)) {
      askConfirm(A_FACTORY, "Alle Einstellungen löschen?", "WLAN und Wechselrichter müssen neu eingerichtet werden.");
      return;
    }
  }
}

// ---------------------------------------------------------------
// Zeichnen (ganzer Bildschirm)
// ---------------------------------------------------------------
static void draw() {
  canvas.fillScreen(C_BG);
  if (screen == SCR_FLOW) {
    drawFlowHeader();
    NetErr err = netError();
    if (err != NE_NONE) {
      for (FlowLine& l : lines) l.active = false;   // keine laufenden Punkte ueber dem Fehlerbild
      drawErrorPanel(err);
    } else {
      drawFlow();
      drawTodayPanel();
    }
  } else if (screen == SCR_DETAIL) {
    drawDetail();
  } else {
    drawSettings();
  }
  drawDialog();
  // Grundbild fuer die laufenden Punkte merken (ohne Punkte)
  baseValid = false;
  if (screen == SCR_FLOW && confirmAct == A_NONE && base.getBuffer()) {
    memcpy(base.getBuffer(), canvas.getBuffer(), disp::W * disp::H * 2);
    baseValid = true;
    for (FlowLine& l : lines) if (l.active && animOn) drawDotsOn(l);
  }
  if (screen == SCR_FLOW && confirmAct == A_NONE && !animOn) {
    for (FlowLine& l : lines) if (l.active) drawArrowsOn(l);
  }
  pushAll();
  needRedraw = false;
}

// Ein Animationsbild: nur die Bereiche der aktiven Leitungen erneuern
static void animFrame(float dtS) {
  if (!baseValid) return;
  uint16_t* cb = (uint16_t*)canvas.getBuffer();
  const uint16_t* bb = (const uint16_t*)base.getBuffer();
  for (FlowLine& l : lines) {
    if (!l.active) continue;
    l.phase = fmodf(l.phase + dtS * l.speed / DOTS, 1.0f);   // ein Punktabstand = 1/DOTS
    Rect r = lineBox(l);
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > disp::W) r.w = disp::W - r.x;
    if (r.y + r.h > disp::H) r.h = disp::H - r.y;
    for (int row = r.y; row < r.y + r.h; row++)
      memcpy(cb + row * disp::W + r.x, bb + row * disp::W + r.x, r.w * 2);
    drawDotsOn(l);
    pushRegion(r);
  }
}

// ---------------------------------------------------------------
// Bestaetigte Aktionen
// ---------------------------------------------------------------
static void runAction(Action a) {
  switch (a) {
    case A_OTA: doOta(); break;
    case A_FACTORY:
      prefs.begin(NVS_NS, false);
      prefs.clear();
      prefs.end();
      showMessage("Werkseinstellungen – Neustart ...");
      delay(1200);
      ESP.restart();
      break;
    default: break;
  }
}

// ---------------------------------------------------------------
// Beruehrung
// ---------------------------------------------------------------
static int nodeAt(int x, int y) {
  for (int i = 0; i < 4; i++) {
    int dx = x - NODE_X[i], dy = y - NODE_Y[i];
    if (dx * dx + dy * dy <= (R_OUT + 16) * (R_OUT + 16)) return i;
  }
  return -1;
}

static void onTap(int x, int y) {
  Serial.printf("Touch %d/%d\n", x, y);
  if (confirmAct != A_NONE) {
    if (hit(R_YES, x, y)) {
      Action a = confirmAct;
      confirmAct = A_NONE;
      draw();
      runAction(a);
    } else if (hit(R_NO, x, y)) {
      confirmAct = A_NONE;
    }
    needRedraw = true;
    return;
  }
  switch (screen) {
    case SCR_SETTINGS: settingsTap(x, y); return;
    case SCR_DETAIL:              // Tipp irgendwohin: zurueck zum Energiefluss
      screen = SCR_FLOW;
      needRedraw = true;
      return;
    default: break;
  }
  if (hit(R_GEAR, x, y)) { openSettings(netError() == NE_WIFI ? 1 : 0); return; }
  if (netError() != NE_NONE) return;   // Fehlerbild: nichts weiter bedienbar
  int n = nodeAt(x, y);
  if (n >= 0) {
    detailSel = n;
    screen = SCR_DETAIL;
  } else if (hit(R_TODAY, x, y)) {
    detailSel = N_LOAD;
    screen = SCR_DETAIL;
  }
  needRedraw = true;
}

static void handleTouch() {
  static bool wasDown = false;
  static uint32_t downSince = 0;
  static int32_t downX = 0, downY = 0, lastX = 0, lastY = 0;
  static bool longDone = false;
  static bool pendingTap = false;   // Startseite: Tipp erst beim Loslassen (Kopfzeile lange halten)
  int32_t tx, ty;
  bool down = disp::getTouch(&tx, &ty);
  if (down) { lastX = tx; lastY = ty; }

  // Kopfzeile der Startseite 2 s gedrueckt halten -> Einstellungen
  if (down && wasDown && !longDone && screen == SCR_FLOW && confirmAct == A_NONE && !dimmed &&
      downY < HEAD && abs((int)(lastX - downX)) < 40 && millis() - downSince > 2000) {
    longDone = true;
    pendingTap = false;
    lastTouchMs = millis();
    openSettings(0);
  }

  if (down && !wasDown) {
    downSince = millis();
    downX = tx;
    downY = ty;
    longDone = false;
    pendingTap = false;
    lastTouchMs = millis();
    if (dimmed) {
      dimmed = false;
      disp::setBrightness(brightDuty(brightPct));
    } else if (screen == SCR_FLOW && confirmAct == A_NONE) {
      pendingTap = true;
    } else {
      onTap(tx, ty);
    }
  }

  if (!down && wasDown && pendingTap) {
    pendingTap = false;
    lastTouchMs = millis();
    int dx = (int)(lastX - downX), dy = (int)(lastY - downY);
    if (abs(dx) < 40 && abs(dy) < 40) onTap(downX, downY);
  }
  wasDown = down;
}

// ---------------------------------------------------------------
// WLAN
// ---------------------------------------------------------------
static bool connectWifi(uint32_t timeoutMs) {
  if (wifiSsid.length() == 0) return false;
  WiFi.disconnect();
  delay(100);
  if (ipFixed) {
    IPAddress ip, mask, gw, dns;
    if (parseIp(ipAddr, ip) && parseIp(ipMask, mask) && parseIp(ipGw, gw)) {
      if (!parseIp(ipDns, dns)) dns = gw;
      WiFi.config(ip, gw, mask, dns);
    }
  }
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(250);
  Serial.printf("WLAN: %s  IP: %s\n", WiFi.status() == WL_CONNECTED ? "verbunden" : "FEHLER",
                WiFi.localIP().toString().c_str());
  return WiFi.status() == WL_CONNECTED;
}

// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Fronius-Display " FW_VERSION " (" BOARD_NAME ") startet");

  loadSettings();
  dayLoad();
  Serial.printf("Fronius: %s  Abfrage: %u ms  PV %u W  Batterie %u Wh\n", froniusHost.c_str(), (unsigned)pollMs,
                (unsigned)pvMaxW, (unsigned)battWh);

  disp::backlightBegin();
  disp::setBrightness(0);
  disp::setGamma(CONTRAST_GAMMA[contrastLvl]);
  disp::setRotate180(rot180);
  if (!disp::begin()) Serial.println("Display konnte nicht gestartet werden");
  disp::touchBegin();
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(disp::W, disp::H)) Serial.println("Zwischenbild konnte nicht angelegt werden (PSRAM?)");
  base.setPsram(true);
  base.setColorDepth(16);
  if (!base.createSprite(disp::W, disp::H)) Serial.println("Grundbild konnte nicht angelegt werden – ohne Animation");
  disp::setBrightness(brightDuty(brightPct));

  fXS.begin(FONT_XS, sizeof(FONT_XS));
  fS.begin(FONT_S, sizeof(FONT_S));
  fT.begin(FONT_T, sizeof(FONT_T));
  fB.begin(FONT_B, sizeof(FONT_B));
  fM.begin(FONT_M, sizeof(FONT_M));
  fL.begin(FONT_L, sizeof(FONT_L));
  fXL.begin(FONT_XL, sizeof(FONT_XL));

  showMessage("Verbinde mit WLAN ...");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  connectWifi(20000);
  // Uhrzeit fuer Kopfzeile und Tageswechsel; Router zuerst, dann oeffentliche Server.
  // SNTP merkt sich nur den Zeiger auf den Namen – daher ein dauerhafter Puffer.
  static char ntpGw[16];
  strlcpy(ntpGw, WiFi.status() == WL_CONNECTED ? WiFi.gatewayIP().toString().c_str() : "pool.ntp.org",
          sizeof(ntpGw));
  configTzTime(TIMEZONE, ntpGw, "pool.ntp.org", "time.google.com");

  // Abfrage-Task auf Kern 0 (loop() laeuft auf Kern 1)
  netMx = xSemaphoreCreateMutex();
  dataMx = xSemaphoreCreateMutex();
  pollNow = true;
  xTaskCreatePinnedToCore(pollTask, "poll", 12288, nullptr, 1, nullptr, 0);

  lastTouchMs = millis();
  lastPoll = millis();
  if (wifiSsid.length() == 0) {      // noch nie eingerichtet
    openSettings(1);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    // Eingerichtet, aber (noch) kein Netz – z. B. Router nach Stromausfall langsamer.
    // Nicht in den Einstellungen haengen bleiben: Fehlerbild zeigen und weiter verbinden.
    wifiLostSince = millis() - WIFI_GRACE_MS;   // Symbol sofort zeigen
    needRedraw = true;
    return;
  }
  if (froniusHost.length() == 0) {
    openSettings(0);
    return;
  }
  showMessage("Lese Wechselrichter ...");
  if (fwUpdatedFrom.length())
    toast("Firmware aktualisiert: " + fwUpdatedFrom + " > " FW_VERSION, C_ACC_GRN, 15000);
}

void loop() {
  handleTouch();

  // Abfrage laeuft im Hintergrund; hier nur die Ergebnisse uebernehmen.
  // In den Einstellungen pausiert sie (WLAN-Suche, Tests).
  pollPaused = (screen == SCR_SETTINGS);
  if (pendingNew) {
    xSemaphoreTake(dataMx, portMAX_DELAY);
    PollResult r = pending;
    pendingNew = false;
    xSemaphoreGive(dataMx);
    applyPoll(r);
  }

  // Kopfzeile (Uhrzeit, WLAN) regelmaessig auffrischen
  static uint32_t lastHdr = 0;
  if (screen == SCR_FLOW && millis() - lastHdr > 15000) {
    lastHdr = millis();
    needRedraw = true;
  }

  // Update-Pruefung einmal am Tag, nur auf der Startseite
  if (screen == SCR_FLOW && millis() - lastUpdateCheck >= updateWaitMs) {
    lastUpdateCheck = millis();
    updateWaitMs = checkForUpdate() ? UPDATE_CHECK_EVERY_MS : UPDATE_RETRY_MS;
  }

  static bool toastShown = false;
  if (millis() < toastUntil) toastShown = true;
  else if (toastShown) { toastShown = false; needRedraw = true; }

  // Stoerung beginnt oder endet: sofort neu zeichnen
  static NetErr shownErr = NE_NONE;
  NetErr err = netError();
  if (err != shownErr) {
    shownErr = err;
    needRedraw = true;
  }

  // WLAN weg: selbst neu verbinden (nicht waehrend der Einstellungen – dort laeuft ggf. die Netzsuche).
  // Nach der Rueckkehr sofort abfragen.
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  static bool wifiWasOk = true;
  if (wifiOk && !wifiWasOk) { failCount = 0; pollNow = true; }
  wifiWasOk = wifiOk;
  static uint32_t lastWifiTry = 0;
  if (!wifiOk && wifiSsid.length() && screen != SCR_SETTINGS && millis() - lastWifiTry >= WIFI_RETRY_MS) {
    lastWifiTry = millis();
    Serial.println("WLAN getrennt – neuer Verbindungsversuch");
    NetLock lock;                                     // nicht mitten in eine Abfrage hinein
    WiFi.disconnect();
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());  // kehrt sofort zurueck
  }

  // Detail- und Einstellungsseiten schliessen sich selbst
  bool unfertig = froniusHost.length() == 0 || wifiSsid.length() == 0;
  if (screen != SCR_FLOW && !(screen == SCR_SETTINGS && unfertig) && millis() - lastTouchMs > PAGE_TIMEOUT_MS) {
    screen = SCR_FLOW;
    confirmAct = A_NONE;
    wifiList = false;
    picPage = false;
    needRedraw = true;
  }

  if (!dimmed && millis() - lastTouchMs > DIM_AFTER_MS) {
    dimmed = true;
    disp::setBrightness(dimDuty());
  }

  if (needRedraw) draw();

  // laufende Punkte
  static uint32_t lastFrame = 0;
  uint32_t now = millis();
  if (animOn && screen == SCR_FLOW && confirmAct == A_NONE && now - lastFrame >= ANIM_FRAME_MS) {
    float dt = lastFrame ? (now - lastFrame) / 1000.0f : 0;
    if (dt > 0.2f) dt = 0.2f;
    lastFrame = now;
    animFrame(dt);
  }
  delay(5);
}
