/*
 * MiniWX Receiver - SmallTV-Ultra (ESP8266 + ST7789 240x240)
 * Minimalist layout: large clock on top + clean metrics below.
 * -------------------------------------------------------------------
 * The networking part (WiFi STA/AP, config portal, MiniWX polling, parsing)
 * is unchanged. Only the display layer was reworked:
 *   - static layout drawn once (no flicker)
 *   - the clock is redrawn only when the minute changes
 *   - values are redrawn only on each poll, in their own cell
 */

//**** APPLICATION VERSION (single source: device footer + web footer) ****
#define APP_VERSION "v3.21"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <time.h>

#define BL_PIN 5   // backlight, ACTIVE LOW
const char* CONFIG_PATH = "/config.json";

struct DeviceConfig {
  String ssid;
  String pass;
  String bmeHost;
  String bmePath;
  String ntpServer;
  int8_t tzOffsetHours;
  String location;
  int8_t lang;            // 0=RO, 1=EN, 2=HU
  int16_t slideSeconds;   // interval for graph slides (s); 0 = no graphs
  int16_t mainSeconds;    // how long the main page is shown (s)
  int8_t graphWindow;     // 0=1h, 1=12h, 2=24h, 3=1 week
  int8_t brightDay;       // normal brightness 5..100
  int8_t brightNight;     // reduced brightness (schedule) 5..100
  bool   schedEnabled;    // brightness schedule enabled?
  int8_t schedStart;      // start hour (0..23)
  int8_t schedEnd;        // end hour (0..23)
  // --- Weather data source ---
  int8_t  dataSource;     // 0 = MiniWX local (/jquery), 1 = APRS-IS
  String  aprsCall0;      // station 1 callsign (e.g. YO7ZRO-13)
  String  aprsCall1;      // station 2 callsign
  String  aprsCall2;      // station 3 callsign
  int8_t  aprsActive;     // which station is shown on screen (0..2)
  String  aprsLogin;      // APRS-IS login callsign (read-only, pass -1)
  String  aprsServer;     // APRS-IS server
  int16_t aprsPort;       // APRS-IS port
  int16_t measSeconds;    // weather read/poll interval (seconds) - same as the station
  String  aprsLoc0;       // displayed name for station 1 (empty -> "Station 1")
  String  aprsLoc1;       // displayed name for station 2
  String  aprsLoc2;       // displayed name for station 3
  String  aprsTempOff0;   // BME280 temperature correction for station 1 (deg C, e.g. -2.0)
  String  aprsTempOff1;   // BME280 temperature correction for station 2
  String  aprsTempOff2;   // BME280 temperature correction for station 3
  bool    logEnabled;     // web console log (RAM ring buffer) enabled?
};


DeviceConfig config = {
  "", "", "192.168.88.191", "jquery", "pool.ntp.org", 0, "", 0, 0, 60, 0, 100, 20, false, 21, 6,
  /*dataSource*/ 0, /*aprsCall0*/ "", /*aprsCall1*/ "", /*aprsCall2*/ "", /*aprsActive*/ 0,
  /*aprsLogin*/ "", /*aprsServer*/ "rotate.aprs2.net", /*aprsPort*/ 14580, /*measSeconds*/ 120, /*aprsLoc0*/ "", /*aprsLoc1*/ "", /*aprsLoc2*/ "", /*aprsTempOff0*/ "0", /*aprsTempOff1*/ "0", /*aprsTempOff2*/ "0", /*logEnabled*/ false
};

TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer server(80);
bool configMode = false;

String weatherValues[9] = {"--","--","--","--","--","--","--","--","--"};
bool weatherValid = false;     // last fetch succeeded (fresh data)
bool hasEverData  = false;     // received valid data at least once

// ===== Multi-resolution history for graphs (epoch timestamps) =====
struct Sample { uint32_t t; float temp; float hum; float pres; };

#define MAXSTA 3                  // max 3 stations
#define N_FINE 30                 // every 2s  -> ~1 min
#define N_MIN  120                // every 60s -> ~2 h
#define N_HOUR 168                // every 1h  -> ~7 days (for the 1-week window)
#define HIST_FILE "/hist.bin"
#define HIST_SAVE_MS 300000UL     // save fine+min to flash every ~5 min (spares the flash)
#define HOUR_FILE  "/hour.bin"      // hourly tier (7 days) - ring in FLASH
#define HOUR_MAGIC 0xB3

Sample bufFine[MAXSTA][N_FINE]; int nFine[MAXSTA] = {0,0,0};
Sample bufMin[MAXSTA][N_MIN];   int nMin[MAXSTA]  = {0,0,0};
// the hourly tier (7 days) is NOT kept in RAM -> it lives in flash (/hour.bin). See hourAppend()/hourAddToSeries().
uint32_t lastMinT[MAXSTA] = {0,0,0}, lastHourT[MAXSTA] = {0,0,0};
uint16_t hourHead[MAXSTA] = {0,0,0};      // write index in the flash hourly ring
unsigned long lastHistSaveMs = 0;
int    dispSta = 0;               // station currently shown on the main page
String curLocationLabel = "";     // location label for the displayed station

// Selectable graph windows (seconds)
const uint32_t GRAPH_WINDOWS[4] = {3600, 43200, 86400, 604800};   // 1h, 12h, 24h, 1 week

// Temporary series for drawing
struct Pt { uint32_t t; float v; };
Pt series[N_FINE + N_MIN + N_HOUR];
int seriesN = 0;

// Slideshow
int   currentSlide = 0;                 // 0=main, 1=temp, 2=humidity, 3=pressure
unsigned long lastSlideMs = 0;
bool  slideDrawn = false;
int   lastBrightMin = -2;               // for brightness recompute on minute change

/*---------------- Palette ----------------*/
#define C_BG    TFT_BLACK
#define C_VALUE TFT_WHITE
uint16_t C_ACCENT, C_LABEL, C_UNIT, C_DIV;
uint16_t C_STALE;               // neutral grey for stale data (server unavailable)
uint16_t C_TCOLD, C_TSLIGHT, C_TCOOL, C_THEATMOD, C_THEATSTR, C_THEAT;   // UTCI scale
uint16_t C_STA[3];                                                       // per-station colors (graphs)

void initPalette() {
  C_ACCENT = tft.color565(255, 179, 71);   // orange (matches the case)
  C_LABEL  = tft.color565(154, 164, 178);  // blue-grey (labels)
  C_UNIT   = tft.color565(107, 114, 128);  // subtle grey (units / subtext)
  C_DIV    = tft.color565(42, 46, 55);      // separator line
  C_STALE  = tft.color565(96, 100, 112);    // dark grey for stale data
  // Scale based on UTCI categories (felt temperature) used by meteorologists,
  // with the comfort zone narrowed to 18..26 C
  C_TCOLD    = tft.color565(60, 120, 255);   // < 0  C   -> cold stress (blue)
  C_TSLIGHT  = tft.color565(90, 200, 235);   // 0..9 C   -> slight cold (cyan)
  C_TCOOL    = tft.color565(120, 220, 200);  // 9..18 C  -> cool (turquoise)
  C_THEATMOD = tft.color565(255, 190, 60);   // 26..32 C -> moderate heat (amber)
  C_THEATSTR = tft.color565(255, 140, 40);   // 32..38 C -> strong heat (orange)
  C_THEAT    = tft.color565(255, 70, 55);    // >= 38 C  -> very strong / extreme (red)
  C_STA[0] = C_ACCENT;                       // station 1 - orange
  C_STA[1] = tft.color565(90, 200, 235);     // station 2 - cyan
  C_STA[2] = tft.color565(120, 235, 120);    // station 3 - green
  // 18..26 C = comfort -> stays white (C_VALUE)
}

/*---------------- Layout (240x240 screen coordinates) ----------------*/
static const int LEFT_X   = 8;             // left margin (location, clock, subtitle)
static const int LOC_Y    = 6;
static const int CLK_TOP  = 30;            // clock, top-left aligned (font 7 = 48px)
static const int SUB_Y    = 84;
static const int DIV_Y    = 104;
static const int ROW0_Y   = 116;
static const int ROW_STEP = 23;
static const int PAD_L    = 18;
static const int PAD_R    = 222;
static const int VAL_X0   = 120;           // left edge of the value cell

// Character area (right of the clock)
static const int CHAR_CX  = 192;           // center X
static const int CHAR_CY  = 50;            // center Y (raised so it isn't clipped at the bottom)
static const int CHAR_R   = 32;            // face radius
static const int CHAR_X0  = 150;           // clearing rectangle
static const int CHAR_Y0  = 0;
static const int CHAR_W   = 88;
static const int CHAR_H   = 102;

// Which value drives the character: 1 = temperature, 5 = real feel
#define CHAR_VIDX 1

// Localized labels (0=RO, 1=EN, 2=HU). No diacritics (the font lacks them).
const char* L_METRIC[3][5] = {
  {"Temperatura","Presiune","Umiditate","Punct roua","Resimtita"},     // RO
  {"Temperature","Pressure","Humidity","Dew point","Real feel"},       // EN
  {"Homerseklet","Legnyomas","Paratartalom","Harmatpont","Hoerzet"}    // HU
};
const char* L_SYNC[3]    = {"sincronizare ora...","time sync...","ido szinkron..."};
const char* L_APTITLE[3] = {"Setare WiFi","WiFi setup","WiFi beallitas"};
const char* L_APNET[3]   = {"Conecteaza-te la reteaua:","Connect to network:","Csatlakozz a halozathoz:"};
const char* L_APURL[3]   = {"apoi deschide in browser:","then open in browser:","majd a bongeszoben:"};

// --- Localized graph texts ---
const char* L_WAITTIME[3] = {"astept ora...","waiting for time...","varok az idot..."};
const char* L_COLLECT[3]  = {"colectez date...","collecting data...","adatgyujtes..."};
const char* L_NOW[3]      = {"acum","now","most"};
const char* GRAPH_WIN_LABEL3[3][4] = {
  {"1 h","12 h","24 h","1 sapt"},   // RO
  {"1 h","12 h","24 h","1 week"},   // EN
  {"1 o","12 o","24 o","1 het"}     // HU
};

// --- Boot texts (screen) ---
const char* L_BOOT[3]     = {"Pornire...","Starting...","Inditas..."};
const char* L_WIFICONN[3] = {"Conectare WiFi...","Connecting WiFi...","WiFi csatlakozas..."};

