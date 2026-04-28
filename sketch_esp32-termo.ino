// ============================================================
// ESP32-C3 Supermini — Муфельная печь v0.3 (MAX31855 only)
// Wi-Fi AP + STA, 50 шагов, 20 программ, Chart.js (LittleFS)
// PID, CSV-лог, 3 кнопки, OLED, WebServer + WebSockets
// ArduinoJson v7 compatible | Fixed HTTP headers | 1050.1 stub
// ============================================================
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PID_v1.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <Adafruit_MAX31855.h>

// 📌 Пины
#define OLED_SDA    1
#define OLED_SCL    9
#define BTN_UP      4
#define BTN_DOWN    5
#define BTN_SEL     6
#define SSR_PIN     7
#define TC_CLK      2
#define TC_CS       10
#define TC_MISO     3

// 📡 Wi-Fi настройки
#define AP_SSID     "Furnace_Config"
#define AP_PASS     "12345678"
#define CONFIG_FILE "/wifi_config.json"
struct WifiConfig { String ssid; String pass; bool isValid; };
WifiConfig wifiCfg = {"", "", false};
bool isApMode = false;

// ⚙️ PID
double Kp = 50.0, Ki = 0.1, Kd = 10.0;
double setpoint = 25.0, input = 25.0, output = 0.0;
PID myPID(&input, &output, &setpoint, Kp, Ki, Kd, DIRECT);

// 📊 Динамическая программа
#define MAX_STEPS 50
struct ProgramStep { float temp; uint16_t duration_min; };
ProgramStep activeSteps[MAX_STEPS];
uint8_t activeStepCount = 0;
bool programLoaded = false;
int8_t currentStepIdx = 0;
unsigned long stepStartTime = 0;

// 📜 Предустановленные программы
#define MAX_PREDEF_PROGS 20
String predefNames[MAX_PREDEF_PROGS];
String predefStrings[MAX_PREDEF_PROGS];
uint8_t predefCount = 0;

// 🌐 Серверы
WebServer server(80);
WebSocketsServer webSocket(81);

// 🖥️ OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// 🌡️ Термодатчик
Adafruit_MAX31855 thermocouple(TC_CLK, TC_MISO, TC_CS);

// ⏱️ Таймеры
unsigned long lastTempRead = 0, lastDisplayUpdate = 0, lastWsUpdate = 0, lastLogTime = 0;
bool programRunning = false, heatingEnabled = false;

// 🛡️ Защита
#define MAX_TEMP_LIMIT 1000.0
#define EMERGENCY_TEMP 1050.0

// 📈 График
#define GRAPH_POINTS 60
float tempHistory[GRAPH_POINTS];
uint8_t historyIdx = 0;

// 📁 Лог
#define LOG_INTERVAL 10000
#define MAX_LOG_SIZE 262144
#define LOG_FILE "/furnace_log.csv"

// ============================================================
String getContentType(String filename) {
  if (filename.endsWith(".html")) return "text/html";
  if (filename.endsWith(".css"))  return "text/css";
  if (filename.endsWith(".js"))   return "application/javascript";
  if (filename.endsWith(".json")) return "application/json";
  if (filename.endsWith(".csv"))  return "text/csv";
  return "text/plain";
}

// 🔧 ГЛАВНОЕ ИСПРАВЛЕНИЕ: streamFile() отправляет ВСЁ сам (статус+заголовки+тело)
void handleFileRequest() {
  if (isApMode) { server.send(404, "text/plain", "Config mode active"); return; }
  String path = server.uri();
  if (path == "/") path = "/index.html";
  if (LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    server.streamFile(file, getContentType(path));  // ✅ Всё в одной строке!
    file.close();
  } else server.send(404, "text/plain", "Not found");
}

void handleLogDownload() {
  if (!LittleFS.exists(LOG_FILE)) { server.send(404, "text/plain", "Log not found"); return; }
  File file = LittleFS.open(LOG_FILE, "r");
  server.sendHeader("Content-Disposition", "attachment; filename=\"furnace_log.csv\"", true);
  server.streamFile(file, "text/csv");  // ✅ streamFile() добавит Content-Type и Content-Length сам
  file.close();
  server.send(200);
}

