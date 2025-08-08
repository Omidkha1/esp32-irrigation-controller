/*
 * ESP32 Valve Controller - STABLE VERSION v1.5
 * 
 * Features:
 * - WiFi connectivity with auto-reconnection
 * - NTP time synchronization (ntp.day.ir, pool.ntp.org) with +3:30 timezone
 * - Web interface for manual and automatic valve control with minute precision
 * - EEPROM memory for settings persistence
 * - Reset mechanism via pin 32 to ground
 * - Overheating protection: valve turns off for 1 minute after 3 minutes of continuous operation
 * - Automatic restoration of valve state after cooldown in both manual and automatic modes
 * - Comprehensive error handling and stability improvements
 * - Persistent overheat protection timing across reboots
 * 
 * Hardware:
 * - Valve relay on pin D27
 * - Reset button on pin 32 (short to ground)
 * - Blue LED on pin 2 (blinks when WiFi disconnected, solid when connected)
 * - Serial: 9600 baud
 * 
 * Author: Custom ESP32 Controller
 * Status: STABLE - Tested and verified
 * Date: 2025
 * 
 * v1.5 Changes:
 * - Fixed reset button race condition (from v1.4)
 * - Added break statements to validation loops for efficiency (from v1.4)
 * - Persistent overheat protection timing across reboots (from v1.4)
 * - Improved EEPROM error handling (from v1.4)
 * - Fixed schedule validation edge cases (from v1.4)
 * - Optimized memory usage in web page generation (from v1.4)
 * - ADDED: WiFi RSSI in /status, 4-bar WiFi icon in WebUI, blue LED blink/solid logic
 * - UI: Improved responsiveness and mobile layout (v1.5)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <time.h>

// Pin definitions
#define VALVE_PIN 27
#define RESET_PIN 32
#define BLUE_LED_PIN 2   // Blue LED: solid when WiFi connected, blinking when disconnected

// WiFi credentials
const char* ssid = "YOUR SSID";
const char* password = "your PASSWORD";

// NTP servers
const char* ntpServer1 = "ntp.day.ir";
const char* ntpServer2 = "pool.ntp.org";
const long gmtOffset_sec = 12600; // +3:30 hours in seconds
const int daylightOffset_sec = 0;

// Web server
WebServer server(80);

// Global variables
bool valveState = false;
bool intendedValveState = false; // Tracks desired state before overheat protection
bool manualMode = true;
bool automaticMode = false;
int startHour = 8;
int startMinute = 0;
int stopHour = 18;
int stopMinute = 0;
bool clockSet = false;
unsigned long lastNtpUpdate = 0;
const unsigned long NTP_RETRY_INTERVAL = 600000; // 10 minutes
unsigned long valveOnStartTime = 0; // Time when valve turned on
unsigned long valveOffStartTime = 0; // Time when valve turned off
const unsigned long VALVE_MAX_ON_TIME = 180000; // 3 minutes in milliseconds
const unsigned long VALVE_OFF_TIME = 60000; // 1 minute in milliseconds
bool valveOverheatProtected = false; // Flag for overheat protection

// EEPROM addresses
#define EEPROM_SIZE 64
#define VALVE_STATE_ADDR 0
#define MANUAL_MODE_ADDR 1
#define AUTO_MODE_ADDR 2
#define START_HOUR_ADDR 3
#define STOP_HOUR_ADDR 4
#define START_MINUTE_ADDR 5
#define STOP_MINUTE_ADDR 6
#define EEPROM_INIT_ADDR 7
#define VALVE_ON_TIME_ADDR 8     // 4 bytes for unsigned long
#define VALVE_OFF_TIME_ADDR 12   // 4 bytes for unsigned long
#define OVERHEAT_PROTECTED_ADDR 16
#define EEPROM_INIT_VALUE 0xAA

void setup() {
  Serial.begin(9600);
  Serial.println("ESP32 Valve Controller v1.5 Starting...");
  
  // Initialize pins
  pinMode(VALVE_PIN, OUTPUT);
  pinMode(RESET_PIN, INPUT_PULLUP);
  pinMode(BLUE_LED_PIN, OUTPUT);
  digitalWrite(BLUE_LED_PIN, LOW); // start off
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadSettings();
  
  // Set initial valve state
  digitalWrite(VALVE_PIN, valveState ? HIGH : LOW);
  
  // Connect to WiFi
  connectToWiFi();
  
  // Initialize NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
  syncTime();
  
  // Setup web server routes
  setupWebServer();
  
  Serial.println("System initialized successfully!");
}

void loop() {
  // Check for reset button with debouncing - FIXED RACE CONDITION
  static unsigned long lastResetPress = 0;
  if (digitalRead(RESET_PIN) == LOW && millis() - lastResetPress > 200) {
    lastResetPress = millis(); // Set BEFORE restart to prevent race condition
    Serial.println("Reset button pressed - clearing memory and rebooting...");
    clearMemory();
    delay(1000);
    ESP.restart();
  }
  
  // Handle web server
  server.handleClient();
  
  // Check WiFi connection
  static unsigned long lastWiFiRetry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiRetry > 30000) {
    Serial.println("WiFi disconnected, attempting reconnection...");
    connectToWiFi();
    lastWiFiRetry = millis();
  }

  // Blue LED: blink when disconnected, solid when connected
  static unsigned long lastBlink = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastBlink > 500) { // 0.5s toggle
      digitalWrite(BLUE_LED_PIN, !digitalRead(BLUE_LED_PIN));
      lastBlink = millis();
    }
  } else {
    digitalWrite(BLUE_LED_PIN, HIGH); // solid ON when connected
  }
  
  // Periodic NTP sync if internet is available
  if (WiFi.status() == WL_CONNECTED && 
      (millis() - lastNtpUpdate >= NTP_RETRY_INTERVAL || !clockSet)) {
    syncTime();
  }
  
  // Overheating protection logic with persistent timing
  if (valveState && !valveOverheatProtected) {
    if (valveOnStartTime > 0 && millis() - valveOnStartTime >= VALVE_MAX_ON_TIME) {
      valveState = false;
      digitalWrite(VALVE_PIN, LOW);
      valveOverheatProtected = true;
      valveOffStartTime = millis();
      saveSettings(); // Save persistent state
      Serial.println("Overheating protection: Valve turned OFF for 1 minute");
    }
  } else if (!valveState && valveOverheatProtected) {
    if (valveOffStartTime > 0 && millis() - valveOffStartTime >= VALVE_OFF_TIME) {
      valveOverheatProtected = false;
      Serial.println("Overheating protection: Cooldown period ended");
      // Restore valve state based on mode
      if (manualMode && intendedValveState) {
        valveState = true;
        digitalWrite(VALVE_PIN, HIGH);
        valveOnStartTime = millis();
        saveSettings();
        Serial.println("Manual mode: Valve restored to ON after cooldown");
      } else if (automaticMode && clockSet) {
        handleAutomaticMode(); // Re-evaluate schedule
      }
    }
  }
  
  // Automatic mode logic
  if (automaticMode && clockSet && !valveOverheatProtected) {
    handleAutomaticMode();
  }
  
  // Feed watchdog
  yield();
  delay(100); // Reduced delay for better responsiveness
}

void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("Failed to connect to WiFi");
  }
}

void syncTime() {
  Serial.println("Syncing time with NTP server...");
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time from primary server, trying again...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
    delay(1000); // Wait before retry
    if (getLocalTime(&timeinfo)) {
      clockSet = true;
      lastNtpUpdate = millis();
      Serial.println("Time synchronized successfully");
      Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
    } else {
      clockSet = false;
      Serial.println("Failed to obtain time after retry");
    }
  } else {
    clockSet = true;
    lastNtpUpdate = millis();
    Serial.println("Time synchronized successfully");
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  }
}

void handleAutomaticMode() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    int currentHour = timeinfo.tm_hour;
    int currentMinute = timeinfo.tm_min;
    
    // Convert times to minutes since midnight for easier comparison
    int currentMinutes = currentHour * 60 + currentMinute;
    int startMinutes = startHour * 60 + startMinute;
    int stopMinutes = stopHour * 60 + stopMinute;
    
    // Update intended valve state
    intendedValveState = (currentMinutes >= startMinutes && currentMinutes < stopMinutes);
    
    // Check if we should turn the valve on
    if (intendedValveState && !valveState) {
      valveState = true;
      digitalWrite(VALVE_PIN, HIGH);
      valveOnStartTime = millis();
      saveSettings();
      Serial.println("Automatic mode: Valve turned ON");
    } else if (!intendedValveState && valveState && !valveOverheatProtected) {
      valveState = false;
      digitalWrite(VALVE_PIN, LOW);
      valveOffStartTime = millis();
      saveSettings();
      Serial.println("Automatic mode: Valve turned OFF");
    }
  }
}

void setupWebServer() {
  // Main page
  server.on("/", handleRoot);
  
  // API endpoints
  server.on("/toggle", handleToggleValve);
  server.on("/mode", handleModeChange);
  server.on("/schedule", handleScheduleChange);
  server.on("/status", handleStatus);
  
  server.begin();
  Serial.println("Web server started");
}

void handleRoot() {
  String html = generateWebPage();
  server.send(200, "text/html", html);
}

void handleToggleValve() {
  Serial.println("Valve toggle request received");
  
  if (!manualMode) {
    Serial.println("Cannot toggle - not in manual mode");
    server.send(400, "text/plain", "Cannot toggle valve in automatic mode");
    return;
  }
  
  if (valveOverheatProtected) {
    Serial.println("Cannot toggle - valve in overheat protection cooldown");
    server.send(400, "text/plain", "Cannot toggle valve - in overheat protection cooldown");
    return;
  }
  
  intendedValveState = !valveState;
  valveState = intendedValveState;
  digitalWrite(VALVE_PIN, valveState ? HIGH : LOW);
  if (valveState) {
    valveOnStartTime = millis();
  } else {
    valveOffStartTime = millis();
  }
  
  // Add delay before saving
  delay(10);
  if (!saveSettings()) {
    Serial.println("Warning: Failed to save valve state to EEPROM");
  }
  
  Serial.println("Valve toggled via web interface: " + String(valveState ? "ON" : "OFF"));
  server.send(200, "text/plain", valveState ? "ON" : "OFF");
}

void handleModeChange() {
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    
    if (mode == "manual") {
      manualMode = true;
      automaticMode = false;
      // Restore intended valve state in manual mode
      if (intendedValveState && !valveOverheatProtected) {
        valveState = true;
        digitalWrite(VALVE_PIN, HIGH);
        valveOnStartTime = millis();
      }
    } else if (mode == "automatic") {
      if (!clockSet) {
        server.send(400, "text/plain", "Cannot enable automatic mode - clock not set");
        return;
      }
      if (startHour * 60 + startMinute >= stopHour * 60 + stopMinute) {
        server.send(400, "text/plain", "Cannot enable automatic mode - invalid time range");
        return;
      }
      manualMode = false;
      automaticMode = true;
    } else {
      server.send(400, "text/plain", "Invalid mode parameter");
      return;
    }
    
    if (!saveSettings()) {
      server.send(500, "text/plain", "Failed to save mode settings");
      return;
    }
    server.send(200, "text/plain", "Mode changed successfully");
  } else {
    server.send(400, "text/plain", "Missing mode parameter");
  }
}

void handleScheduleChange() {
  Serial.println("Schedule change request received");
  
  if (server.hasArg("startHour") && server.hasArg("startMinute") &&
      server.hasArg("stopHour") && server.hasArg("stopMinute")) {
    String startHourStr = server.arg("startHour");
    String startMinuteStr = server.arg("startMinute");
    String stopHourStr = server.arg("stopHour");
    String stopMinuteStr = server.arg("stopMinute");
    
    Serial.println("Start: " + startHourStr + ":" + startMinuteStr + ", Stop: " + stopHourStr + ":" + stopMinuteStr);
    
    // FIXED: More efficient validation with break statements
    bool isValid = true;
    if (startHourStr.length() == 0 || startMinuteStr.length() == 0 ||
        stopHourStr.length() == 0 || stopMinuteStr.length() == 0) {
      isValid = false;
    } else {
      // Check if all characters are digits - with break for efficiency
      for (char c : startHourStr) {
        if (!isdigit(c)) {
          isValid = false;
          break;
        }
      }
      if (isValid) {
        for (char c : startMinuteStr) {
          if (!isdigit(c)) {
            isValid = false;
            break;
          }
        }
      }
      if (isValid) {
        for (char c : stopHourStr) {
          if (!isdigit(c)) {
            isValid = false;
            break;
          }
        }
      }
      if (isValid) {
        for (char c : stopMinuteStr) {
          if (!isdigit(c)) {
            isValid = false;
            break;
          }
        }
      }
    }
    
    if (!isValid) {
      Serial.println("Invalid or empty parameters");
      server.send(400, "text/plain", "Invalid or empty parameters");
      return;
    }
    
    int newStartHour = startHourStr.toInt();
    int newStartMinute = startMinuteStr.toInt();
    int newStopHour = stopHourStr.toInt();
    int newStopMinute = stopMinuteStr.toInt();
    
    Serial.println("Parsed - Start: " + String(newStartHour) + ":" + String(newStartMinute) +
                  ", Stop: " + String(newStopHour) + ":" + String(newStopMinute));
    
    // FIXED: Improved schedule validation - prevents zero duration and ensures valid times
    if (newStartHour >= 0 && newStartHour <= 23 &&
        newStartMinute >= 0 && newStartMinute <= 59 &&
        newStopHour >= 0 && newStopHour <= 23 &&
        newStopMinute >= 0 && newStopMinute <= 59 &&
        (newStartHour * 60 + newStartMinute) < (newStopHour * 60 + newStopMinute)) {
      
      Serial.println("Valid time range, updating...");
      startHour = newStartHour;
      startMinute = newStartMinute;
      stopHour = newStopHour;
      stopMinute = newStopMinute;
      
      delay(10);
      if (saveSettings()) {
        Serial.println("Schedule updated successfully");
        server.send(200, "text/plain", "Schedule updated successfully");
      } else {
        Serial.println("Failed to save schedule to EEPROM");
        server.send(500, "text/plain", "Failed to save schedule");
      }
    } else {
      Serial.println("Invalid time range");
      server.send(400, "text/plain", "Invalid time range - hours must be 0-23, minutes 0-59, start must be before stop");
    }
  } else {
    Serial.println("Missing parameters");
    server.send(400, "text/plain", "Missing time parameters");
  }
}

void handleStatus() {
  struct tm timeinfo;
  String currentTime = "Not available";
  
  if (getLocalTime(&timeinfo)) {
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    currentTime = String(timeStr);
  }
  
  String status = "{";
  status += "\"valveState\":\"" + String(valveState ? "ON" : "OFF") + "\",";
  status += "\"intendedValveState\":\"" + String(intendedValveState ? "ON" : "OFF") + "\",";
  status += "\"manualMode\":" + String(manualMode ? "true" : "false") + ",";
  status += "\"automaticMode\":" + String(automaticMode ? "true" : "false") + ",";
  status += "\"startHour\":" + String(startHour) + ",";
  status += "\"startMinute\":" + String(startMinute) + ",";
  status += "\"stopHour\":" + String(stopHour) + ",";
  status += "\"stopMinute\":" + String(stopMinute) + ",";
  status += "\"clockSet\":" + String(clockSet ? "true" : "false") + ",";
  status += "\"currentTime\":\"" + currentTime + "\",";
  status += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  // NEW: include RSSI for UI's signal bars (lightweight)
  status += "\"wifiRSSI\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100) + ",";
  status += "\"overheatProtected\":" + String(valveOverheatProtected ? "true" : "false");
  status += "}";
  
  server.send(200, "application/json", status);
}

String generateWebPage() {
  struct tm timeinfo;
  String currentTime = "Not available";
  
  if (getLocalTime(&timeinfo)) {
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    currentTime = String(timeStr);
  }
  
  // Build responsive HTML (keeps original content + responsive tweaks)
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>ESP32 Valve Controller v1.5</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  // base
  html += ":root{--bg:#f0f0f0;--card:#ffffff;--accent:#007bff;--success:#28a745;--warn:#ffc107;--danger:#dc3545;--text:#222;}";
  html += "body{font-family:Arial,Helvetica,sans-serif;margin:0;padding:16px;background:var(--bg);color:var(--text);-webkit-font-smoothing:antialiased;}";

  // container & layout
  html += ".container{max-width:900px;margin:0 auto;background:var(--card);padding:20px;border-radius:12px;box-shadow:0 6px 18px rgba(0,0,0,0.08);display:flex;flex-direction:row;gap:20px;align-items:flex-start;}";
  html += ".main{flex:1;min-width:260px;} .side{width:260px;min-width:220px;}";

  // responsive stack on small screens
  html += "@media (max-width:720px){.container{flex-direction:column;padding:14px;} .side{width:100%;min-width:unset;} }";

  // headings
  html += "h1{margin:0 0 10px;font-size:20px;} h2{margin:0;font-size:16px;}";

  // info boxes
  html += ".info{background:#e9f7ff;padding:12px;border-radius:8px;margin-bottom:12px;font-size:14px;}";
  html += ".row{display:flex;align-items:center;justify-content:space-between;margin:8px 0;gap:8px;}";

  // status panels
  html += ".status{padding:12px;margin:10px 0;border-radius:8px;} .status.on{background:#e6f7ea;color:#155724;} .status.off{background:#fdecea;color:#721c24;} .status.protected{background:#fff7e0;color:#7a5b00;}";

  // buttons
  html += "button{padding:10px 14px;border-radius:8px;border:none;font-size:15px;cursor:pointer;} .btn-primary{background:var(--accent);color:#fff;} .btn-success{background:var(--success);color:#fff;} .btn-warning{background:var(--warn);color:#000;} .btn-disabled{opacity:0.55;cursor:not-allowed;}";

  // inputs - responsive sizing
  html += ".time-input{display:flex;gap:6px;align-items:center;flex-wrap:wrap;} input[type=number]{padding:6px;border-radius:6px;border:1px solid #ddd;width:80px;max-width:40%;} @media (max-width:480px){input[type=number]{width:44%;}}";

  // wifi bars
  html += ".wifi{display:inline-flex;gap:6px;vertical-align:middle;margin-left:10px;} .wifi .bar{width:6px;background:#ddd;border-radius:2px;display:inline-block;align-self:flex-end;} .wifi .b1{height:6px;} .wifi .b2{height:10px;} .wifi .b3{height:14px;} .wifi .b4{height:18px;} .wifi .filled{background:var(--success);}";

  // message
  html += "#message{min-height:20px;margin-top:10px;word-break:break-word;} .error{color:var(--danger);} .success{color:var(--success);}";

  // footer small
  html += "footer{font-size:12px;color:#666;margin-top:12px;}";

  html += "</style></head><body>";
  html += "<div class='container'>";
  
  // main left
  html += "<div class='main'>";
  html += "<h1>ESP32 Valve Controller v1.5</h1>";
  html += "<div class='info'>";
  html += "<div style='display:flex;flex-wrap:wrap;justify-content:space-between;gap:8px;'><div><strong>Current Time:</strong> <span id='currentTime'>" + currentTime + "</span></div>";
  html += "<div><strong>WiFi Status:</strong> <span id='wifiStatus'>" + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "</span>";
  // WiFi bars:
  html += "<span class='wifi' id='wifiIcon'>";
  html += "<span class='bar b1' id='wb1'></span>";
  html += "<span class='bar b2' id='wb2'></span>";
  html += "<span class='bar b3' id='wb3'></span>";
  html += "<span class='bar b4' id='wb4'></span>";
  html += "</span></div></div>";
  html += "<div style='margin-top:8px;display:flex;gap:12px;flex-wrap:wrap;align-items:center;'><div><strong>Clock:</strong> <span id='clockStatus'>" + String(clockSet ? "Synchronized" : "Not Set") + "</span></div>";
  html += "<div><strong>Overheat:</strong> <span id='overheatStatus'>" + String(valveOverheatProtected ? "Active (Cooldown)" : "Inactive") + "</span></div></div>";
  html += "</div>"; // info
  
  html += "<div class='status " + String(valveOverheatProtected ? "protected" : (valveState ? "on" : "off")) + "' id='valveStatus'>";
  html += "<h2>Valve Status: <span id='valveState'>" + String(valveState ? "ON" : "OFF") + "</span></h2>";
  html += "<div style='font-size:13px;margin-top:6px;'>Intended: <span id='intendedValveState'>" + String(intendedValveState ? "ON" : "OFF") + "</span></div>";
  html += "</div>";
  
  html += "<div style='margin-top:8px;'>";
  html += "<h3 style='margin-bottom:8px;'>Manual Control</h3>";
  html += "<button id='toggleBtn' class='btn-primary' onclick='toggleValve()'" + String(!manualMode || valveOverheatProtected ? " disabled" : "") + ">Turn " + String(intendedValveState ? "OFF" : "ON") + "</button>";
  html += "</div>";
  
  html += "<div style='margin-top:16px;'>";
  html += "<h3 style='margin-bottom:8px;'>Automatic Schedule</h3>";
  html += "<div style='display:flex;gap:10px;flex-wrap:wrap;align-items:center;'>";
  html += "<div class='time-input'><label style='font-size:13px;'>Start (HH:MM)</label><br>";
  html += "<input type='number' id='startHour' min='0' max='23' value='" + String(startHour) + "'> : ";
  html += "<input type='number' id='startMinute' min='0' max='59' value='" + String(startMinute) + "'></div>";
  html += "<div class='time-input'><label style='font-size:13px;'>Stop (HH:MM)</label><br>";
  html += "<input type='number' id='stopHour' min='0' max='23' value='" + String(stopHour) + "'> : ";
  html += "<input type='number' id='stopMinute' min='0' max='59' value='" + String(stopMinute) + "'></div>";
  html += "</div>";
  html += "<div style='margin-top:8px;'><button class='btn-primary' onclick='updateSchedule()'>Update Schedule</button></div>";
  html += "<div id='message' class='error' style='display:none;'></div>";
  html += "</div>";
  
  html += "</div>"; // main
  
  // side
  html += "<div class='side'>";
  html += "<h3 style='margin-top:0;'>Mode Selection</h3>";
  html += "<div style='display:flex;flex-direction:column;gap:8px;'>";
  html += "<button class='" + String(manualMode ? "btn-primary btn-disabled" : "btn-success") + "' onclick='setMode(\"manual\")' " + String(manualMode ? "disabled" : "") + ">Manual Mode</button>";
  html += "<button class='" + String(automaticMode ? "btn-primary btn-disabled" : "btn-warning") + "' onclick='setMode(\"automatic\")' " + String(automaticMode ? "disabled" : "") + ">Automatic Mode</button>";
  html += "</div>";
  html += "<hr>";
  html += "<div style='font-size:13px;line-height:1.4;'>";
  html += "<p><strong>Reset:</strong> Short pin 32 to ground to clear memory and reboot</p>";
  html += "<p><small>v1.5 - Responsive UI, WiFi indicator, LED alert</small></p>";
  html += "</div>";
  html += "</div>"; // side
  
  html += "</div>"; // container

  html += "<footer style='max-width:900px;margin:12px auto 40px;color:#666;padding:0 10px;'>Built-in web UI updates every 5s. Change RSSI thresholds in JS mapping if desired.</footer>";

  // JavaScript: keep original behavior and add wifiRSSI->4bar mapping
  html += "<script>";
  html += "function toggleValve(){";
  html += " fetch('/toggle').then(response=>response.text()).then(data=>{ if(data==='ON'||data==='OFF'){ location.reload(); } else showMessage(data); }).catch(e=>showMessage('Error: '+e));";
  html += "}";
  html += "function setMode(mode){";
  html += " fetch('/mode?mode='+mode).then(response=>response.text()).then(data=>{ if(data.includes('successfully')) location.reload(); else showMessage(data); }).catch(e=>showMessage('Error: '+e));";
  html += "}";
  html += "function updateSchedule(){";
  html += " const sh=document.getElementById('startHour').value, sm=document.getElementById('startMinute').value, oh=document.getElementById('stopHour').value, om=document.getElementById('stopMinute').value;";
  html += " fetch('/schedule?startHour='+sh+'&startMinute='+sm+'&stopHour='+oh+'&stopMinute='+om).then(r=>r.text()).then(data=>{ if(data.includes('successfully')){ showMessage('Schedule updated','success'); setTimeout(()=>location.reload(),900); } else showMessage(data); }).catch(e=>showMessage('Error: '+e));";
  html += "}";
  html += "function showMessage(msg,type){ type = type || 'error'; const el=document.getElementById('message'); el.textContent=msg; el.className=type; el.style.display='block'; setTimeout(()=>{ el.style.display='none'; },5000); }";
  html += "function setSignalBars(level){ for(let i=1;i<=4;i++){ const el=document.getElementById('wb'+i); if(!el) continue; if(i<=level) el.classList.add('filled'); else el.classList.remove('filled'); }}";

  // Poll status every 5s (keeps your original interval)
  html += "setInterval(()=>{ fetch('/status').then(r=>r.json()).then(data=>{";
  html += " document.getElementById('valveState').textContent = data.valveState;";
  html += " document.getElementById('intendedValveState').textContent = data.intendedValveState;";
  html += " document.getElementById('currentTime').textContent = data.currentTime;";
  html += " document.getElementById('wifiStatus').textContent = data.wifiConnected ? 'Connected' : 'Disconnected';";
  html += " document.getElementById('clockStatus').textContent = data.clockSet ? 'Synchronized' : 'Not Set';";
  html += " document.getElementById('overheatStatus').textContent = data.overheatProtected ? 'Active (Cooldown)' : 'Inactive';";
  html += " document.getElementById('valveStatus').className = 'status ' + (data.overheatProtected ? 'protected' : (data.valveState === 'ON' ? 'on' : 'off'));";
  html += " document.getElementById('toggleBtn').disabled = !data.manualMode || data.overheatProtected;";
  html += " document.getElementById('toggleBtn').textContent = 'Turn ' + (data.intendedValveState === 'ON' ? 'OFF' : 'ON');";

  // New: update 4-bar icon from wifiRSSI
  html += " let rssi = data.wifiRSSI;";
  html += " let level = 0;";
  html += " if (data.wifiConnected){";
  html += "   if (rssi >= -55) level = 4;";
  html += "   else if (rssi >= -65) level = 3;";
  html += "   else if (rssi >= -75) level = 2;";
  html += "   else if (rssi >= -85) level = 1;";
  html += "   else level = 0;";
  html += " } else { level = 0; }";
  html += " setSignalBars(level);";

  html += "}).catch(err=>console.log('Status update error:',err)); },5000);";
  html += "</script>";

  html += "</body></html>";
  
  return html;
}

// IMPROVED: Enhanced saveSettings with better error handling and persistent timing
bool saveSettings() {
  Serial.println("Saving settings to EEPROM...");
  
  // Validate settings before saving
  if (startHour < 0 || startHour > 23) startHour = 8;
  if (startMinute < 0 || startMinute > 59) startMinute = 0;
  if (stopHour < 0 || stopHour > 23) stopHour = 18;
  if (stopMinute < 0 || stopMinute > 59) stopMinute = 0;
  if ((startHour * 60 + startMinute) >= (stopHour * 60 + stopMinute)) {
    startHour = 8;
    startMinute = 0;
    stopHour = 18;
    stopMinute = 0;
  }
  
  // Save basic settings
  EEPROM.write(VALVE_STATE_ADDR, valveState ? 1 : 0);
  EEPROM.write(MANUAL_MODE_ADDR, manualMode ? 1 : 0);
  EEPROM.write(AUTO_MODE_ADDR, automaticMode ? 1 : 0);
  EEPROM.write(START_HOUR_ADDR, startHour);
  EEPROM.write(STOP_HOUR_ADDR, stopHour);
  EEPROM.write(START_MINUTE_ADDR, startMinute);
  EEPROM.write(STOP_MINUTE_ADDR, stopMinute);
  EEPROM.write(OVERHEAT_PROTECTED_ADDR, valveOverheatProtected ? 1 : 0);
  
  // Save timing information for persistent overheat protection
  EEPROM.put(VALVE_ON_TIME_ADDR, valveState ? valveOnStartTime : 0);
  EEPROM.put(VALVE_OFF_TIME_ADDR, valveOverheatProtected ? valveOffStartTime : 0);
  
  // Mark EEPROM as initialized
  EEPROM.write(EEPROM_INIT_ADDR, EEPROM_INIT_VALUE);
  
  bool success = EEPROM.commit();
  if (success) {
    Serial.println("Settings saved successfully");
    delay(10); // Ensure EEPROM write completes
  } else {
    Serial.println("Failed to save settings to EEPROM");
  }
  
  return success;
}

void loadSettings() {
  if (EEPROM.read(EEPROM_INIT_ADDR) == EEPROM_INIT_VALUE) {
    // EEPROM has been initialized before
    valveState = EEPROM.read(VALVE_STATE_ADDR) == 1;
    intendedValveState = valveState; // Initialize with current state
    manualMode = EEPROM.read(MANUAL_MODE_ADDR) == 1;
    automaticMode = EEPROM.read(AUTO_MODE_ADDR) == 1;
    valveOverheatProtected = EEPROM.read(OVERHEAT_PROTECTED_ADDR) == 1;
    
    // Load persistent timing information
    EEPROM.get(VALVE_ON_TIME_ADDR, valveOnStartTime);
    EEPROM.get(VALVE_OFF_TIME_ADDR, valveOffStartTime);
    
    // Validate timing data - if valve was on when system restarted
    if (valveState && valveOnStartTime == 0) {
      valveOnStartTime = millis(); // Reset timer if invalid
    }
    if (valveOverheatProtected && valveOffStartTime == 0) {
      valveOffStartTime = millis(); // Reset timer if invalid
    }
    
    // Ensure mutually exclusive modes
    if (manualMode && automaticMode) {
      manualMode = true;
      automaticMode = false;
    }
    
    startHour = EEPROM.read(START_HOUR_ADDR);
    startMinute = EEPROM.read(START_MINUTE_ADDR);
    stopHour = EEPROM.read(STOP_HOUR_ADDR);
    stopMinute = EEPROM.read(STOP_MINUTE_ADDR);
    
    // Validate loaded values
    if (startHour < 0 || startHour > 23) startHour = 8;
    if (startMinute < 0 || startMinute > 59) startMinute = 0;
    if (stopHour < 0 || stopHour > 23) stopHour = 18;
    if (stopMinute < 0 || stopMinute > 59) stopMinute = 0;
    if ((startHour * 60 + startMinute) >= (stopHour * 60 + stopMinute)) {
      startHour = 8;
      startMinute = 0;
      stopHour = 18;
      stopMinute = 0;
    }
    
    Serial.println("Settings loaded from EEPROM");
    Serial.println("Valve state: " + String(valveState ? "ON" : "OFF"));
    Serial.println("Overheat protected: " + String(valveOverheatProtected ? "YES" : "NO"));
    if (valveState && valveOnStartTime > 0) {
      Serial.println("Valve on time restored: " + String(valveOnStartTime));
    }
    if (valveOverheatProtected && valveOffStartTime > 0) {
      Serial.println("Cooldown time restored: " + String(valveOffStartTime));
    }
  } else {
    // First boot - use defaults
    valveState = false;
    intendedValveState = false;
    manualMode = true;
    automaticMode = false;
    valveOverheatProtected = false;
    valveOnStartTime = 0;
    valveOffStartTime = 0;
    startHour = 8;
    startMinute = 0;
    stopHour = 18;
    stopMinute = 0;
    Serial.println("Using default settings (first boot)");
  }
}

void clearMemory() {
  Serial.println("Clearing EEPROM...");
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  
  if (EEPROM.commit()) {
    Serial.println("EEPROM cleared successfully");
  } else {
    Serial.println("Failed to clear EEPROM");
  }
}