// --- Config page texts (server) ---
const char* UI_TITLE[3]   = {"MiniWX Receiver - Configurare","MiniWX Receiver Setup","MiniWX Receiver Beallitas"};
const char* UI_PASS[3]    = {"Parola","Password","Jelszo"};
const char* UI_LOC[3]     = {"Locatie","Location","Helyszin"};
const char* UI_TZ[3]      = {"Fus orar (ore)","Timezone offset (hours)","Idozona eltolas (ora)"};
const char* UI_SLIDE[3]   = {"Interval grafice (s) - 0 = oprit","Graph slide interval (s) - 0 = off","Grafikon dia (s) - 0 = ki"};
const char* UI_MAIN[3]    = {"Timp pagina principala (s)","Main page time (s)","Fooldal ido (s)"};
const char* UI_GWIN[3]    = {"Fereastra grafic","Graph window","Grafikon ablak"};
const char* UI_BRIGHT[3]  = {"Luminozitate (%)","Brightness (%)","Fenyero (%)"};
const char* UI_SCHED[3]   = {"Program luminozitate","Brightness schedule","Fenyero utemezes"};
const char* UI_BNIGHT[3]  = {"Luminozitate redusa (%)","Reduced brightness (%)","Csokkentett fenyero (%)"};
const char* UI_FROM[3]    = {"De la ora (0-23)","From hour (0-23)","Ettol az oratol (0-23)"};
const char* UI_TO[3]      = {"Pana la ora (0-23)","To hour (0-23)","Eddig az oraig (0-23)"};
const char* UI_SAVE[3]    = {"Salveaza","Save settings","Mentes"};
const char* UI_REBOOT[3]  = {"Repornire","Reboot device","Ujraindites"};
const char* UI_SAVING[3]  = {"Se salveaza...","Saving...","Mentes..."};
const char* UI_SAVED[3]   = {"Salvare efectuata. Reporneste pentru ecran.","Saved. Reboot to apply on screen.","Mentve. Inditsd ujra a kijelzohoz."};
const char* UI_SAVEERR[3] = {"Eroare la salvare.","Save error.","Mentesi hiba."};
const char* UI_RBCONF[3]  = {"Repornesti dispozitivul?","Reboot the device?","Ujrainditod az eszkozt?"};
const char* UI_RBING[3]   = {"Repornire... reconecteaza-te in cateva secunde.","Rebooting... reconnect in a few seconds.","Ujrainditas... csatlakozz par mp mulva."};

const char* UI_SRCGROUP[3]  = {"Sursa date meteo","Weather data source","Idojaras adatforras"};
const char* UI_SRCTYPE[3]   = {"Tip sursa","Source type","Forras tipus"};
const char* UI_APRSCALL[3]  = {"Indicativ statie","Station callsign","Allomas hivojel"};
const char* UI_APRSLOC[3]   = {"Nume statie","Station name","Allomas neve"};
const char* UI_TEMPOFF[3]   = {"Corectie temp. (C)","Temp. correction (C)","Hom. korrekcio (C)"};
const char* UI_APRSACTIVE[3]= {"Statie afisata pe ecran","Displayed station","Megjelenitett allomas"};
const char* UI_APRSLOGIN[3] = {"Indicativ login APRS-IS","APRS-IS login callsign","APRS-IS belepesi hivojel"};
const char* UI_APRSSRV[3]   = {"Server APRS-IS","APRS-IS server","APRS-IS szerver"};
const char* UI_APRSPORT[3]  = {"Port APRS-IS","APRS-IS port","APRS-IS port"};
const char* UI_MEAS[3]      = {"Interval citire senzor (s)","Sensor read interval (s)","Szenzor olvasasi intervallum (s)"};

int curLang() { return (config.lang >= 0 && config.lang <= 2) ? config.lang : 0; }

const char* METRIC_UNITS[5]  = {"C","mBar","%","C","C"};
const bool  METRIC_DEG[5]    = {true,false,false,true,true};   // has degree symbol?
const int   METRIC_VIDX[5]   = {1,2,3,4,5};                    // index into weatherValues[]
const bool  METRIC_TEMPCOLOR[5] = {true,false,false,false,true}; // color by temperature?

// Thresholds (deg C) based on UTCI categories used by meteorologists for
// "felt temperature". The 0/9/26/32/38 breakpoints are the official UTCI ones;
// the comfort zone is narrowed to 18..26 C (the comfort sub-range).
#define T_COLD_STRESS  0    // < 0   -> cold stress
#define T_SLIGHT_COLD  9    // 0..9  -> slight cold
#define T_COOL         18   // 9..18 -> cool (below comfort)
#define T_COMFORT_HI   26   // 18..26-> comfort (white)
#define T_HEAT_MOD     32   // 26..32-> moderate heat
#define T_HEAT_STRONG  38   // 32..38-> strong heat
//        >= 38  -> very strong / extreme heat

uint16_t tempColor(float t) {
  if (t <  T_COLD_STRESS) return C_TCOLD;     // cold stress
  if (t <  T_SLIGHT_COLD) return C_TSLIGHT;   // slight cold
  if (t <  T_COOL)        return C_TCOOL;     // cool
  if (t <  T_COMFORT_HI)  return C_VALUE;     // comfort -> white
  if (t <  T_HEAT_MOD)    return C_THEATMOD;  // moderate heat
  if (t <  T_HEAT_STRONG) return C_THEATSTR;  // strong heat
  return C_THEAT;                             // very strong / extreme
}

// Character state (6 classes) based on UTCI thresholds:
// 0=winter(<0), 1=cold(0..9), 2=cool(9..18), 3=ok(18..26), 4=warm(26..32), 5=hot(>=32)
int charState(float t) {
  if (t < T_COLD_STRESS) return 0;
  if (t < T_SLIGHT_COLD) return 1;
  if (t < T_COOL)        return 2;
  if (t < T_COMFORT_HI)  return 3;
  if (t < T_HEAT_MOD)    return 4;
  return 5;
}
const char* L_MONTHS[3][12] = {
  {"Ian","Feb","Mar","Apr","Mai","Iun","Iul","Aug","Sep","Oct","Noi","Dec"},      // RO
  {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"},      // EN
  {"Jan","Febr","Marc","Apr","Maj","Jun","Jul","Aug","Szept","Okt","Nov","Dec"}   // HU
};

int  lastMinShown = -1;
bool layoutDrawn  = false;
bool rebootPending = false;
unsigned long rebootAt = 0;

/*---------------- Helpers (unchanged) ----------------*/
String extractJsonValue(const String &json, const String &key) {
  int idx = json.indexOf("\"" + key + "\"");
  if (idx < 0) return String();
  int colon = json.indexOf(':', idx);
  if (colon < 0) return String();
  int q1 = json.indexOf('"', colon);
  if (q1 < 0) return String();
  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0) return String();
  return json.substring(q1 + 1, q2);
}

String intToString(int value) { char buf[8]; sprintf(buf, "%d", value); return String(buf); }

String twoDigits(int value) { return value < 10 ? "0" + String(value) : String(value); }

bool getLocalTimeSafe(struct tm &info) {
  time_t now = time(nullptr);
  if (now <= 1600000000UL) return false;
  struct tm *tmp = localtime(&now);
  if (!tmp) return false;
  info = *tmp;
  return true;
}

/* ===== Web console log (RAM ring buffer, toggled from settings) ===== */
#define LOG_LINES 80
String  logBuf[LOG_LINES];
uint8_t logHead = 0, logCount = 0;

void logMsg(const String &m) {
  if (!config.logEnabled) return;
  String ts;
  struct tm t;
  if (getLocalTimeSafe(t)) ts = twoDigits(t.tm_hour) + ":" + twoDigits(t.tm_min) + ":" + twoDigits(t.tm_sec);
  else ts = "+" + String(millis() / 1000) + "s";
  String line = "[" + ts + "] " + m;
  if (line.length() > 140) line = line.substring(0, 140);   // cap RAM per line
  logBuf[logHead] = line;
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}

void setupNTP() {
  Serial.printf("NTP '%s' tz=%d\n", config.ntpServer.c_str(), config.tzOffsetHours);
  configTime(config.tzOffsetHours * 3600, 0, config.ntpServer.c_str());
}

bool parseMiniWXData(const String &response, String values[9]) {
  int start = 0;
  for (int i = 0; i < 8; i++) {
    int comma = response.indexOf(',', start);
    if (comma < 0) return false;
    values[i] = response.substring(start, comma);
    start = comma + 1;
  }
  values[8] = response.substring(start);
  return values[0].length() > 0;
}

void updateWeatherFromResponse(const String &payload) {
  String values[9];
  if (parseMiniWXData(payload, values)) {
    for (int i = 0; i < 9; i++) weatherValues[i] = values[i];
    weatherValid = true;
    hasEverData  = true;
  } else {
    weatherValid = false;
  }
}

void pollWeather() {
  if (WiFi.status() != WL_CONNECTED || config.bmeHost.length() == 0) return;
  HTTPClient http; WiFiClient client;
  String dataUrl = String("http://") + config.bmeHost;
  if (!config.bmePath.startsWith("/")) dataUrl += "/";
  dataUrl += config.bmePath;
  http.begin(client, dataUrl);
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    logMsg("SRV< " + payload);
    updateWeatherFromResponse(payload);
  } else {
    Serial.printf("HTTP GET fail: %d\n", code);
    logMsg("ERROR: HTTP GET " + String(code) + " " + dataUrl);
    weatherValid = false;
  }
  http.end();
}

/*---------------- Data source: APRS-IS (read-only) ----------------*/
#define APRS_NONE     (-100000L)
#define APRS_STALE_MS 1200000UL      // 20 min without a packet -> data considered stale

struct AprsStation { float tC; float hum; float pres; unsigned long lastMs; bool valid; String comment; };
AprsStation   aprsSt[3];
WiFiClient    aprsClient;
String        aprsRxBuf;
unsigned long aprsLastConn = 0;

String aprsCallOf(int i) {
  if (i == 0) return config.aprsCall0;
  if (i == 1) return config.aprsCall1;
  return config.aprsCall2;
}

String aprsBuildFilter() {
  String calls = "";
  for (int i = 0; i < 3; i++) {
    String c = aprsCallOf(i); c.trim();
    if (c.length()) { calls += "/"; calls += c; }
  }
  if (!calls.length()) return "";
  return "b" + calls + " e" + calls;     // buddy filter + entry/object filter
}