void handleLogStatus() {
  size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes(), fSize = 0;
  if (LittleFS.exists(LOG_FILE)) { File f = LittleFS.open(LOG_FILE, "r"); fSize = f.size(); f.close(); }
  JsonDocument doc;
  doc["file_kb"] = fSize/1024.0; doc["fs_free_kb"] = (total-used)/1024.0; doc["fs_total_kb"] = total/1024.0;
  String s; serializeJson(doc, s); server.send(200, "application/json", s);
}

void handleLogClear() {
  LittleFS.remove(LOG_FILE);
  File f = LittleFS.open(LOG_FILE, "w");
  f.println("time_sec,temp_c,setpoint_c,output_pct,ssr_state,mode,program_step");
  f.close();
  server.send(200, "text/plain", "OK");
}

bool saveProgramsToFile() {
  JsonDocument doc;
  JsonArray arr = doc["programs"].to<JsonArray>();
  for (uint8_t i = 0; i < predefCount; i++) {
    JsonObject p = arr.add<JsonObject>();
    p["name"] = predefNames[i];
    p["prg"] = predefStrings[i];
  }
  File f = LittleFS.open("/programs.json", "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

void loadPredefinedPrograms() {
  File f = LittleFS.open("/programs.json", "r");
  if (!f) {
    Serial.println("📝 programs.json not found. Creating default...");
    File w = LittleFS.open("/programs.json", "w");
    if (w) {
      w.print(R"raw({"programs":[
{"name":"Отжиг стали","prg":"600,30;850,45;200,180"},
{"name":"Закалка инструментальная","prg":"900,60;550,120;50,300"},
{"name":"Нормализация","prg":"950,45;600,90;200,150"},
{"name":"Отпуск средний","prg":"450,20;450,60;50,120"},
{"name":"Отпуск высокий","prg":"650,30;650,90;50,150"},
{"name":"Керамика обжиг","prg":"200,60;950,120;500,180"},
{"name":"Сушка смолы","prg":"120,90;180,60;50,120"},
{"name":"Плавка алюминия","prg":"700,45;750,30"},
{"name":"Тестовый (быстрый)","prg":"200,5;300,5;200,5"},
{"name":"Калибровка датчика","prg":"232,10;327,10;50,10"},
{"name":"Синтеризация меди","prg":"800,60;900,90;400,120"},
{"name":"Закалка латуни","prg":"650,40;350,80;50,100"},
{"name":"Отжиг бронзы","prg":"550,50;450,70;200,90"},
{"name":"Плавка свинца","prg":"350,30;400,20"},
{"name":"Плавка олова","prg":"250,25;300,20"},
{"name":"Отжиг титана","prg":"750,90;650,120;300,180"},
{"name":"Закалка нержавейки","prg":"1050,60;550,90;100,150"},
{"name":"Стеклянный обжиг","prg":"550,120;750,180;450,240"},
{"name":"Эмаль обжиг","prg":"800,45;750,60;300,90"},
{"name":"Гипс сушка","prg":"150,60;200,90;50,60"}
]})raw");
      w.close();
      Serial.println("✅ Default programs.json created");
      f = LittleFS.open("/programs.json", "r");
    } else {
      Serial.println("❌ Failed to create programs.json");
      return;
    }
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.print("JSON err: "); Serial.println(err.c_str()); return; }
  JsonArray arr = doc["programs"].as<JsonArray>();
  predefCount = 0;
  for (JsonObject p : arr) {
    if (predefCount >= MAX_PREDEF_PROGS) break;
    predefNames[predefCount] = p["name"].as<String>();
    predefStrings[predefCount] = p["prg"].as<String>();
    predefCount++;
  }
  Serial.printf("✅ Loaded %d predefined programs\n", predefCount);
}

void handleGetPrograms() {
  JsonDocument doc;
  JsonArray arr = doc["programs"].to<JsonArray>();
  for(uint8_t i=0; i<predefCount; i++) {
    JsonObject p = arr.add<JsonObject>();
    p["id"] = i;
    p["name"] = predefNames[i];
    p["prg"] = predefStrings[i];
  }
  String resp; serializeJson(doc, resp); server.send(200, "application/json", resp);
}

void handleManageProgram() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Bad request"); return; }
  String j = server.arg("plain");
  JsonDocument d;
  if (deserializeJson(d, j)) { server.send(400, "text/plain", "Invalid JSON"); return; }
  String name = d["name"].as<String>();
  String prg = d["prg"].as<String>();
  int id = d["id"].is<JsonVariant>() ? d["id"].as<int>() : -1;
  if (name.length() == 0 || prg.length() == 0) { server.send(400, "text/plain", "Name and prg required"); return; }
  int start = 0, steps = 0;
  while (start < prg.length() && steps < MAX_STEPS) {
    int end = prg.indexOf(';', start); if (end == -1) end = prg.length();
    String step = prg.substring(start, end); step.trim(); start = end + 1;
    if (step.length() == 0) continue;
    int comma = step.indexOf(',');
    if (comma == -1) { server.send(400, "text/plain", "Invalid step format"); return; }
    float t = step.substring(0, comma).toFloat();
    float m = step.substring(comma + 1).toFloat();
    if (t < 20.0 || t > MAX_TEMP_LIMIT || m <= 0.0) { server.send(400, "text/plain", "Invalid temp/duration"); return; }
    steps++;
  }
  if (steps == 0) { server.send(400, "text/plain", "Empty program"); return; }
  if (id >= 0 && id < predefCount) {
    predefNames[id] = name; predefStrings[id] = prg;
    Serial.printf("🔄 Updated program %d: %s\n", id, name.c_str());
  } else {
    if (predefCount >= MAX_PREDEF_PROGS) { server.send(400, "text/plain", "Max programs reached (20)"); return; }
    predefNames[predefCount] = name; predefStrings[predefCount] = prg; predefCount++;
    Serial.printf("➕ Added program %d: %s\n", predefCount-1, name.c_str());
  }
  if (saveProgramsToFile()) { server.send(200, "text/plain", "OK"); sendWsStatus(); }
  else { server.send(500, "text/plain", "File write failed"); }
}

void handleDeleteProgram() {
  String idStr = server.arg("id");
  if (idStr.length() == 0) { server.send(400, "text/plain", "Missing id"); return; }
  int id = idStr.toInt();
  if (id < 0 || id >= predefCount) { server.send(400, "text/plain", "Invalid ID"); return; }
  String removed = predefNames[id];
  for (int i = id; i < predefCount - 1; i++) {
    predefNames[i] = predefNames[i+1];
    predefStrings[i] = predefStrings[i+1];
  }
  predefCount--;
  Serial.printf("🗑️ Removed program: %s\n", removed.c_str());
  if (saveProgramsToFile()) { server.send(200, "text/plain", "OK"); sendWsStatus(); }
  else { server.send(500, "text/plain", "File write failed"); }
}

void handleLoadProgram() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Bad request"); return; }
  String j = server.arg("plain");
  JsonDocument d;
  if (deserializeJson(d, j)) { server.send(400, "text/plain", "Invalid JSON"); return; }
  String prg;
  if (d["id"].is<JsonVariant>()) {
    uint8_t id = d["id"].as<uint8_t>();
    if (id >= predefCount) { server.send(400, "text/plain", "Invalid ID"); return; }
    prg = predefStrings[id];
  } else if (d["prg"].is<JsonVariant>()) {
    prg = d["prg"].as<String>();
  } else { server.send(400, "text/plain", "Missing id or prg"); return; }
  if (parseProgramString(prg)) {
    currentStepIdx = 0; stepStartTime = millis();
    programLoaded = true; programRunning = true; heatingEnabled = true;
    myPID.SetMode(AUTOMATIC);
    server.send(200, "text/plain", "OK");
    Serial.printf("📥 Program loaded: %d steps\n", activeStepCount);
  } else {
    server.send(400, "text/plain", "Invalid format");
  }
  sendWsStatus();
}