// extract an APRS field of the form <letter><digits> (e.g. t072, h45, b10130)
long aprsNum(const String &w, char key, int mindig, int maxdig, bool sign) {
  for (int i = 0; i + 1 < (int)w.length(); i++) {
    if (w[i] != key) continue;
    int j = i + 1; bool neg = false;
    if (sign && j < (int)w.length() && w[j] == '-') { neg = true; j++; }
    long val = 0; int cnt = 0;
    while (j < (int)w.length() && cnt < maxdig && w[j] >= '0' && w[j] <= '9') {
      val = val * 10 + (w[j] - '0'); j++; cnt++;
    }
    if (cnt >= mindig) return neg ? -val : val;
  }
  return APRS_NONE;
}

float aprsDewPoint(float T, float RH) {
  if (RH <= 0) return T;
  const float a = 17.27f, b = 237.7f;
  float g = (a * T) / (b + T) + logf(RH / 100.0f);
  return (b * g) / (a - g);
}

float aprsHeatIndexC(float T, float RH) {
  if (T < 27.0f) return T;               // below 27C "real feel" ~ temperature
  float Tf = T * 1.8f + 32.0f;
  float hi = -42.379f + 2.04901523f*Tf + 10.14333127f*RH - 0.22475541f*Tf*RH
             - 0.00683783f*Tf*Tf - 0.05481717f*RH*RH + 0.00122874f*Tf*Tf*RH
             + 0.00085282f*Tf*RH*RH - 0.00000199f*Tf*Tf*RH*RH;
  return (hi - 32.0f) / 1.8f;
}

void aprsParseLine(const String &line) {
  if (line.length() < 5 || line[0] == '#') return;     // server comment/keepalive
  int gt = line.indexOf('>'); if (gt < 0) return;
  int co = line.indexOf(':', gt); if (co < 0) return;
  String src = line.substring(0, gt); src.trim(); src.toUpperCase();
  int idx = -1;
  for (int i = 0; i < 3; i++) {
    String c = aprsCallOf(i); c.trim(); c.toUpperCase();
    if (c.length() && c == src) { idx = i; break; }
  }
  // if FROM didn't match, check for APRS object name (;NAME     * format)
  if (idx < 0) {
    String info = line.substring(co + 1);
    if (info.length() >= 10 && info[0] == ';') {
      String objName = info.substring(1, 10); objName.trim(); objName.toUpperCase();
      for (int i = 0; i < 3; i++) {
        String c = aprsCallOf(i); c.trim(); c.toUpperCase();
        if (c.length() && c == objName) { idx = i; break; }
      }
    }
  }
  if (idx < 0) return;
  String info = line.substring(co + 1);

  // --- WX data (tXXX/hXX/bXXXXX), anchored on the '_' symbol ---
  int ws = info.indexOf('_');
  String w = (ws >= 0) ? info.substring(ws) : info;
  long tF = aprsNum(w, 't', 2, 3, true);
  long h  = aprsNum(w, 'h', 2, 2, false);
  long b  = aprsNum(w, 'b', 5, 5, false);
  bool any = false;
  if (tF != APRS_NONE) { aprsSt[idx].tC  = (tF - 32.0f) * 5.0f / 9.0f
                         + (idx==0?config.aprsTempOff0:idx==1?config.aprsTempOff1:config.aprsTempOff2).toFloat(); any = true; }
  if (h  != APRS_NONE) { aprsSt[idx].hum = (h == 0) ? 100.0f : (float)h; any = true; }
  if (b  != APRS_NONE) { aprsSt[idx].pres = b / 10.0f; any = true; }
  if (any) { aprsSt[idx].lastMs = millis(); aprsSt[idx].valid = true; }

  // --- station comment (from the position packet, symbol != '_') ---
  int p = -1; char ty = info.length() ? info[0] : 0;
  if (ty == '=' || ty == '!') p = 1;
  else if (ty == '@' || ty == '/') p = 8;              // type + 7-char timestamp
  if (p >= 0 && (int)info.length() >= p + 19 && info[p] >= '0' && info[p] <= '9') {
    char symcode = info[p + 18];
    if (symcode != '_') {                               // not a WX packet -> human comment
      String cm = info.substring(p + 19); cm.trim();
      int par = cm.indexOf(" (");                       // strip trailing " (version)"
      if (par > 0) cm = cm.substring(0, par);
      if (cm.length() > 48) cm = cm.substring(0, 48);
      if (cm.length()) aprsSt[idx].comment = cm;
    }
  }
}

void aprsEnsureConnected() {
  if (aprsClient.connected()) return;
  unsigned long now = millis();
  if (aprsLastConn != 0 && now - aprsLastConn < 15000UL) return;   // retry every 15s
  aprsLastConn = now;
  String srv = config.aprsServer; srv.trim();
  if (!srv.length()) srv = "rotate.aprs2.net";
  int port = config.aprsPort ? config.aprsPort : 14580;
  if (aprsClient.connect(srv.c_str(), port)) {
    String login = config.aprsLogin; login.trim();
    if (!login.length()) login = "NOCALL";
    String cmd = "user " + login + " pass -1 vers SmallTV 3 ";
    String f = aprsBuildFilter();
    if (f.length()) cmd += "filter " + f;
    aprsClient.print(cmd + "\r\n");
    aprsRxBuf = "";
    Serial.printf("APRS-IS conectat %s:%d  [%s]\n", srv.c_str(), port, cmd.c_str());
    logMsg("APRS-IS connected " + srv + ":" + String(port) + " [" + cmd + "]");
  } else {
    Serial.printf("APRS-IS conectare esuata la %s:%d\n", srv.c_str(), port);
    logMsg("ERROR: APRS-IS connect failed " + srv + ":" + String(port));
  }
}

void aprsPoll() {
  while (aprsClient.available()) {
    char ch = (char)aprsClient.read();
    if (ch == '\n') { if (aprsRxBuf.length()) logMsg("APRS< " + aprsRxBuf); aprsParseLine(aprsRxBuf); aprsRxBuf = ""; }
    else if (ch != '\r') {
      if (aprsRxBuf.length() < 300) aprsRxBuf += ch; else aprsRxBuf = "";
    }
  }
}

int activeStations(int idxOut[MAXSTA]) {
  if (config.dataSource != 1) { idxOut[0] = 0; return 1; }   // local: a single source
  int limit = config.aprsActive + 1;                         // 0->1 station, 1->2, 2->3
  if (limit < 1) limit = 1;
  if (limit > MAXSTA) limit = MAXSTA;
  int n = 0;
  for (int i = 0; i < MAXSTA && n < limit; i++) {
    String c = aprsCallOf(i); c.trim();
    if (c.length()) idxOut[n++] = i;
  }
  if (n == 0) { idxOut[0] = 0; n = 1; }
  return n;
}

String aprsLocOf(int i) {
  if (i == 0) return config.aprsLoc0;
  if (i == 1) return config.aprsLoc1;
  return config.aprsLoc2;
}
String buildLocationLabel(int sta) {
  if (config.dataSource == 1) {
    String nm = aprsLocOf(sta); nm.trim();
    if (nm.length()) return nm;                      // name set manually from the server
    return "Station " + String(sta + 1);             // fallback until you set the name
  }
  String base = config.location;
  if (!base.length()) base = "Mini Weather";
  return base;
}

void aprsApplyStation(int idx) {
  if (idx < 0 || idx > 2) idx = 0;
  AprsStation &s = aprsSt[idx];
  if (s.valid) {
    weatherValues[1] = String(s.tC, 1);
    weatherValues[2] = String(s.pres, 1);
    weatherValues[3] = String(s.hum, 0);
    weatherValues[4] = String(aprsDewPoint(s.tC, s.hum), 1);
    weatherValues[5] = String(aprsHeatIndexC(s.tC, s.hum), 1);
    hasEverData  = true;
    weatherValid = (millis() - s.lastMs < APRS_STALE_MS);
  } else {
    weatherValid = false;
  }
}

/*---------------- UI drawing (new) ----------------*/
void drawClock(const struct tm &t) {
  String hhmm = twoDigits(t.tm_hour) + ":" + twoDigits(t.tm_min);
  tft.fillRect(0, 26, 146, 52, C_BG);          // clear only the left strip (keep the character)
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_VALUE, C_BG);
  tft.drawString(hhmm, LEFT_X, CLK_TOP, 7);     // font 7 = 7-seg digits, 48px, left
}

void drawSubline(const struct tm &t) {
  String tz  = String(config.tzOffsetHours);
  String sub = twoDigits(t.tm_mday) + " " + L_MONTHS[curLang()][t.tm_mon] + " " + String(t.tm_year + 1900) + "   UTC " +
               (config.tzOffsetHours >= 0 ? "+" + tz : tz);
  tft.fillRect(0, SUB_Y, 240, 16, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_UNIT, C_BG);
  tft.drawString(sub, LEFT_X, SUB_Y, 2);
}

void drawMetricValue(int row) {
  int y = ROW0_Y + row * ROW_STEP;
  tft.fillRect(VAL_X0, y, PAD_R - VAL_X0 + 2, 16, C_BG);  // clear the value cell

  if (!hasEverData) {                          // never received data
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(C_UNIT, C_BG);
    tft.drawString("--", PAD_R, y, 2);
    return;
  }

  // value color
  uint16_t valColor;
  if (!weatherValid)                           // stale data (server unavailable) -> neutral grey
    valColor = C_STALE;
  else if (METRIC_TEMPCOLOR[row])              // fresh + temperature -> color code
    valColor = tempColor(weatherValues[METRIC_VIDX[row]].toFloat());
  else
    valColor = C_VALUE;                        // fresh, the rest -> white

  // unit (grey), right-aligned
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_UNIT, C_BG);
  tft.drawString(METRIC_UNITS[row], PAD_R, y, 2);
  int cursor = PAD_R - tft.textWidth(METRIC_UNITS[row], 2) - 4;

  // degree symbol (small ring), in the value color
  if (METRIC_DEG[row]) {
    tft.drawCircle(cursor - 2, y + 3, 2, valColor);
    cursor -= 7;
  }

  // the value
  tft.setTextColor(valColor, C_BG);
  tft.drawString(weatherValues[METRIC_VIDX[row]], cursor, y, 2);
}

int lastCharState = -99;

// thin arc (mouth), angles in degrees: 0=right, 90=down (y grows downward)
void arcMouth(int cx, int cy, int r, int a0, int a1, uint16_t col) {
  int steps = 16; float px = 0, py = 0;
  for (int i = 0; i <= steps; i++) {
    float a = (a0 + (float)(a1 - a0) * i / steps) * PI / 180.0;
    float x = cx + cos(a) * r, y = cy + sin(a) * r;
    if (i > 0) { tft.drawLine((int)px, (int)py, (int)x, (int)y, col); }
    px = x; py = y;
  }
}