void handleControlRequest() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Bad request"); return; }
  String j = server.arg("plain"); JsonDocument d; deserializeJson(d,j);
  const char* a = d["action"];
  if(strcmp(a,"setpoint")==0){setpoint=d["value"];if(setpoint>MAX_TEMP_LIMIT)setpoint=MAX_TEMP_LIMIT;if(setpoint<20)setpoint=20;}
  else if(strcmp(a,"toggle")==0){heatingEnabled=!heatingEnabled;programRunning=false;myPID.SetMode(heatingEnabled?AUTOMATIC:MANUAL);}
  else if(strcmp(a,"stop")==0){heatingEnabled=false;programRunning=false;digitalWrite(SSR_PIN,LOW);}
  else if(strcmp(a,"pid")==0){Kp=d["kp"];Ki=d["ki"];Kd=d["kd"];myPID.SetTunings(Kp,Ki,Kd);}
  server.send(200, "text/plain", "OK");
  sendWsStatus();
}

void sendJsonStatus() {
  JsonDocument doc;
  doc["temp"]=input; doc["setpoint"]=setpoint; doc["heating"]=heatingEnabled; doc["programRunning"]=programRunning;
  doc["programLoaded"]=programLoaded; doc["programSteps"]=activeStepCount; doc["currentStep"]=currentStepIdx;
  String s; serializeJson(doc,s); server.send(200, "application/json",s);
}

bool parseProgramString(const String& prgStr) {
  activeStepCount = 0;
  int start = 0;
  while (start < prgStr.length() && activeStepCount < MAX_STEPS) {
    int end = prgStr.indexOf(';', start);
    if (end == -1) end = prgStr.length();
    String step = prgStr.substring(start, end);
    step.trim();
    start = end + 1;
    if (step.length() == 0) continue;
    int comma = step.indexOf(',');
    if (comma == -1) return false;
    float t = step.substring(0, comma).toFloat();
    float m = step.substring(comma + 1).toFloat();
    if (t < 20.0 || t > MAX_TEMP_LIMIT || m <= 0.0) return false;
    activeSteps[activeStepCount].temp = t;
    activeSteps[activeStepCount].duration_min = (uint16_t)m;
    activeStepCount++;
  }
  return (activeStepCount > 0);
}

void loadWifiConfig() {
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    wifiCfg.ssid = doc["ssid"].as<String>();
    wifiCfg.pass = doc["pass"].as<String>();
    wifiCfg.isValid = (wifiCfg.ssid.length() > 0);
  }
  f.close();
}

void saveWifiConfig(String ssid, String pass) {
  JsonDocument doc;
  doc["ssid"] = ssid;
  doc["pass"] = pass;
  File f = LittleFS.open(CONFIG_FILE, "w");
  serializeJson(doc, f);
  f.close();
  Serial.printf("💾 WiFi config saved: %s\n", ssid.c_str());
}

void startAP() {
  Serial.println("🔄 Resetting Wi-Fi stack...");
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(300);
  WiFi.mode(WIFI_AP);
  delay(150);
  WiFi.setSleep(false);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  bool apStarted = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
  isApMode = apStarted;
  if (apStarted) {
    Serial.printf("✅ AP broadcast: %s | IP: %s | Ch: 1\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("❌ WiFi.softAP() failed. Check antenna/power.");
  }
}

void handleConfigPage() {
  String html = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>WiFi Config</title>
<style>body{font-family:sans-serif;background:#222;color:#eee;padding:20px;text-align:center}input,select,button{padding:10px;margin:5px;width:80%;max-width:300px}button{background:#0a0;color:#fff;border:none;cursor:pointer}</style></head>
<body><h2>🔌 Furnace Wi-Fi Setup</h2>
<select id="ssidList"><option value="">-- Select Network --</option></select><br>
<input id="ssidInput" placeholder="Or type SSID"><br>
<input id="passInput" type="password" placeholder="Password"><br>
<button onclick="save()">💾 Save & Reboot</button><br>
<button onclick="scan()">🔍 Scan Networks</button>
<p id="msg" style="margin-top:10px;color:#ff0"></p>
<script>
function scan(){fetch('/scan').then(r=>r.json()).then(d=>{const s=document.getElementById('ssidList');s.innerHTML='<option value="">-- Select --</option>';d.networks.forEach(n=>{let o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rss+'dBm)';s.appendChild(o);});}).catch(()=>document.getElementById('msg').innerText='Scan error');}
function save(){const ssid=document.getElementById('ssidList').value||document.getElementById('ssidInput').value;const pass=document.getElementById('passInput').value;if(!ssid)return alert('Enter SSID!');fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass})}).then(r=>r.text()).then(t=>{document.getElementById('msg').innerText=t;});}
</script></body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleScanNetworks() {
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i=0; i<n && i<15; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rss"] = WiFi.RSSI(i);
  }
  String resp; serializeJson(doc, resp); server.send(200, "application/json", resp);
  WiFi.scanDelete();
}