void drawCharacter(bool force) {
  int st = hasEverData ? charState(weatherValues[CHAR_VIDX].toFloat()) : -1;
  if (!force && st == lastCharState) return;
  lastCharState = st;

  tft.fillRect(CHAR_X0, CHAR_Y0, CHAR_W, CHAR_H, C_BG);
  int cx = CHAR_CX, cy = CHAR_CY, R = CHAR_R;
  uint16_t eye = tft.color565(30, 30, 40);

  if (st < 0) {                                   // no data -> neutral grey face
    tft.drawCircle(cx, cy, R, C_UNIT);
    tft.fillCircle(cx - 11, cy - 6, 3, C_UNIT);
    tft.fillCircle(cx + 11, cy - 6, 3, C_UNIT);
    tft.drawFastHLine(cx - 9, cy + 12, 18, C_UNIT);
    return;
  }

  if (st == 0) {                                  // WINTER (<0) - winter clothes
    uint16_t face = tft.color565(175, 210, 235);
    uint16_t hat  = C_THEAT;
    uint16_t scrf = C_TCOLD;
    tft.fillCircle(cx, cy, R, face);
    tft.fillCircle(cx - 11, cy - 4, 3, eye);
    tft.fillCircle(cx + 11, cy - 4, 3, eye);
    tft.drawFastHLine(cx - 8, cy + 12, 16, eye);                       // neutral mouth
    tft.fillRoundRect(cx - R - 1, cy + R - 8, 2 * R + 2, 14, 5, scrf); // scarf
    tft.fillRoundRect(cx - R + 2, cy - R - 12, 2 * R - 4, 16, 6, hat); // cap
    tft.fillRect(cx - R, cy - R - 1, 2 * R, 8, hat);                   // band
    tft.fillCircle(cx, cy - R - 14, 5, TFT_WHITE);                     // pompom
  } else if (st == 1) {                           // COLD (0..9) - blue, sad
    uint16_t face = tft.color565(111, 176, 255);
    tft.fillCircle(cx, cy, R, face);
    tft.fillCircle(cx - 11, cy - 6, 3, eye);
    tft.fillCircle(cx + 11, cy - 6, 3, eye);
    arcMouth(cx, cy + 20, 13, 200, 340, eye);                         // sad mouth
  } else if (st == 2) {                           // COOL (9..18) - turquoise, neutral
    uint16_t face = tft.color565(154, 217, 200);
    tft.fillCircle(cx, cy, R, face);
    tft.fillCircle(cx - 11, cy - 6, 3, eye);
    tft.fillCircle(cx + 11, cy - 6, 3, eye);
    tft.drawFastHLine(cx - 8, cy + 12, 16, eye);                      // neutral mouth
  } else if (st == 3) {                           // OK (18..26) - yellow, smile
    uint16_t face = tft.color565(255, 205, 70);
    tft.fillCircle(cx, cy, R, face);
    tft.fillCircle(cx - 11, cy - 6, 3, eye);
    tft.fillCircle(cx + 11, cy - 6, 3, eye);
    arcMouth(cx, cy + 4, 13, 20, 160, eye);                          // smile
  } else if (st == 4) {                           // WARM (26..32) - orange, one sweat drop
    uint16_t face = tft.color565(255, 177, 74);
    uint16_t drop = tft.color565(90, 200, 235);
    tft.fillCircle(cx, cy, R, face);
    tft.fillCircle(cx - 11, cy - 6, 3, eye);
    tft.fillCircle(cx + 11, cy - 6, 3, eye);
    arcMouth(cx, cy + 6, 12, 25, 155, eye);                          // slight smile
    tft.fillTriangle(cx + R - 5, cy - 12, cx + R + 1, cy - 12, cx + R - 2, cy - 18, drop);
    tft.fillCircle(cx + R - 2, cy - 11, 3, drop);                    // one sweat drop
  } else {                                        // HOT (>=32) - red, panting + sweating
    uint16_t face = C_THEAT;
    uint16_t drop = tft.color565(90, 200, 235);
    tft.fillCircle(cx, cy, R, face);
    tft.fillCircle(cx - 11, cy - 6, 3, eye);
    tft.fillCircle(cx + 11, cy - 6, 3, eye);
    tft.fillCircle(cx, cy + 12, 7, eye);                             // open mouth
    tft.fillTriangle(cx + R - 5, cy - 12, cx + R + 1, cy - 12, cx + R - 2, cy - 18, drop);
    tft.fillCircle(cx + R - 2, cy - 11, 3, drop);
    tft.fillTriangle(cx - R + 1, cy - 6, cx - R + 7, cy - 6, cx - R + 4, cy - 12, drop);
    tft.fillCircle(cx - R + 4, cy - 5, 3, drop);                     // second sweat drop
  }
}

void drawAllMetrics() {
  for (int i = 0; i < 5; i++) drawMetricValue(i);
  drawCharacter(false);
}


// Common footer (bottom): IP on the left, credit + version on the right
void drawFooter() {
  tft.setTextColor(C_UNIT, C_BG);
  tft.setTextDatum(BL_DATUM);
  tft.drawString(WiFi.localIP().toString(), 4, 238, 1);     // device IP address
  tft.setTextDatum(BR_DATUM);
  tft.drawString("Dev. by YO7ZRO " APP_VERSION, 236, 238, 1);
}

void drawStaticLayout() {
  tft.fillScreen(C_BG);
  // location (top-left)
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_ACCENT, C_BG);
  String loc = curLocationLabel.length() ? curLocationLabel : (config.location.length() ? config.location : "Mini Weather");
  int locX = LEFT_X;
  if (config.dataSource == 1) {                       // pill in the station color (same as the graph)
    tft.fillRoundRect(LEFT_X, LOC_Y + 2, 10, 10, 2, C_STA[dispSta % 3]);
    locX = LEFT_X + 16;
  }
  tft.drawString(loc, locX, LOC_Y, 2);
  // separator
  tft.drawFastHLine(16, DIV_Y, 208, C_DIV);
  // metric labels (drawn once)
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_LABEL, C_BG);
  for (int i = 0; i < 5; i++)
    tft.drawString(L_METRIC[curLang()][i], PAD_L, ROW0_Y + i * ROW_STEP, 2);

  // footer bottom-right (credit + version)
  drawFooter();

  layoutDrawn   = true;
  lastMinShown  = -1;
  lastCharState = -99;          // force character redraw
}

/*---------------- Multi-resolution history + graphs ----------------*/
void ringPush(Sample* buf, int& n, int cap, const Sample& s) {
  if (n < cap) buf[n++] = s;
  else { for (int i = 1; i < cap; i++) buf[i-1] = buf[i]; buf[cap-1] = s; }
}

// ---- Hourly tier (7 days) kept in FLASH (ring), not in RAM ----
//      Written rarely (once/hour/station), read directly from flash when drawing.
//      LittleFS spreads writes (wear leveling), it doesn't wear the same block.
#define HOUR_HDR (3 + MAXSTA * 2)
static uint32_t hourSlotOffset(int sta, int slot) {
  return (uint32_t)HOUR_HDR + ((uint32_t)sta * N_HOUR + slot) * sizeof(Sample);
}
void hourFormat() {
  File f = LittleFS.open(HOUR_FILE, "w");
  if (!f) return;
  uint8_t magic = HOUR_MAGIC, ver = 1, nst = MAXSTA;
  f.write(magic); f.write(ver); f.write(nst);
  uint16_t z = 0;
  for (int k = 0; k < MAXSTA; k++) f.write((uint8_t*)&z, 2);
  Sample empty = {0, 0, 0, 0};
  for (int k = 0; k < MAXSTA; k++) for (int i = 0; i < N_HOUR; i++) f.write((uint8_t*)&empty, sizeof(Sample));
  f.close();
  for (int k = 0; k < MAXSTA; k++) hourHead[k] = 0;
}
void hourLoad() {
  File f = LittleFS.open(HOUR_FILE, "r");
  if (!f) { hourFormat(); return; }
  uint8_t magic = 0, ver = 0, nst = 0;
  f.read(&magic, 1); f.read(&ver, 1); f.read(&nst, 1);
  if (magic != HOUR_MAGIC || ver != 1 || nst != MAXSTA) { f.close(); hourFormat(); return; }
  for (int k = 0; k < MAXSTA; k++) { uint16_t h = 0; f.read((uint8_t*)&h, 2); hourHead[k] = (h < N_HOUR) ? h : 0; }
  f.close();
  Serial.println("Nivel orar (flash) incarcat.");
}
void hourAppend(int sta, const Sample& s) {
  if (sta < 0 || sta >= MAXSTA) return;
  File f = LittleFS.open(HOUR_FILE, "r+");
  if (!f) { hourFormat(); f = LittleFS.open(HOUR_FILE, "r+"); if (!f) return; }
  uint16_t head = hourHead[sta];
  f.seek(hourSlotOffset(sta, head)); f.write((uint8_t*)&s, sizeof(Sample));
  head = (head + 1) % N_HOUR; hourHead[sta] = head;
  f.seek(3 + sta * 2); f.write((uint8_t*)&head, 2);     // update head in the header
  f.close();
}

// Record a reading into the tiers (requires synced time for the timestamp)
void recordSample(int sta, float t, float h, float p) {
  if (sta < 0 || sta >= MAXSTA) return;
  time_t now = time(nullptr);
  if (now < 1600000000UL) return;
  Sample s = { (uint32_t)now, t, h, p };
  ringPush(bufFine[sta], nFine[sta], N_FINE, s);
  if (lastMinT[sta]  == 0 || (uint32_t)now - lastMinT[sta]  >= 60)   { ringPush(bufMin[sta],  nMin[sta],  N_MIN,  s); lastMinT[sta]  = now; }
  if (lastHourT[sta] == 0 || (uint32_t)now - lastHourT[sta] >= 3600) { hourAppend(sta, s); lastHourT[sta] = now; }
}

void saveHistory() {
  File f = LittleFS.open(HIST_FILE, "w");
  if (!f) return;
  uint8_t magic = 0xA2, ver = 5, nst = MAXSTA;     // ver 5: only fine+min (the hourly tier is in /hour.bin)
  f.write(magic); f.write(ver); f.write(nst);
  for (int k = 0; k < MAXSTA; k++) {
    f.write((uint8_t*)&nFine[k], sizeof(int));
    f.write((uint8_t*)&nMin[k],  sizeof(int));
    f.write((uint8_t*)&lastMinT[k],  sizeof(uint32_t));
    f.write((uint8_t*)&lastHourT[k], sizeof(uint32_t));
    f.write((uint8_t*)bufFine[k], sizeof(Sample) * N_FINE);
    f.write((uint8_t*)bufMin[k],  sizeof(Sample) * N_MIN);
  }
  f.close();
}

void loadHistory() {
  File f = LittleFS.open(HIST_FILE, "r");
  if (!f) return;
  uint8_t magic = 0, ver = 0, nst = 0;
  f.read(&magic, 1); f.read(&ver, 1); f.read(&nst, 1);
  if (magic != 0xA2 || ver != 5 || nst != MAXSTA) { f.close(); return; }
  for (int k = 0; k < MAXSTA; k++) {
    f.read((uint8_t*)&nFine[k], sizeof(int));
    f.read((uint8_t*)&nMin[k],  sizeof(int));
    f.read((uint8_t*)&lastMinT[k],  sizeof(uint32_t));
    f.read((uint8_t*)&lastHourT[k], sizeof(uint32_t));
    if (nFine[k] < 0 || nFine[k] > N_FINE || nMin[k] < 0 || nMin[k] > N_MIN) {
      nFine[k] = nMin[k] = 0; f.close(); return;
    }
    f.read((uint8_t*)bufFine[k], sizeof(Sample) * N_FINE);
    f.read((uint8_t*)bufMin[k],  sizeof(Sample) * N_MIN);
  }
  f.close();
  Serial.println("Istoric (fine+min) incarcat din flash.");
}

static float sampleVal(const Sample& s, int metric) {
  return metric == 0 ? s.temp : (metric == 1 ? s.hum : s.pres);
}
static int cmpPt(const void* a, const void* b) {
  uint32_t ta = ((const Pt*)a)->t, tb = ((const Pt*)b)->t;
  return (ta < tb) ? -1 : (ta > tb ? 1 : 0);
}

// Append to series[] the hourly samples (7 days) read DIRECTLY from flash
void hourAddToSeries(int sta, int metric, uint32_t cutoff) {
  if (sta < 0 || sta >= MAXSTA) return;
  File f = LittleFS.open(HOUR_FILE, "r");
  if (!f) return;
  f.seek(hourSlotOffset(sta, 0));
  int cap = (int)(sizeof(series) / sizeof(series[0]));
  Sample s;
  for (int i = 0; i < N_HOUR && seriesN < cap; i++) {
    if (f.read((uint8_t*)&s, sizeof(Sample)) != (int)sizeof(Sample)) break;
    if (s.t > 0 && s.t >= cutoff) series[seriesN++] = { s.t, sampleVal(s, metric) };
  }
  f.close();
}

// Gather samples within the window from all tiers and sort them by time
void buildSeries(int sta, int metric, uint32_t windowSec, uint32_t nowEpoch) {
  seriesN = 0;
  if (sta < 0 || sta >= MAXSTA) return;
  uint32_t cutoff = (nowEpoch > windowSec) ? nowEpoch - windowSec : 0;
  for (int i = 0; i < nFine[sta]; i++) if (bufFine[sta][i].t >= cutoff) series[seriesN++] = { bufFine[sta][i].t, sampleVal(bufFine[sta][i], metric) };
  for (int i = 0; i < nMin[sta];  i++) if (bufMin[sta][i].t  >= cutoff) series[seriesN++] = { bufMin[sta][i].t,  sampleVal(bufMin[sta][i],  metric) };
  hourAddToSeries(sta, metric, cutoff);     // hourly tier (7 days) read directly from flash
  if (seriesN > 1) qsort(series, seriesN, sizeof(Pt), cmpPt);
}