void handleSaveWifi() {
  if (!server.hasArg("plain")) { server.send(400); return; }
  String j = server.arg("plain"); JsonDocument d; deserializeJson(d,j);
  String ssid = d["ssid"]; String pass = d["pass"];
  if (ssid.length() == 0) { server.send(400, "text/plain", "Empty SSID"); return; }
  saveWifiConfig(ssid, pass);
  server.send(200, "text/plain", "Saved! Rebooting...");
  delay(500);
  ESP.restart();
}

// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(SSR_PIN, OUTPUT); digitalWrite(SSR_PIN, LOW);
  pinMode(BTN_UP, INPUT_PULLUP); pinMode(BTN_DOWN, INPUT_PULLUP); pinMode(BTN_SEL, INPUT_PULLUP);
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { Serial.println(F("OLED fail")); while(1); }
  display.setTextColor(SSD1306_WHITE); display.setTextSize(1); display.clearDisplay();
  display.setCursor(0,0); display.println("Furnace Pro v0.3"); display.display(); delay(1000);
  myPID.SetMode(AUTOMATIC); myPID.SetOutputLimits(0, 255); myPID.SetSampleTime(1000);
  readTemperature();
  
  bool fsMounted = LittleFS.begin(false);
  if (!fsMounted) {
    Serial.println("⚠️ LittleFS mount failed. Forcing format...");
    display.clearDisplay(); display.setCursor(0,0);
    display.println("FS CORRUPTED "); display.println("Formatting..."); display.display();
    LittleFS.format(); delay(500);
    fsMounted = LittleFS.begin(true);
  }
  if (!fsMounted) {
    Serial.println("❌ Critical: LittleFS failed after format!");
    Serial.println("➡️ Enable 'Erase All Flash' in IDE and re-flash");
    display.clearDisplay(); display.setCursor(0,0);
    display.println("FLASH ERROR "); display.println("Re-flash with "); display.println("Erase All Flash"); display.display(); while(1) delay(1000);
  }
  
  if (!LittleFS.exists(LOG_FILE)) {
    File f = LittleFS.open(LOG_FILE, "w");
    f.println("time_sec,temp_c,setpoint_c,output_pct,ssr_state,mode,program_step"); f.close();
  }
  loadPredefinedPrograms();
  display.println("WiFi: checking..."); display.display();
  loadWifiConfig();
  if (wifiCfg.isValid) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiCfg.ssid, wifiCfg.pass);
    display.print("Connecting..."); display.display();
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) { delay(500); display.print("."); display.display(); }
  }
  if (WiFi.status() == WL_CONNECTED) {
    isApMode = false;
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    startAP();
  }
  // 🌐 Маршруты
  server.on("/programs",        HTTP_GET, handleGetPrograms);
  server.on("/log/download",    HTTP_GET, handleLogDownload);
  server.on("/log/status",      HTTP_GET, handleLogStatus);
  server.on("/log/clear",       HTTP_POST, handleLogClear);
  server.on("/api/status",      HTTP_GET, sendJsonStatus);
  server.on("/api/control",     HTTP_POST, handleControlRequest);
  server.on("/api/loadProgram", HTTP_POST, handleLoadProgram);
  server.on("/api/programs",    HTTP_GET, handleGetPrograms);
  server.on("/api/programs",    HTTP_POST, handleManageProgram);
  server.on("/api/programs",    HTTP_DELETE, handleDeleteProgram);
  
  if (isApMode) {
    server.on("/",              HTTP_GET, handleConfigPage);
    server.on("/scan",          HTTP_GET, handleScanNetworks);
    server.on("/save",          HTTP_POST, handleSaveWifi);
  } else {
    server.onNotFound(handleFileRequest);
    webSocket.begin();
    webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
      if (type == WStype_CONNECTED) sendWsStatus();
    });
  }
  server.begin();
  display.println(isApMode ? "AP: 192.168.4.1" : "STA: " + WiFi.localIP().toString());
  display.display();
}