void drawGraphMulti(int metric, const char* title) {
  int gwi = (config.graphWindow >= 0 && config.graphWindow <= 3) ? config.graphWindow : 0;
  uint32_t W = GRAPH_WINDOWS[gwi];

  tft.fillScreen(C_BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.drawString(title, 120, 6, 4);
  drawFooter();

  const int gx = 40, gy = 64, gw_ = 192, gh = 138;
  tft.drawRect(gx, gy, gw_, gh, C_DIV);

  time_t now = time(nullptr);
  if (now < 1600000000UL) {
    tft.setTextDatum(MC_DATUM); tft.setTextColor(C_UNIT, C_BG);
    tft.drawString(L_WAITTIME[curLang()], 120, gy + gh / 2, 2);
    return;
  }

  int idx[MAXSTA]; int nSta = activeStations(idx);
  uint32_t cutoff = (uint32_t)now - W;

  // step 1: common min/max across all stations (same Y scale)
  float mn = 1e9f, mx = -1e9f; bool have = false;
  for (int k = 0; k < nSta; k++) {
    buildSeries(idx[k], metric, W, (uint32_t)now);     // RAM (fine+min) + flash (hourly)
    for (int i = 0; i < seriesN; i++) { float v = series[i].v; if (v<mn)mn=v; if(v>mx)mx=v; have=true; }
  }

  // legend: name + last value, in the station color (max 3 rows)
  tft.setTextDatum(TL_DATUM);
  for (int k = 0; k < nSta; k++) {
    int si = idx[k];
    String cl = (config.dataSource == 1) ? buildLocationLabel(si) : String("BME");
    cl.trim(); if (!cl.length()) cl = String("ST") + String(si + 1);
    buildSeries(si, metric, W, (uint32_t)now);
    String val = (seriesN > 0) ? String(series[seriesN - 1].v, 1) : String("--");
    tft.setTextColor(C_STA[si % 3], C_BG);
    tft.drawString(cl + " " + val, 4, 34 + k * 10, 1);
  }

  if (!have) {
    tft.setTextDatum(MC_DATUM); tft.setTextColor(C_UNIT, C_BG);
    tft.drawString(L_COLLECT[curLang()], 120, gy + gh / 2, 2);
    return;
  }
  if (mx - mn < 0.5f) { mx += 0.5f; mn -= 0.5f; }
  float pad = (mx - mn) * 0.10f; mn -= pad; mx += pad;

  int yd = (mx >= 1000 || mn >= 1000) ? 0 : 1;
  tft.setTextColor(C_UNIT, C_BG);
  tft.setTextDatum(TR_DATUM); tft.drawString(String(mx, yd), gx - 3, gy - 2, 1);
  tft.setTextDatum(BR_DATUM); tft.drawString(String(mn, yd), gx - 3, gy + gh + 2, 1);

  // step 2: each station's line, in its own color
  for (int k = 0; k < nSta; k++) {
    int si = idx[k];
    buildSeries(si, metric, W, (uint32_t)now);
    if (seriesN < 2) continue;
    uint16_t col = C_STA[si % 3];
    float px = 0, py = 0;
    for (int i = 0; i < seriesN; i++) {
      float x = gx + 1 + (float)(series[i].t - cutoff) * (gw_ - 3) / W;
      float y = gy + gh - 2 - (series[i].v - mn) / (mx - mn) * (gh - 4);
      if (i > 0) tft.drawLine((int)px, (int)py, (int)x, (int)y, col);
      px = x; py = y;
    }
  }

  tft.setTextColor(C_UNIT, C_BG);
  tft.setTextDatum(TL_DATUM); tft.drawString(String("-") + GRAPH_WIN_LABEL3[curLang()][gwi], gx, gy + gh + 4, 1);
  tft.setTextDatum(TR_DATUM); tft.drawString(L_NOW[curLang()], gx + gw_, gy + gh + 4, 1);
}

void renderSlide(int phase) {
  int idx[MAXSTA]; int nSta = activeStations(idx);
  if (phase < nSta) {                              // main page for one station
    dispSta = idx[phase];
    if (config.dataSource == 1) aprsApplyStation(dispSta);
    curLocationLabel = buildLocationLabel(dispSta);
    drawStaticLayout();
    drawAllMetrics();
  } else {                                         // graphs overlaid across stations
    int metric = phase - nSta;                     // 0=temp, 1=humidity, 2=pressure
    if (metric == 0)      drawGraphMulti(0, L_METRIC[curLang()][0]);
    else if (metric == 1) drawGraphMulti(1, L_METRIC[curLang()][2]);
    else                  drawGraphMulti(2, L_METRIC[curLang()][1]);
  }
}

// Apply brightness (PWM on backlight, active LOW), taking the schedule into account
void applyBrightness() {
  int b = config.brightDay;
  if (config.schedEnabled) {
    struct tm t;
    if (getLocalTimeSafe(t)) {
      int h = t.tm_hour, s = config.schedStart, e = config.schedEnd;
      bool night;
      if (s == e)      night = false;
      else if (s < e)  night = (h >= s && h < e);
      else             night = (h >= s || h < e);   // interval spanning midnight
      if (night) b = config.brightNight;
    }
  }
  if (b < 5)   b = 5;
  if (b > 100) b = 100;
  int low = (int)((long)b * 255 / 100);   // the longer the pin is LOW = the brighter
  analogWrite(BL_PIN, 255 - low);          // analogWrite sets the HIGH time (backlight active LOW)
}

/* ---------------- Setup ---------------- */
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n[BOOT] MiniWeather"));

  tft.init();
  tft.setRotation(0);
  initPalette();
  pinMode(BL_PIN, OUTPUT);
  analogWriteRange(255);
  applyBrightness();                  // backlight via PWM (active LOW)

  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString(L_BOOT[curLang()], 8, 8, 2);

  if (LittleFS.begin() && LittleFS.exists(CONFIG_PATH)) {
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (f) {
      String s = f.readString(); f.close();
      config.ssid = extractJsonValue(s, "ssid");
      config.pass = extractJsonValue(s, "pass");
      String v;
      if ((v = extractJsonValue(s, "bmeHost")).length())   config.bmeHost = v;
      if ((v = extractJsonValue(s, "bmePath")).length())   config.bmePath = v;
      if ((v = extractJsonValue(s, "ntpServer")).length()) config.ntpServer = v;
      if ((v = extractJsonValue(s, "tzOffsetHours")).length()) config.tzOffsetHours = atoi(v.c_str());
      if ((v = extractJsonValue(s, "location")).length())  config.location = v;
      if ((v = extractJsonValue(s, "lang")).length())      config.lang = atoi(v.c_str());
      if ((v = extractJsonValue(s, "slideSeconds")).length()) config.slideSeconds = atoi(v.c_str());
      if ((v = extractJsonValue(s, "mainSeconds")).length())  config.mainSeconds = atoi(v.c_str());
      if ((v = extractJsonValue(s, "graphWindow")).length())  config.graphWindow = atoi(v.c_str());
      if ((v = extractJsonValue(s, "brightDay")).length())    config.brightDay = atoi(v.c_str());
      if ((v = extractJsonValue(s, "brightNight")).length())  config.brightNight = atoi(v.c_str());
      if ((v = extractJsonValue(s, "schedEnabled")).length()) config.schedEnabled = (atoi(v.c_str()) != 0);
      if ((v = extractJsonValue(s, "logEnabled")).length())   config.logEnabled   = (atoi(v.c_str()) != 0);
      if ((v = extractJsonValue(s, "schedStart")).length())   config.schedStart = atoi(v.c_str());
      if ((v = extractJsonValue(s, "schedEnd")).length())     config.schedEnd = atoi(v.c_str());
      if ((v = extractJsonValue(s, "dataSource")).length()) config.dataSource = atoi(v.c_str());
      config.aprsCall0 = extractJsonValue(s, "aprsCall0");
      config.aprsCall1 = extractJsonValue(s, "aprsCall1");
      config.aprsCall2 = extractJsonValue(s, "aprsCall2");
      if ((v = extractJsonValue(s, "aprsActive")).length()) config.aprsActive = atoi(v.c_str());
      config.aprsLogin = extractJsonValue(s, "aprsLogin");
      { String sv = extractJsonValue(s, "aprsServer"); if (sv.length()) config.aprsServer = sv; }
      if ((v = extractJsonValue(s, "aprsPort")).length()) config.aprsPort = atoi(v.c_str());
      if ((v = extractJsonValue(s, "measSeconds")).length()) config.measSeconds = atoi(v.c_str());
      config.aprsLoc0 = extractJsonValue(s, "aprsLoc0");
      config.aprsLoc1 = extractJsonValue(s, "aprsLoc1");
      config.aprsLoc2 = extractJsonValue(s, "aprsLoc2");
      config.aprsTempOff0 = extractJsonValue(s, "aprsTempOff0");
      config.aprsTempOff1 = extractJsonValue(s, "aprsTempOff1");
      config.aprsTempOff2 = extractJsonValue(s, "aprsTempOff2");
    }
  }

  loadHistory();   // restore fine+min from flash
  hourLoad();      // init/load the hourly tier (7 days) from flash
  applyBrightness();  // apply brightness per the loaded config

  if (config.ssid.length() > 0) {
    tft.drawString(L_WIFICONN[curLang()], 8, 28, 2);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(config.ssid.c_str(), config.pass.c_str());
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 60) { delay(500); Serial.print('.'); retry++; }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
      setupNTP();
      configMode = false;                 // the layout is drawn by loop()
    } else {
      configMode = true;
    }
  } else {
    configMode = true;
  }

  if (configMode) {
    WiFi.mode(WIFI_AP);
    String apName = "MiniWeather-Setup-" + String(ESP.getChipId(), HEX);
    WiFi.softAP(apName.c_str());
    IPAddress apIP = WiFi.softAPIP();
    tft.fillScreen(C_BG);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(C_ACCENT, C_BG);
    tft.drawString(L_APTITLE[curLang()], 120, 14, 4);
    tft.setTextColor(C_LABEL, C_BG);
    tft.drawString(L_APNET[curLang()], 120, 60, 2);
    tft.setTextColor(C_VALUE, C_BG);
    tft.drawString(apName, 120, 84, 2);
    tft.setTextColor(C_LABEL, C_BG);
    tft.drawString(L_APURL[curLang()], 120, 124, 2);
    tft.setTextColor(C_VALUE, C_BG);
    tft.drawString("http://" + apIP.toString(), 120, 148, 2);
  }

  server.on("/", HTTP_GET, []() {
    int L = curLang();
    String page = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>MiniWX Receiver</title>";
    page += "<style>html{background:#0d0f12;}";
    page += "body{max-width:520px;margin:0 auto;background:#181b20;color:#e8e8e8;font-family:'Segoe UI',Arial,sans-serif;padding:18px 20px;}";
    page += "h2{color:#FFB000;}";
    page += "label{font-size:13px;color:#8a9099;}";
    page += "input,select{font-size:15px;padding:8px;margin:4px 0 12px;width:100%;box-sizing:border-box;background:#1f232a;color:#e8e8e8;border:1px solid #2a2f37;border-radius:6px;}";
    page += "button{font-size:15px;padding:10px;margin:8px 0;width:100%;background:#1f232a;color:#e8e8e8;border:1px solid #2a2f37;border-radius:8px;cursor:pointer;}";
    page += "button:hover{background:#FFB000;color:#000;border-color:#FFB000;}";
    page += "#rb{color:#FF7A5A;border-color:#3a2a2a;}";
    page += "#rb:hover{background:#FF7A5A;color:#000;border-color:#FF7A5A;}";
    page += "input[type=checkbox]{width:auto;margin-right:8px;vertical-align:middle;}";
    page += "#status{min-height:22px;font-size:14px;margin-top:10px;color:#8a9099;}</style></head><body>";
    page += "<h2>" + String(UI_TITLE[L]) + "</h2>";
    page += "<label>SSID</label><input id=\"ssid\" value=\"" + config.ssid + "\">";
    page += "<label>" + String(UI_PASS[L]) + "</label><input id=\"pass\" type=\"password\" value=\"" + config.pass + "\">";
    page += "<label>BME host/IP</label><input id=\"bmeHost\" value=\"" + config.bmeHost + "\">";
    page += "<label>BME path</label><input id=\"bmePath\" value=\"" + config.bmePath + "\" readonly style=\"opacity:0.5;cursor:default;\">";
    page += "<h3 style=\"color:#FFB000;border-top:1px solid #2A2E37;padding-top:8px;margin-top:14px;\">" + String(UI_SRCGROUP[L]) + "</h3>";
    page += "<label>" + String(UI_SRCTYPE[L]) + "</label><select id=\"dataSource\">";
    page += "<option value=\"0\"" + String(config.dataSource==0?" selected":"") + ">MiniWX local (/jquery)</option>";
    page += "<option value=\"1\"" + String(config.dataSource==1?" selected":"") + ">APRS-IS</option></select>";
    page += "<label>" + String(UI_APRSCALL[L]) + " 1</label><input id=\"aprsCall0\" value=\"" + config.aprsCall0 + "\">";
    page += "<label>" + String(UI_APRSCALL[L]) + " 2</label><input id=\"aprsCall1\" value=\"" + config.aprsCall1 + "\">";
    page += "<label>" + String(UI_APRSCALL[L]) + " 3</label><input id=\"aprsCall2\" value=\"" + config.aprsCall2 + "\">";
    page += "<label>" + String(UI_APRSLOC[L]) + " 1</label><input id=\"aprsLoc0\" value=\"" + config.aprsLoc0 + "\">";
    page += "<label>" + String(UI_APRSLOC[L]) + " 2</label><input id=\"aprsLoc1\" value=\"" + config.aprsLoc1 + "\">";
    page += "<label>" + String(UI_APRSLOC[L]) + " 3</label><input id=\"aprsLoc2\" value=\"" + config.aprsLoc2 + "\">";
    page += "<label>" + String(UI_TEMPOFF[L]) + " 1</label><input id=\"aprsTempOff0\" value=\"" + config.aprsTempOff0 + "\">";
    page += "<label>" + String(UI_TEMPOFF[L]) + " 2</label><input id=\"aprsTempOff1\" value=\"" + config.aprsTempOff1 + "\">";
    page += "<label>" + String(UI_TEMPOFF[L]) + " 3</label><input id=\"aprsTempOff2\" value=\"" + config.aprsTempOff2 + "\">";
    page += "<label>" + String(UI_APRSACTIVE[L]) + "</label><select id=\"aprsActive\">";
    page += "<option value=\"0\"" + String(config.aprsActive==0?" selected":"") + ">1</option>";
    page += "<option value=\"1\"" + String(config.aprsActive==1?" selected":"") + ">2</option>";
    page += "<option value=\"2\"" + String(config.aprsActive==2?" selected":"") + ">3</option></select>";
    page += "<label>" + String(UI_APRSLOGIN[L]) + "</label><input id=\"aprsLogin\" value=\"" + config.aprsLogin + "\">";
    page += "<label>" + String(UI_APRSSRV[L]) + "</label><input id=\"aprsServer\" value=\"" + config.aprsServer + "\">";
    page += "<label>" + String(UI_APRSPORT[L]) + "</label><input id=\"aprsPort\" type=\"number\" value=\"" + intToString(config.aprsPort) + "\">";
    page += "<button type=\"button\" onclick=\"document.getElementById('aprsServer').value='rotate.aprs2.net';document.getElementById('aprsPort').value='14580';\" style=\"margin-top:8px;padding:7px;width:100%;border:0;border-radius:6px;background:#333;color:#FFB000;font-size:14px;cursor:pointer;\">Reset APRS to defaults</button>";
    page += "<label>" + String(UI_MEAS[L]) + "</label><input id=\"measSeconds\" type=\"number\" min=\"5\" value=\"" + intToString(config.measSeconds) + "\">";
    page += "<label>" + String(UI_LOC[L]) + "</label><input id=\"location\" value=\"" + config.location + "\">";
    page += "<label>NTP server</label><input id=\"ntpServer\" value=\"" + config.ntpServer + "\">";
    page += "<label>" + String(UI_TZ[L]) + "</label><input id=\"tzOffsetHours\" value=\"" + intToString(config.tzOffsetHours) + "\">";
    page += "<label>Limba / Language / Nyelv</label><select id=\"lang\">";
    page += "<option value=\"0\"" + String(config.lang == 0 ? " selected" : "") + ">Romana</option>";
    page += "<option value=\"1\"" + String(config.lang == 1 ? " selected" : "") + ">English</option>";
    page += "<option value=\"2\"" + String(config.lang == 2 ? " selected" : "") + ">Magyar</option>";
    page += "</select>";
    page += "<label>" + String(UI_MAIN[L]) + "</label><input id=\"mainSeconds\" type=\"number\" value=\"" + intToString(config.mainSeconds) + "\">";
    page += "<label>" + String(UI_SLIDE[L]) + "</label><input id=\"slideSeconds\" type=\"number\" value=\"" + intToString(config.slideSeconds) + "\">";
    page += "<label>" + String(UI_GWIN[L]) + "</label><select id=\"graphWindow\">";
    for (int i = 0; i < 4; i++)
      page += "<option value=\"" + intToString(i) + "\"" + String(config.graphWindow == i ? " selected" : "") + ">" + GRAPH_WIN_LABEL3[L][i] + "</option>";
    page += "</select>";
    page += "<label>" + String(UI_BRIGHT[L]) + ": <span id=\"bdv\">" + intToString(config.brightDay) + "</span></label>";
    page += "<input id=\"brightDay\" type=\"range\" min=\"5\" max=\"100\" value=\"" + intToString(config.brightDay) + "\" oninput=\"document.getElementById('bdv').textContent=this.value\">";
    page += "<label><input id=\"schedEnabled\" type=\"checkbox\"" + String(config.schedEnabled ? " checked" : "") + ">" + String(UI_SCHED[L]) + "</label>";
    page += "<label>" + String(UI_BNIGHT[L]) + "</label><input id=\"brightNight\" type=\"number\" min=\"5\" max=\"100\" value=\"" + intToString(config.brightNight) + "\">";
    page += "<label>" + String(UI_FROM[L]) + "</label><input id=\"schedStart\" type=\"number\" min=\"0\" max=\"23\" value=\"" + intToString(config.schedStart) + "\">";
    page += "<label>" + String(UI_TO[L]) + "</label><input id=\"schedEnd\" type=\"number\" min=\"0\" max=\"23\" value=\"" + intToString(config.schedEnd) + "\">";
    page += "<label><input id=\"logEnabled\" type=\"checkbox\"" + String(config.logEnabled ? " checked" : "") + ">Web console log</label>";
    page += "<button type=\"button\" onclick=\"window.open('/weather','_blank')\">Weather</button>";
    page += "<button type=\"button\" onclick=\"window.open('/console','_blank')\">Console</button><span style=\"color:#8a9099;font-size:12px;margin-left:8px;\">(opens in a new page)</span>";
    page += "<button onclick=\"save()\">" + String(UI_SAVE[L]) + "</button>";
    page += "<button id=\"rb\" onclick=\"reboot()\">" + String(UI_REBOOT[L]) + "</button>";
    page += "<p style=\"color:#FFB000;opacity:0.7;font-size:13px;margin-top:14px;text-align:center;\">MiniWX Receiver " APP_VERSION " &mdash; Developed by YO7ZRO</p>";
    page += "<p id=\"status\"></p>";
    page += "<script>";
    page += "function g(){var i=['ssid','pass','bmeHost','bmePath','location','ntpServer','tzOffsetHours','lang','slideSeconds','mainSeconds','graphWindow','brightDay','schedEnabled','brightNight','schedStart','schedEnd','dataSource','aprsCall0','aprsCall1','aprsCall2','aprsActive','aprsLogin','aprsServer','aprsPort','measSeconds','aprsLoc0','aprsLoc1','aprsLoc2','aprsTempOff0','aprsTempOff1','aprsTempOff2','logEnabled'];var p=new URLSearchParams();i.forEach(function(x){var e=document.getElementById(x);p.append(x, e.type==='checkbox'?(e.checked?'1':'0'):e.value);});return p.toString();}";
    page += "function save(){var s=document.getElementById('status');s.style.color='#FFAA3C';s.textContent='" + String(UI_SAVING[L]) + "';";
    page += "fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:g()})";
    page += ".then(function(r){return r.text();}).then(function(){s.style.color='#FFB000';s.textContent='\\u2713 " + String(UI_SAVED[L]) + "';})";
    page += ".catch(function(){s.style.color='#FF5A5A';s.textContent='" + String(UI_SAVEERR[L]) + "';});}";
    page += "function reboot(){if(!confirm('" + String(UI_RBCONF[L]) + "'))return;var s=document.getElementById('status');s.style.color='#FFAA3C';s.textContent='" + String(UI_RBING[L]) + "';fetch('/reboot',{method:'POST'}).catch(function(){});setTimeout(function(){location.reload();},10000);}";
    page += "</script></body></html>";
    server.send(200, "text/html", page);
  });

  server.on("/save", HTTP_POST, []() {
    config.ssid = server.arg("ssid");
    config.pass = server.arg("pass");
    config.bmeHost = server.arg("bmeHost");
    config.bmePath = server.arg("bmePath");
    config.location = server.arg("location");
    config.ntpServer = server.arg("ntpServer");
    config.tzOffsetHours = atoi(server.arg("tzOffsetHours").c_str());
    config.lang = atoi(server.arg("lang").c_str());
    config.slideSeconds = atoi(server.arg("slideSeconds").c_str());
    config.mainSeconds = atoi(server.arg("mainSeconds").c_str());
    config.graphWindow = atoi(server.arg("graphWindow").c_str());
    config.brightDay = atoi(server.arg("brightDay").c_str());
    config.brightNight = atoi(server.arg("brightNight").c_str());
    config.schedEnabled = (server.arg("schedEnabled") == "1");
    config.schedStart = atoi(server.arg("schedStart").c_str());
    config.schedEnd = atoi(server.arg("schedEnd").c_str());
    config.logEnabled = (server.arg("logEnabled") == "1");
    config.dataSource = atoi(server.arg("dataSource").c_str());
    config.aprsCall0 = server.arg("aprsCall0");
    config.aprsCall1 = server.arg("aprsCall1");
    config.aprsCall2 = server.arg("aprsCall2");
    config.aprsActive = atoi(server.arg("aprsActive").c_str());
    config.aprsLogin = server.arg("aprsLogin");
    config.aprsServer = server.arg("aprsServer");
    config.aprsPort = atoi(server.arg("aprsPort").c_str());
    config.measSeconds = atoi(server.arg("measSeconds").c_str());
    config.aprsLoc0 = server.arg("aprsLoc0");
    config.aprsLoc1 = server.arg("aprsLoc1");
    config.aprsLoc2 = server.arg("aprsLoc2");
    config.aprsTempOff0 = server.arg("aprsTempOff0");
    config.aprsTempOff1 = server.arg("aprsTempOff1");
    config.aprsTempOff2 = server.arg("aprsTempOff2");
    applyBrightness();                 // apply immediately (live)
    if (LittleFS.begin()) {
      File f = LittleFS.open(CONFIG_PATH, "w");
      if (f) {
        String j = "{\"ssid\":\"" + config.ssid + "\",\"pass\":\"" + config.pass +
                   "\",\"bmeHost\":\"" + config.bmeHost + "\",\"bmePath\":\"" + config.bmePath +
                   "\",\"location\":\"" + config.location + "\",\"ntpServer\":\"" + config.ntpServer +
                   "\",\"tzOffsetHours\":\"" + intToString(config.tzOffsetHours) +
                   "\",\"lang\":\"" + intToString(config.lang) +
                   "\",\"slideSeconds\":\"" + intToString(config.slideSeconds) +
                   "\",\"mainSeconds\":\"" + intToString(config.mainSeconds) +
                   "\",\"graphWindow\":\"" + intToString(config.graphWindow) +
                   "\",\"brightDay\":\"" + intToString(config.brightDay) +
                   "\",\"brightNight\":\"" + intToString(config.brightNight) +
                   "\",\"schedEnabled\":\"" + String(config.schedEnabled ? 1 : 0) +
                   "\",\"schedStart\":\"" + intToString(config.schedStart) +
                   "\",\"schedEnd\":\"" + intToString(config.schedEnd) +
                   "\",\"dataSource\":\"" + intToString(config.dataSource) +
                   "\",\"aprsCall0\":\"" + config.aprsCall0 +
                   "\",\"aprsCall1\":\"" + config.aprsCall1 +
                   "\",\"aprsCall2\":\"" + config.aprsCall2 +
                   "\",\"aprsActive\":\"" + intToString(config.aprsActive) +
                   "\",\"aprsLogin\":\"" + config.aprsLogin +
                   "\",\"aprsServer\":\"" + config.aprsServer +
                   "\",\"aprsPort\":\"" + intToString(config.aprsPort) + "\",\"measSeconds\":\"" + intToString(config.measSeconds) + "\",\"aprsLoc0\":\"" + config.aprsLoc0 + "\",\"aprsLoc1\":\"" + config.aprsLoc1 + "\",\"aprsLoc2\":\"" + config.aprsLoc2 + "\",\"aprsTempOff0\":\"" + config.aprsTempOff0 + "\",\"aprsTempOff1\":\"" + config.aprsTempOff1 + "\",\"aprsTempOff2\":\"" + config.aprsTempOff2 + "\",\"logEnabled\":\"" + String(config.logEnabled ? 1 : 0) + "\"}";
        f.print(j); f.close();
      }
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/log", HTTP_GET, []() {
    String out;
    out.reserve(logCount * 100);
    for (uint8_t i = 0; i < logCount; i++) {
      uint8_t idx = (logHead + LOG_LINES - logCount + i) % LOG_LINES;
      out += logBuf[idx]; out += "\n";
    }
    if (!config.logEnabled) out += "(log disabled - enable \"Web console log\" in settings and save)\n";
    server.send(200, "text/plain", out);
  });

  server.on("/logclear", HTTP_POST, []() {
    logHead = 0; logCount = 0;
    server.send(200, "text/plain", "OK");
  });

  server.on("/console", HTTP_GET, []() {
    String p = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>MiniWX Receiver Console</title>";
    p += "<style>html{background:#0d0f12;}body{max-width:900px;margin:0 auto;background:#181b20;color:#e8e8e8;font-family:'Segoe UI',Arial,sans-serif;padding:16px;}";
    p += "h2{color:#FFB000;}pre{background:#0d0f12;border:1px solid #2a2f37;border-radius:8px;padding:12px;font-size:12px;line-height:1.45;white-space:pre-wrap;word-break:break-all;min-height:300px;max-height:70vh;overflow-y:auto;}";
    p += "button{font-size:14px;padding:8px 16px;margin:6px 6px 6px 0;background:#1f232a;color:#e8e8e8;border:1px solid #2a2f37;border-radius:8px;cursor:pointer;}button:hover{background:#FFB000;color:#000;}#st{color:#8a9099;font-size:12px;}</style></head><body>";
    p += "<h2>Console</h2><p id='st'></p><pre id='log'>loading...</pre>";
    p += "<button id='pb' onclick='paused=!paused;this.textContent=paused?\"Resume\":\"Pause\";'>Pause</button>";
    p += "<button onclick='fetch(\"/logclear\",{method:\"POST\"}).then(function(){upd(true);});'>Clear</button>";
    p += "<button onclick='window.location=\"/\";'>Back</button>";
    p += "<script>var paused=false;function upd(f){if(paused&&!f)return;fetch('/log').then(function(r){return r.text();}).then(function(t){var e=document.getElementById('log');var atEnd=e.scrollTop+e.clientHeight>=e.scrollHeight-8;e.textContent=t||'(empty)';if(atEnd)e.scrollTop=e.scrollHeight;document.getElementById('st').textContent='auto-refresh 2s';});}upd();setInterval(function(){upd(false);},2000);</script>";
    p += "</body></html>";
    server.send(200, "text/html", p);
  });

  // ---- /wxdata: JSON with all station data (for /weather page) ----
  server.on("/wxdata", HTTP_GET, []() {
    String j = "[";
    int idx[MAXSTA]; int nSta = activeStations(idx);
    for (int k = 0; k < nSta; k++) {
      int i = idx[k];
      if (k) j += ",";
      j += "{";
      if (config.dataSource == 1) {
        AprsStation &a = aprsSt[i];
        j += "\"name\":\"" + buildLocationLabel(i) + "\",";
        j += "\"call\":\"" + aprsCallOf(i) + "\",";
        j += "\"temp\":" + String(a.tC, 2) + ",";
        j += "\"pres\":" + String(a.pres, 1) + ",";
        j += "\"hum\":" + String(a.hum, 0) + ",";
        j += "\"dew\":" + String(aprsDewPoint(a.tC, a.hum), 1) + ",";
        j += "\"feel\":" + String(aprsHeatIndexC(a.tC, a.hum), 1) + ",";
        j += "\"comment\":\"" + a.comment + "\",";
        j += "\"valid\":" + String(a.valid ? 1 : 0) + ",";
        j += "\"age\":" + String(a.valid ? (millis() - a.lastMs) / 1000 : -1);
      } else {
        j += "\"name\":\"" + buildLocationLabel(0) + "\",";
        j += "\"call\":\"\",";
        j += "\"temp\":" + weatherValues[1] + ",";
        j += "\"pres\":" + weatherValues[2] + ",";
        j += "\"hum\":" + weatherValues[3] + ",";
        j += "\"dew\":" + weatherValues[4] + ",";
        j += "\"feel\":" + weatherValues[5] + ",";
        j += "\"comment\":\"\",\"valid\":1,\"age\":0";
      }
      j += "}";
    }
    j += "]";
    server.send(200, "application/json", j);
  });

  // ---- /weather: live weather dashboard ----
  server.on("/weather", HTTP_GET, []() {
    String p = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>MiniWX Receiver Weather</title>";
    p += "<style>html{background:#0d0f12;}body{max-width:1000px;margin:0 auto;background:#0d0f12;color:#e8e8e8;font-family:'Segoe UI',Arial,sans-serif;padding:16px;}";
    p += "h2{color:#FFB000;margin-bottom:4px;}a{color:#FFB000;text-decoration:none;}";
    p += ".sub{color:#8a9099;font-size:13px;margin-bottom:16px;}";
    p += ".sta{background:#181b20;border-radius:10px;padding:14px 18px;margin-bottom:14px;}";
    p += ".sta-h{display:flex;justify-content:space-between;color:#8a9099;font-size:13px;border-bottom:1px solid #2a2f37;padding-bottom:8px;margin-bottom:12px;}";
    p += ".sta-h b{color:#FFB000;font-size:16px;}";
    p += ".grid{display:grid;gap:10px;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));}";
    p += ".c{background:#1f232a;border-radius:8px;padding:12px 14px;}";
    p += ".c .lab{color:#8a9099;font-size:13px;margin-bottom:10px;}";
    p += ".c .val{font-size:28px;font-weight:300;}";
    p += ".cmt{color:#8a9099;font-size:12px;margin-top:8px;font-style:italic;}";
    p += ".foot{color:#8a9099;font-size:12px;text-align:center;margin-top:16px;}";
    p += "button{font-size:14px;padding:8px 16px;margin:6px 6px 6px 0;background:#1f232a;color:#e8e8e8;border:1px solid #2a2f37;border-radius:8px;cursor:pointer;}button:hover{background:#FFB000;color:#000;}</style></head><body>";
    p += "<h2>MiniWX Receiver " APP_VERSION "</h2>";
    p += "<div class='sub'>Live weather data &mdash; auto-refresh 5 s</div>";
    p += "<div id='wx'>loading...</div>";
    p += "<button onclick='window.location=\"/\";'>Settings</button>";
    p += "<button onclick='window.open(\"/console\",\"_blank\");'>Console</button>";
    p += "<div class='foot'>MiniWX Receiver " APP_VERSION " &mdash; Developed by YO7ZRO</div>";
    p += "<script>";
    p += "function upd(){fetch('/wxdata').then(function(r){return r.json();}).then(function(d){";
    p += "var h='';for(var i=0;i<d.length;i++){var s=d[i];";
    p += "h+='<div class=sta><div class=sta-h><b>'+s.name+'</b><span>'+(s.call||'local')+(s.valid?' &middot; '+s.age+'s ago':' &middot; no data')+'</span></div>';";
    p += "h+='<div class=grid>';";
    p += "h+='<div class=c><div class=lab>Temperature (&deg;C)</div><div class=val>'+s.temp.toFixed(2)+'</div></div>';";
    p += "h+='<div class=c><div class=lab>Pressure (hPa)</div><div class=val>'+s.pres.toFixed(1)+'</div></div>';";
    p += "h+='<div class=c><div class=lab>Humidity (%)</div><div class=val>'+s.hum.toFixed(0)+'</div></div>';";
    p += "h+='<div class=c><div class=lab>Dew point (&deg;C)</div><div class=val>'+s.dew.toFixed(1)+'</div></div>';";
    p += "h+='<div class=c><div class=lab>Real feel (&deg;C)</div><div class=val>'+s.feel.toFixed(1)+'</div></div>';";
    p += "h+='</div>';";
    p += "if(s.comment)h+='<div class=cmt>'+s.comment+'</div>';";
    p += "h+='</div>';";
    p += "}document.getElementById('wx').innerHTML=h||'<p>No station data</p>';";
    p += "}).catch(function(){document.getElementById('wx').innerHTML='<p style=color:#FF7A5A>Connection error</p>';});}";
    p += "upd();setInterval(upd,5000);";
    p += "</script></body></html>";
    server.send(200, "text/html", p);
  });

  server.on("/reboot", HTTP_POST, []() {
    server.send(200, "text/plain", "REBOOT");
    rebootPending = true;
    rebootAt = millis() + 400;     // let the HTTP response leave before restart
  });

  server.begin();
}

/* ---------------- Loop ---------------- */
void loop() {
  server.handleClient();
  if (rebootPending && millis() >= rebootAt) ESP.restart();
  if (configMode) { delay(2); return; }

  unsigned long now = millis();

  // --- active stations (APRS: filled callsigns; local: a single one) ---
  int idx[MAXSTA]; int nSta = activeStations(idx);

  // --- slideshow: one main page per station (mainSeconds),
  //     then the graphs overlaid across stations (slideSeconds each) ---
  int graphs = (config.slideSeconds > 0) ? 3 : 0;
  int total  = nSta + graphs;
  bool onMain = (currentSlide < nSta);
  unsigned long curDur = onMain ? (unsigned long)config.mainSeconds
                                : (unsigned long)config.slideSeconds;
  if (curDur < 1) curDur = 1;
  if (total > 1 && now - lastSlideMs >= curDur * 1000UL) {
    lastSlideMs = now;
    currentSlide = (currentSlide + 1) % total;
    slideDrawn = false;
  }
  if (currentSlide >= total) { currentSlide = 0; slideDrawn = false; }
  onMain = (currentSlide < nSta);
  if (onMain) dispSta = idx[currentSlide];

  if (!slideDrawn) { renderSlide(currentSlide); slideDrawn = true; }

  // --- dynamic updates only on the main pages ---
  if (onMain) {
    struct tm t;
    if (getLocalTimeSafe(t)) {
      if (t.tm_min != lastMinShown) {
        drawClock(t);
        drawSubline(t);
        lastMinShown = t.tm_min;
      }
    } else if (lastMinShown != -2) {
      struct tm z; memset(&z, 0, sizeof(z));
      drawClock(z);
      tft.fillRect(0, SUB_Y, 240, 16, C_BG);
      tft.setTextDatum(TC_DATUM);
      tft.setTextColor(C_UNIT, C_BG);
      tft.drawString(L_SYNC[curLang()], 120, SUB_Y, 2);
      lastMinShown = -2;
    }
  }

  // --- brightness recompute on minute change (schedule) ---
  {
    struct tm bt;
    if (getLocalTimeSafe(bt) && bt.tm_min != lastBrightMin) {
      applyBrightness();
      lastBrightMin = bt.tm_min;
    }
  }

  // --- APRS-IS: keep the connection and drain packets (non-blocking) ---
  if (config.dataSource == 1 && WiFi.status() == WL_CONNECTED) {
    aprsEnsureConnected();
    aprsPoll();
  }

  // --- weather + history update (at the read interval) ---
  static unsigned long lastWeather = 0;
  bool srcReady = (config.dataSource == 1)
                    ? (WiFi.status() == WL_CONNECTED)
                    : (WiFi.status() == WL_CONNECTED && config.bmeHost.length() > 0);
  if (srcReady && (lastWeather == 0 || now - lastWeather >= ((unsigned long)(config.measSeconds > 0 ? config.measSeconds : 2) * 1000UL))) {
    lastWeather = now;
    bool any = false;
    if (config.dataSource == 1) {
      for (int k = 0; k < nSta; k++) {            // history for each active station
        int si = idx[k];
        if (aprsSt[si].valid && (millis() - aprsSt[si].lastMs < APRS_STALE_MS)) {
          recordSample(si, aprsSt[si].tC, aprsSt[si].hum, aprsSt[si].pres);
          any = true;
        }
      }
      aprsApplyStation(dispSta);                  // displayed values = current station
    } else {
      pollWeather();
      if (weatherValid) {
        recordSample(0, weatherValues[1].toFloat(), weatherValues[3].toFloat(), weatherValues[2].toFloat());
        any = true;
      }
    }
    if (any && (lastHistSaveMs == 0 || now - lastHistSaveMs >= HIST_SAVE_MS)) {
      lastHistSaveMs = now;
      saveHistory();
    }
    if (onMain) drawAllMetrics();
  }

  delay(2);
}