// ============================================================
void loop() {
  unsigned long now = millis();
  if (now - lastTempRead >= 1000) { readTemperature(); updateHistory(); lastTempRead = now; }
  if (!isApMode) {
    if (heatingEnabled && !programRunning) { myPID.Compute(); controlSSR(output); }
    else if (programRunning) runProgramStep(now);
    else digitalWrite(SSR_PIN, LOW);
    
    // Аварийная отсечка (срабатывает при 1050.1)
    if (input > EMERGENCY_TEMP) { 
      digitalWrite(SSR_PIN, LOW); 
      heatingEnabled = false; 
      programRunning = false; 
      Serial.println("🚨 EMERGENCY TRIP!");
    }
    
    if (now - lastLogTime >= LOG_INTERVAL) { logFurnaceData(); lastLogTime = now; }
    if (now - lastWsUpdate >= 2000) { sendWsStatus(); lastWsUpdate = now; }
  }
  if (now - lastDisplayUpdate >= 500) { updateDisplay(); lastDisplayUpdate = now; }
  handleButtons();
  server.handleClient();
  if (!isApMode) webSocket.loop();
}

// ============================================================
// 🔧 ЗАГЛУШКА 1050.1 + ДИАГНОСТИКА ОШИБОК MAX31855
float readTemperature() {
  float t = thermocouple.readCelsius();
  if (isnan(t) || t > 1200 || t < -100) {
    uint32_t err = thermocouple.readError();
    Serial.printf("🌡️ MAX31855 Error! Raw: %.2f | Code: 0x%02lX", t, (unsigned long)err);
    if (err & 0x1) Serial.print(" [Open Circuit]");
    if (err & 0x2) Serial.print(" [Short to GND]");
    if (err & 0x4) Serial.print(" [Short to VCC]");
    if (err & 0x8) Serial.print(" [Data Fault]");
    if (err == 0) Serial.print(" [SPI/Comm Timeout]");
    Serial.println();
    
    input = 1050.1; // 🔒 Безопасная заглушка: > EMERGENCY_TEMP
    return input;
  }
  input = t;
  return input;
}

void updateHistory() { tempHistory[historyIdx] = input; historyIdx = (historyIdx + 1) % GRAPH_POINTS; }

void controlSSR(double pidOut) {
  static unsigned long winStart = 0;
  const unsigned long winSize = 2000;
  unsigned long now = millis();
  if (now - winStart >= winSize) winStart = now;
  unsigned long duty = (unsigned long)((pidOut/255.0)*winSize);
  digitalWrite(SSR_PIN, (now-winStart < duty) ? HIGH : LOW);
}

void runProgramStep(unsigned long now) {
  if (!programLoaded || currentStepIdx >= activeStepCount) {
    programRunning = false; heatingEnabled = false; digitalWrite(SSR_PIN, LOW);
    return;
  }
  float elapsedMin = (now - stepStartTime) / 60000.0;
  ProgramStep &s = activeSteps[currentStepIdx];
  if (elapsedMin < s.duration_min) {
    float prevTemp = (currentStepIdx == 0) ? input : activeSteps[currentStepIdx-1].temp;
    float progress = elapsedMin / s.duration_min;
    setpoint = prevTemp + (s.temp - prevTemp) * progress;
  } else {
    currentStepIdx++;
    stepStartTime = now;
  }
  myPID.Compute(); controlSSR(output);
}

void logFurnaceData() {
  File f = LittleFS.open(LOG_FILE, FILE_APPEND); if(!f)return;
  unsigned long t=millis()/1000;
  const char* m = programRunning? "PROG":(heatingEnabled? "MANUAL": "OFF");
  f.printf("%lu,%.1f,%.1f,%.1f,%d,%s,%d\n",t,input,setpoint,output,digitalRead(SSR_PIN),m,currentStepIdx);
  f.close();
  File i = LittleFS.open(LOG_FILE, "r"); size_t sz=i.size(); i.close();
  if(sz>MAX_LOG_SIZE){LittleFS.remove(LOG_FILE);File n=LittleFS.open(LOG_FILE, "w");n.println("time_sec,temp_c,setpoint_c,output_pct,ssr_state,mode,program_step");n.close();}
}

void handleButtons(){
  static bool lst[3]={1,1,1}; static unsigned long lt[3]={0,0,0};
  const uint8_t pins[3]={BTN_UP,BTN_DOWN,BTN_SEL};
  for(uint8_t i=0;i<3;i++){
    bool cur=digitalRead(pins[i]);
    if(cur==LOW && lst[i] && millis()-lt[i]>200){
      lt[i]=millis();
      if(i==0){if(!programRunning) setpoint+=1.0; if(setpoint>MAX_TEMP_LIMIT) setpoint=MAX_TEMP_LIMIT;}
      else if(i==1){if(!programRunning) setpoint-=1.0; if(setpoint<20) setpoint=20;}
      else if(i==2){if(!programRunning){heatingEnabled=!heatingEnabled; myPID.SetMode(heatingEnabled?AUTOMATIC:MANUAL);}}
    }
    lst[i]=cur;
  }
}

void updateDisplay(){
  display.clearDisplay(); display.setCursor(0,0);
  display.println("FURNACE PRO v0.3");
  display.print("T:  "); display.print(input,1); display.println(" C");
  display.print("SP:  "); display.print(setpoint,1); display.println(" C");
  if(programRunning){
    display.print("PROG:  "); display.print(activeStepCount); display.println(" steps");
    display.print("Step:  "); display.print(currentStepIdx+1); display.print("/"); display.println(activeStepCount);
    display.print("->  "); display.print(activeSteps[currentStepIdx].temp); display.println("C");
  } else {
    display.print("Mode:  "); display.println(heatingEnabled? "MANUAL": "STANDBY");
    if(programLoaded) display.println("Prog loaded ✓");
  }
  display.print("PID:  ");display.print(output/255.0*100,0);display.print("% | SSR:  ");display.println(digitalRead(SSR_PIN)? "ON": "OFF");
  display.drawFastHLine(0,56,128,SSD1306_WHITE);
  for(int i=0;i<GRAPH_POINTS;i+=2){int y=63-map(constrain(tempHistory[(historyIdx+i)%GRAPH_POINTS],0,MAX_TEMP_LIMIT),0,MAX_TEMP_LIMIT,0,7);display.drawPixel(i/2,y,SSD1306_WHITE);}
  display.display();
}

void sendWsStatus() {
  JsonDocument doc;
  doc["temp"] = input; doc["setpoint"] = setpoint; doc["output"] = output/255.0*100;
  doc["ssr"] = digitalRead(SSR_PIN); doc["mode"] = programRunning? "PROGRAM":(heatingEnabled? "MANUAL": "STANDBY");
  doc["programLoaded"]=programLoaded; doc["programSteps"]=activeStepCount; doc["currentStep"]=currentStepIdx;
  JsonArray h = doc["tempHistory"].to<JsonArray>();
  for(int i=0;i<GRAPH_POINTS;i++) h.add(tempHistory[(historyIdx+i)%GRAPH_POINTS]);
  String m; serializeJson(doc, m);
  webSocket.broadcastTXT(m);
}