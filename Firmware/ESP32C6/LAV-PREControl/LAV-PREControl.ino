#include <Preferences.h>
#define DECODE_DISTANCE
#define TOLERANCE_PERCENTAGE 30
#include <IRremote.hpp>
#include <WiFi.h>
#include <WebServer.h>      
#include <WebSocketsServer.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <PubSubClient.h> 
#include <Update.h>
#include "esp_wifi.h"
#include "esp_pm.h"
#include <ESPmDNS.h>
#include "github_ota.h"

// --- 物件實例 ---
Preferences prefAudio;
Preferences prefWifi;
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81); // 監聽 Port 81
DNSServer dnsServer;
volatile unsigned long lastWebActivityTick = 0;
volatile bool isWebHighPerformance = false;

// 事件驅動狀態旗標
volatile bool wifiConnected = false; 
volatile bool wifiShouldReconnect = false; // 安全重連旗標
unsigned long lastWifiReconnectAttempt = 0;
volatile unsigned long wifiGotIpLedTimer = 0;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
String mqttServer, mqttUser, mqttPass;
int mqttPort;
unsigned long lastMqttReconnectAttempt = 0;
unsigned long mqttRetryInterval = 5000;
unsigned long lastPeriodPublish = 0; 
String deviceId = "";
String routerHostname = "";

// --- 1. 腳位定義 ---
#define MAIN_PWR_RELAY    18  
#define TRIGGER_RELAY_PIN 19  
#define PWR_BTN_PIN       22  
#define CLK_PIN           20  
#define DT_PIN            21  
#define IR_RECEIVE_PIN    23  

#define MOTOR_UP_PIN       2   
#define MOTOR_DOWN_PIN     3   

const int ledPins[]   = {4, 5, 6, 7, 13}; 
const int relayPins[] = {1, 10, 11, 12, 0}; 
const int numSources  = 5;

#ifndef RGB_BUILTIN
#define RGB_BUILTIN 8 
#endif

// --- 2. 紅外線代碼 ---
const uint32_t IR_BTN_1  = 0x3B6;
const uint32_t IR_BTN_2  = 0x37A;
const uint32_t IR_BTN_3  = 0x3D6;
const uint32_t IR_BTN_4  = 0x376;
const uint32_t IR_BTN_5  = 0x1FA; 
const uint32_t IR_MUTE   = 0x1F6;
const uint32_t IR_POWER  = 0x2F6;
const uint32_t IR_VOL_UP = 0x3DA;
const uint32_t IR_VOL_DN = 0x3BA;

// --- 3. 系統變數 ---
int currentSelection = 0; 
int lastClkState;
bool isPowerOn = false; 
bool isMuted = false;
int brightnessLevel = 2; 

bool isConfigMode = false;
volatile bool wifiInitDone = false; 
bool hasWifiConfig = false;
unsigned long wifiStartTime = 0;

bool lastReading = HIGH;
bool confirmedBtnState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

bool isAnimating = false;
int animLED = 0;
int animBrightness = 0;
unsigned long lastAnimMillis = 0;
const int animSpeed = 3; 

bool isTriggerPending = false;
unsigned long triggerStartTime = 0;
const unsigned long triggerDelay = 2500;

bool isMotorRunning = false;
unsigned long motorStartTime = 0; // 安全馬達計時器
unsigned long motorDuration = 0;
int currentMotorDir = 0;
const unsigned long IR_MOTOR_WINDOW = 30;
const unsigned long MQTT_MOTOR_STEP = 50;
const unsigned long WEB_MOTOR_SAFETY = 2000;

uint8_t currentR = 0, currentG = 0, currentB = 0;
unsigned long lastEncoderTime = 0;
const unsigned long encoderDebounce = 10; 
int encoderStepCount = 0;
unsigned long lastIrActionTime = 0;
unsigned long lastIrSignalMillis = 0;

volatile bool mqttPublishPending = false; 
volatile bool nvsSavePending = false;
volatile unsigned long nvsSaveTimer = 0;
int animRound = 0; 

// 湧浪電流保護相關變數
unsigned long lastPowerOffTime = 0;           
bool isPowerOffProtected = false;             
const unsigned long POWER_ON_DELAY = 7000;  
bool isDelayedPowerOnPending = false; 

TaskHandle_t NetworkTaskHandle = NULL;

void buildStatusJSON(char* jsonBuffer, size_t bufferSize);
void publishMQTTStatus();
void switchSource();
void handleButton();
void updateStatusRGB();
void handleIRCommand();
void handleEncoder();

// Escape JSON string contents; preserve UTF-8 bytes and escape all control bytes.
String jsonEscape(const String& value) {
  String result;
  result.reserve(value.length() + 16);
  const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < value.length(); ++i) {
    unsigned char c = static_cast<unsigned char>(value[i]);
    if (c == '"' || c == '\\') {
      result += '\\';
      result += static_cast<char>(c);
    } else if (c < 0x20) {
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 0x0f];
    } else {
      result += static_cast<char>(c);
    }
  }
  return result;
}

int getPWM() {
  if (brightnessLevel == 0) return 15;  
  if (brightnessLevel == 1) return 80;  
  return 255;                                 
}

void refreshWebActivity() {
  lastWebActivityTick = millis();
  if (!isWebHighPerformance) {
    esp_wifi_set_ps(WIFI_PS_NONE); 
    isWebHighPerformance = true;
    Serial.println(">>> 網頁端擊發：立刻喚醒射頻至高性能模式");
  }
}

// ==========================================
// Web 介面
// ==========================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no, viewport-fit=cover">
  <title>Linear Acoustic</title>
  <style>
    body { 
      font-family: "Google Sans", -apple-system, BlinkMacSystemFont, Arial; 
      background: #000000; 
      color: white; 
      margin: 0; 
      padding: 0; 
      touch-action: manipulation; 
      -webkit-user-select: none; 
      user-select: none;
    }
    
    /* 縮減頂部留白，加大內容顯示區 */
    .container { 
      max-width: 500px; margin: 0 auto; background: #121214; 
      padding: calc(18px + env(safe-area-inset-top)) 20px 24px; 
      min-height: 100vh; box-sizing: border-box; 
      border-radius: 36px; 
      overflow: hidden;
    }

    @media (min-width: 600px) {
      body { padding: 20px 20px; background: #0a0a0c; }
      .container { 
        min-height: auto; border-radius: 36px; padding: 23px 30px 26px; 
        border: 1px solid rgba(255,255,255,0.05); box-shadow: 0 10px 40px rgba(0,0,0,0.8);
      }
    }
    
    /* 縮小標題與下方狀態的距離 */
    .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0; }
    h2 { margin: 0; font-size: 25px; letter-spacing: 0.8px; font-weight: 600; }
    
    .btn-pwr { 
      width: 54px; height: 54px; border-radius: 50%; padding: 0; 
      display: inline-flex; align-items: center; justify-content: center; 
      background: linear-gradient(180deg, #04ecd6 0%, #02b6a5 100%); color: #000;
      border: 1px solid rgba(255, 255, 255, 0.4);
      box-shadow: inset 0 2px 4px rgba(255,255,255,0.4), 0 4px 12px rgba(3,218,198,0.28); 
      cursor: pointer; transition: 0.2s;
    }
    .btn-pwr.off { 
      background: linear-gradient(180deg, #e06c81 0%, #bd4e63 100%); color: white; 
      border-color: rgba(255,255,255,0.2); box-shadow: inset 0 2px 4px rgba(255,255,255,0.2); 
    }
    .btn-pwr:active { transform: scale(0.92); }

    #st {
      display: inline-flex; align-items: center; gap: 9px;
      font-size: 16px; font-weight: bold; letter-spacing: 0;
      margin-top: 7px; margin-bottom: 23px; color: #03dac6;
    }
    #st::before {
      content: ''; width: 7px; height: 7px; border-radius: 50%;
      background: var(--status-dot-color, #03dac6); box-shadow: 0 0 8px var(--status-dot-color, #03dac6);
      flex-shrink: 0;
    }
    #st.motor-mode::before { display: none; }

    /* 資訊藥丸列，改為置中並準備放到最下方 */
    .info-pill-bar { display: flex; gap: 8px; margin-top: 2px; margin-bottom: 22px; width: 100%; }
    .info-pill { 
      flex: 1;
      min-width: 0;
      background: rgba(255,255,255,0.045); 
      border: 1px solid rgba(255,255,255,0.075);
      padding: 7px 4px; 
      border-radius: 18px; 
      font-size: 12px; 
      color: #888; 
      display: flex; 
      align-items: center; 
      justify-content: center;
      gap: 4px; 
      white-space: nowrap;
    }
    .info-pill:nth-child(2) {
      flex: 0 0 auto;
      padding: 7px 9px;
    }
    .info-pill .val { 
      color: #f2f2f2; 
      font-weight: 500; 
      overflow: hidden; 
      text-overflow: ellipsis; 
    }

    /* 恢復正常大小寫，三行間距完全統一 */
    h3 { font-size: 14px; color: #8d8d93; margin: 0 0 10px 5px; font-weight: 500; letter-spacing: 0.2px; }

    /* 1. 外軌道容器：三行完全等高 58px、四周留白 5px、圓角 33px */
    .segmented-control {
      display: flex;
      align-items: center;
      height: 58px;
      padding: 5px;
      box-sizing: border-box;
      background: #09090b;
      border: 1px solid rgba(255, 255, 255, 0.05);
      border-radius: 33px;
      margin-bottom: 21px;
    }
    #volGroup { gap: 6px; }

    /* 2. 內部按鍵基底：統一高度 48px、圓角 28px、清空 padding */
    .segmented-control button {
      position: relative;
      overflow: hidden;
      flex: 1;
      height: 48px;
      padding: 0;
      margin: 0;
      box-sizing: border-box;
      background: transparent;
      border: none;
      border-radius: 28px;
      font-size: 16px;
      color: #888;
      font-weight: 500;
      cursor: pointer;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      transition: all 0.1s cubic-bezier(0.25, 1, 0.5, 1);
    }

    /* 3. 黃色按鍵 Active 狀態 (訊源 Source / 亮度 Brightness) */
    .segmented-control button.active {
      background: linear-gradient(180deg, #f5d431 0%, #efc21e 50%, #e2ab0f 100%);
      color: #000;
      font-weight: 700;
      border: none;
      box-shadow: 
        inset 0 1px 0 rgba(255, 255, 255, 0.75),
        inset 0 -1.5px 0 rgba(255, 245, 170, 0.6),
        0 4px 10px rgba(0, 0, 0, 0.35);
    }
    .segmented-control button.active::after {
      content: '';
      position: absolute;
      top: 1px;
      left: 8%;
      right: 8%;
      height: 44%;
      border-radius: 50px;
      background: linear-gradient(
        180deg, 
        rgba(255, 255, 255, 0.42) 0%, 
        rgba(255, 255, 255, 0.08) 75%, 
        rgba(255, 255, 255, 0) 100%
      );
      pointer-events: none;
    }

    /* 4. 音量加減鍵 (+ / -) 預設狀態 */
    .segmented-control .btn-vol {
      flex: 1;
      font-size: 32px;
      font-weight: 300;
      line-height: 1;
      color: #e0e0e0;
      background: linear-gradient(135deg, #222226 0%, #09090b 100%);
      border: none !important; /* 徹底移除實體邊框，解決跳動與溢出 */
      box-shadow: 
        inset 0 0 0 1px rgba(0, 0, 0, 0.8), /* 用內陰影畫出深色極細邊框 */
        inset 1px 2px 2px rgba(255, 255, 255, 0.12),
        inset -1px -2px 5px rgba(0, 0, 0, 0.8),
        0px 4px 8px rgba(0, 0, 0, 0.4);
      margin: 0; 
      border-radius: 28px; /* 統一改為 28px，與 Source 按鈕弧度與高度完美一致 */
    }
    
    /* 音量加減鍵 Active (按下狀態) */
    .segmented-control button.btn-vol:active,
    .segmented-control button.btn-vol.active {
      background: linear-gradient(180deg, #f5d431 0%, #efc21e 50%, #e2ab0f 100%) !important;
      color: #000 !important;
      font-weight: 700;
      border: none !important; /* 徹底移除邊框 */
      box-shadow: 
        inset 0 1px 0 rgba(255, 255, 255, 0.75),
        inset 0 -1.5px 0 rgba(255, 245, 170, 0.6),
        0 0 14px rgba(239, 194, 30, 0.45),
        0 4px 10px rgba(0, 0, 0, 0.35) !important;
    }
    
    .segmented-control button.btn-vol:active::after,
    .segmented-control button.btn-vol.active::after {
      content: ''; position: absolute; top: 1px; left: 8%; right: 8%; height: 44%; border-radius: 50px;
      background: linear-gradient(180deg, rgba(255, 255, 255, 0.42) 0%, rgba(255, 255, 255, 0.08) 75%, transparent 100%);
      pointer-events: none;
    }

    /* 5. 靜音鍵 (MUTE) 預設狀態 */
    .segmented-control .btn-mute {
      flex: 0 0 64px;
      padding: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      color: #777;
      background: linear-gradient(135deg, #222226 0%, #09090b 100%);
      border: none !important; /* 徹底移除實體邊框 */
      box-shadow: 
        inset 0 0 0 1px rgba(0, 0, 0, 0.8), /* 用內陰影畫出邊框 */
        inset 1px 2px 2px rgba(255, 255, 255, 0.12), 
        inset -1px -2px 5px rgba(0, 0, 0, 0.8),
        0px 4px 8px rgba(0, 0, 0, 0.4);
      margin: 0; 
      border-radius: 28px; /* 統一改為 28px */
    }
    
    .segmented-control button.btn-mute svg {
      width: 22px;
      height: 22px;
      fill: currentColor;
    }
    
    /* 靜音鍵 Active (按下狀態) */
    .segmented-control button.btn-mute.active {
      background: linear-gradient(180deg, #cb3947 0%, #b52533 50%, #9e1724 100%) !important;
      color: #ffffff !important;
      border: none !important; /* 徹底移除邊框 */
      box-shadow: inset 0 1px 0 rgba(255,255,255,0.65), inset 0 -1.5px 0 rgba(255,180,180,0.45), 0 4px 10px rgba(0,0,0,0.4) !important;
    }
    
    .segmented-control button.btn-mute.active::after {
      content: ''; position: absolute; top: 1px; left: 8%; right: 8%; height: 44%; border-radius: 50px;
      background: linear-gradient(180deg, rgba(255, 255, 255, 0.4) 0%, rgba(255, 255, 255, 0.05) 80%, transparent 100%);
      pointer-events: none;
    }
    
    /* 馬達運轉時的微光動態 (套用在 Mute 圖標上) */
    @keyframes motorPulse {
      0% { filter: drop-shadow(0 0 2px #f5d431); color: #f5d431; }
      50% { filter: drop-shadow(0 0 12px #f5d431); color: #fff; }
      100% { filter: drop-shadow(0 0 2px #f5d431); color: #f5d431; }
    }
    .motor-running {
      animation: motorPulse 0.6s infinite !important;
    }

    /* V2：待機時保留控制位置，但降低操作區視覺權重 */
    .main-controls {
      transition: opacity 0.22s ease, filter 0.22s ease;
    }
    .main-controls.standby {
      opacity: 0.48;
      filter: saturate(0.72);
    }

    /* 設定頁面獨立按鈕 */
    .btn-glass {
      font-family: inherit; font-weight: 500; color: white; cursor: pointer; width: 100%; box-sizing: border-box; margin-bottom: 12px;
      height: 42px; padding: 0 16px; font-size: 14px; border-radius: 21px; transition: all 0.2s;
      display: flex; align-items: center; justify-content: center;
      background: linear-gradient(180deg, rgba(255,255,255,0.1) 0%, rgba(255,255,255,0.03) 100%); border: none;
      box-shadow: inset 0 1px 1px rgba(255,255,255,0.25), 0 4px 10px rgba(0,0,0,0.3); backdrop-filter: blur(10px);
    }
    .btn-glass:active { transform: scale(0.97); background: rgba(255,255,255,0.04); box-shadow: inset 0 2px 8px rgba(0,0,0,0.6); }
    .setup-btn { margin-top: 1px; }
    .btn-red { background: linear-gradient(180deg, rgba(200,50,70,0.6) 0%, rgba(200,50,70,0.2) 100%); box-shadow: inset 0 1px 1px rgba(255,150,150,0.4), 0 4px 10px rgba(0,0,0,0.3); }
    .btn-blue { background: linear-gradient(180deg, rgba(10,132,255,0.6) 0%, rgba(10,132,255,0.2) 100%); box-shadow: inset 0 1px 1px rgba(150,200,255,0.4), 0 4px 10px rgba(0,0,0,0.3); }
    .btn-cyan { background: linear-gradient(180deg, rgba(3,218,198,0.6) 0%, rgba(3,218,198,0.2) 100%); box-shadow: inset 0 1px 1px rgba(150,255,250,0.5), 0 4px 10px rgba(0,0,0,0.3); color: black; font-weight: bold; }

    /* 設定小標題 */
    .setting-label { font-size: 13px; color: #888; margin: 0 0 6px 12px; letter-spacing: 1px; }

    .input-wrapper { position: relative; margin: 0 0 8px 0; display: block; }
    input, select { 
      font-family: inherit; width: 100%; padding: 0 44px; margin: 0; border-radius: 23px; 
      border: 1px solid #2a2a2a; background: #0c0c0e; color: white; font-size: 15px; 
      box-sizing: border-box; box-shadow: inset 0 2px 8px rgba(0,0,0,0.8); transition: 0.2s; 
      height: 46px; line-height: 46px;
    }
    input:focus { border-color: #bb86fc; outline: none; box-shadow: inset 0 2px 4px rgba(0,0,0,0.6), 0 0 8px rgba(187,134,252,0.3); }
    .field-icon { position: absolute; left: 16px; top: 50%; transform: translateY(-50%); color: #777; pointer-events: none; display: flex; align-items: center; }
    .field-icon svg { width: 18px; height: 18px; fill: currentColor; }
    .toggle-password { position: absolute; right: 16px; top: 50%; transform: translateY(-50%); color: #777; cursor: pointer; display: flex; align-items: center; transition: color 0.2s; }
    .toggle-password:hover { color: #fff; }
    .toggle-password svg { width: 20px; height: 20px; fill: currentColor; }

    details { background: #1a1a1c; border-radius: 18px; margin-bottom: 12px; overflow: hidden; border: 1px solid #2a2a2c; }
    summary { padding: 14px 16px; font-weight: 500; color: #ccc; cursor: pointer; outline: none; list-style: none; display: flex; justify-content: space-between; align-items: center; }
    summary::-webkit-details-marker { display: none; }
    summary::after { content: '▼'; font-size: 12px; transition: transform 0.2s; color: #777; }
    details[open] summary { border-bottom: 1px solid #2a2a2c; background: #202022; }
    details[open] summary::after { transform: rotate(180deg); }
    .setup-content { padding: 10px; }

    .wifi-list-container { display: none; max-height: 280px; overflow-y: auto; background: #0c0c0e; border-radius: 16px; margin-bottom: 12px; padding: 0px; box-shadow: inset 0 2px 8px rgba(0,0,0,0.8); border: 1px solid #222; }
    .wifi-item { display: flex; justify-content: space-between; align-items: center; padding: 9px 12px; border-bottom: 1px solid #161618; cursor: pointer; font-size: 14.5px; transition: background 0.1s; }
    .wifi-item:last-child { border-bottom: none; }
    .wifi-item:active { background: #202024; color: #bb86fc; }
    .wifi-item-left { display: flex; align-items: center; gap: 7px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .wifi-item-right { display: flex; align-items: center; gap: 8px; font-size: 12px; color: #777; flex-shrink: 0; }
    .wf-sig { font-weight: 600; font-size: 12px; }
  </style>
</head>
<body>
  <div class="container">
    
    <div id="view-main">
      <div class="header">
        <h2>Linear Acoustic</h2>
        <button id="pwr" class="btn-pwr off" onclick="cmd('power')">
          <svg viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
            <path d="M18.36 6.64a9 9 0 1 1-12.73 0"></path><line x1="12" y1="2" x2="12" y2="12"></line>
          </svg>
        </button>
      </div>
      <div id="st">Status: Connecting...</div>
      
      <div class="main-controls">
      <h3>Volume</h3>
      <div class="segmented-control" id="volGroup">
        <button id="volDn" class="btn-vol" onpointerdown="cmd('vol_dn_start')" onpointerup="cmd('motor_stop')" onpointerleave="cmd('motor_stop')">−</button>
        <button id="mute" class="btn-mute" onclick="cmd('mute')">
          <svg id="spk-icon" viewBox="0 0 24 24">
            <!-- 預設喇叭圖示 (將由JS動態切換) -->
            <path d="M3 9v6h4l5 5V4L7 9H3zm13.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02zM14 3.23v2.06c2.89.86 5 3.54 5 6.71s-2.11 5.85-5 6.71v2.06c4.01-.91 7-4.49 7-8.77s-2.99-7.86-7-8.77z"/>
          </svg>
        </button>
        <button id="volUp" class="btn-vol" onpointerdown="cmd('vol_up_start')" onpointerup="cmd('motor_stop')" onpointerleave="cmd('motor_stop')">+</button>
      </div>
      
      <h3>Source</h3>
      <div class="segmented-control">
        <button id="s0" onclick="cmd('source',0)">1</button>
        <button id="s1" onclick="cmd('source',1)">2</button>
        <button id="s2" onclick="cmd('source',2)">3</button>
        <button id="s3" onclick="cmd('source',3)">4</button>
        <button id="s4" onclick="cmd('source',4)">Bypass</button>
      </div>
      
      <h3>LED Brightness</h3>
      <div class="segmented-control" id="brSeg" style="touch-action: none;">
        <button id="b0" onclick="cmd('brightness',0)">Low</button>
        <button id="b1" onclick="cmd('brightness',1)">Mid</button>
        <button id="b2" onclick="cmd('brightness',2)">High</button>
      </div>
      </div>
      
      
      <!-- IP, Temp, WiFi 移到系統設定上方，並置中顯示 -->
      <div class="info-pill-bar">
        <div class="info-pill">
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" style="flex-shrink: 0; color: #fff;">
            <path id="wf-arc3" d="M3.7 8.7a11.8 11.8 0 0 1 16.6 0" style="transition: opacity 0.3s; opacity: 0.2;" />
            <path id="wf-arc2" d="M6.2 11.2a8.2 8.2 0 0 1 11.6 0" style="transition: opacity 0.3s; opacity: 0.2;" />
            <path id="wf-arc1" d="M8.8 13.8a4.5 4.5 0 0 1 6.4 0" style="transition: opacity 0.3s; opacity: 0.2;" />
            <circle id="wf-dot" cx="12" cy="17" r="1.5" fill="currentColor" stroke="none" style="transition: opacity 0.3s; opacity: 0.2;" />
          </svg>
          <span id="connSSID" class="val">--</span>
        </div>
        <div class="info-pill">
          <svg viewBox="0 0 24 24" width="15" height="15" fill="currentColor" style="flex-shrink: 0;">
            <path d="M15 13V5c0-1.66-1.34-3-3-3S9 3.34 9 5v8c-1.21.91-2 2.37-2 4 0 2.76 2.24 5 5 5s5-2.24 5-5c0-1.63-.79-3.09-2-4zm-4-2V5c0-.55.45-1 1-1s1 .45 1 1v6h-2z"/>
          </svg>
          <span id="tempBox" class="val">-- °C</span>
        </div>
        <div class="info-pill">IP <span id="ipAddr" class="val">--</span></div>
      </div>

      <button class="btn-glass setup-btn" onclick="changeView('fw')">系統設定 (Setup)</button>
    </div>

    <div id="view-fw" style="display: none;">
      <div class="header" style="margin-bottom: 25px;">
        <h2 style="color: #ffb74d;">System Setup</h2>
      </div>

      <details>
        <summary>System Info</summary>
        <div class="setup-content">
          <div style="font-size: 14px; line-height: 2.2; padding: 0px 4px;">
            <div style="display: flex; justify-content: space-between; align-items: center;">
              <span style="color: #fff;">Wi-Fi 類型</span>
              <span id="wifiTypeBox" style="color: #8e8e93;">--</span>
            </div>
            <div style="display: flex; justify-content: space-between; align-items: center;">
              <span style="color: #fff;">MAC Address</span>
              <span id="macBox" style="color: #8e8e93; font-family: monospace;">--</span>
            </div>
            <div style="display: flex; justify-content: space-between; align-items: center;">
              <span style="color: #fff;">BSSID</span>
              <span id="bssidBox" style="color: #8e8e93; font-family: monospace;">--</span>
            </div>
            <div style="display: flex; justify-content: space-between; align-items: center;">
              <span style="color: #fff;">RSSI</span>
              <span id="rssiBox" style="color: #8e8e93;">-- dBm</span>
            </div>
          </div>
        </div>
      </details>

      <details open>
        <summary>Wi-Fi Network</summary>
        <div class="setup-content">
          <button id="btnScan" class="btn-glass" style="background:rgba(255,255,255,0.05); box-shadow:inset 0 1px 1px rgba(255,255,255,0.1), 0 4px 10px rgba(0,0,0,0.3); margin-bottom: 12px;" onclick="scanWiFi()">掃描附近 Wi-Fi</button>
          <div id="wifi_list_box" class="wifi-list-container"></div>
          
          <div class="input-wrapper">
            <div class="field-icon"><svg viewBox="0 0 24 24"><path d="M12 3c-4.2 0-8 1.7-10.9 4.4L12 19.8l10.9-12.4C20 4.7 16.2 3 12 3z"/></svg></div>
            <input type="text" id="ss" placeholder="Network Name">
          </div>

          <div class="input-wrapper">
            <div class="field-icon"><svg viewBox="0 0 24 24"><path d="M18 8h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2zm-6 9c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2zm3.1-9H8.9V6c0-1.71 1.39-3.1 3.1-3.1 1.71 0 3.1 1.39 3.1 3.1v2z"/></svg></div>
            <input type="password" id="ps" placeholder="Password">
            <div class="toggle-password" onclick="togglePs()">
              <svg id="eye-open" style="display:block;" viewBox="0 0 24 24"><path d="M12 4.5C7 4.5 2.73 7.61 1 12c1.73 4.39 6 7.5 11 7.5s9.27-3.11 11-7.5c-1.73-4.39-6-7.5-11-7.5zM12 17c-2.76 0-5-2.24-5-5s2.24-5 5-5 5 2.24 5 5-2.24 5-5 5zm0-8c-1.66 0-3 1.34-3 3s1.34 3 3 3 3-1.34 3-3-1.34-3-3-3z"/></svg>
              <svg id="eye-close" style="display:none;" viewBox="0 0 24 24"><path d="M12 7c2.76 0 5 2.24 5 5 0 .65-.13 1.26-.36 1.82l2.92 2.92c1.51-1.39 2.7-3.14 3.44-5.12-1.73-4.39-6-7.5-11-7.5-1.4 0-2.74.25-3.98.7l2.16 2.16C10.74 7.13 11.35 7 12 7zM2 4.27l2.28 2.28.46.46C3.08 8.3 1.78 10.02 1 12c1.73 4.39 6 7.5 11 7.5 1.55 0 3.03-.3 4.38-.84l.42.42L19.73 22 21 20.73 3.27 3 2 4.27zM7.53 9.8l1.55 1.55c-.05.21-.08.43-.08.65 0 1.66 1.34 3 3 3 .22 0 .44-.03.65-.08l1.55 1.55c-.67.33-1.41.53-2.2.53-2.76 0-5-2.24-5-5 0-.79.2-1.53.53-2.2zm4.31-.78l3.15 3.15.02-.16c0-1.66-1.34-3-3-3l-.17.01z"/></svg>
            </div>
          </div>
          
          <button class="btn-glass btn-blue" style="margin-top: 10px; margin-bottom: 0;" onclick="saveWifi()">儲存 Wi-Fi</button>
        </div>
      </details>

      <details>
        <summary>MQTT Connection <span id="mqttStatus" style="margin-left: auto; margin-right: 8px; font-size: 12px;">--</span></summary>
        <div class="setup-content">
          <div class="setting-label">伺服器 IP (Broker IP)</div>
          <div class="input-wrapper"><input type="text" id="mq_ip" placeholder="例如: 192.168.1.10"></div>
          
          <div class="setting-label">連接埠 (Port)</div>
          <div class="input-wrapper"><input type="number" id="mq_port" placeholder="預設: 1883"></div>
          
          <div class="setting-label">使用者名稱 (Username)</div>
          <div class="input-wrapper"><input type="text" id="mq_user" placeholder="若無則留空"></div>
          
          <div class="setting-label">密碼 (Password)</div>
          <div class="input-wrapper"><input type="password" id="mq_pw" placeholder="若無則留空"></div>
          
          <button class="btn-glass btn-cyan" onclick="saveMqtt()">儲存 MQTT</button>
        </div>
      </details>

      <details>
        <summary>Firmware Update <span id="fwDate" style="margin-left: auto; margin-right: 8px; font-size: 12px; color: #888;">--</span></summary>
        <div class="setup-content">
          <a class="btn-glass btn-red" href="/ota">GitHub 更新／手動上傳</a>
          <p class="note">登入更新頁後可檢查版本。只有按下安裝才會更新。</p>
        </div>
      </details>
      
      <div style="margin-top: 20px;">
        <button class="btn-glass" style="background:rgba(255,255,255,0.05); box-shadow:inset 0 1px 1px rgba(255,255,255,0.1), 0 4px 10px rgba(0,0,0,0.3);" onclick="changeView('main')">返回主面板</button>
        <button class="btn-glass" style="background:linear-gradient(180deg, rgba(200,50,50,0.2) 0%, rgba(200,50,50,0.05) 100%); box-shadow:inset 0 1px 1px rgba(255,100,100,0.2), 0 4px 10px rgba(0,0,0,0.3);" onclick="rebootDevice()">重新啟動 (Reboot)</button>
      </div>
    </div>
  </div>

  <script>
    let isInteracting = false; 
    let interactionTimeout = null;
    let ws;

    function changeView(view) {
      document.getElementById('view-main').style.display = (view === 'main') ? 'block' : 'none';
      document.getElementById('view-fw').style.display = (view === 'fw') ? 'block' : 'none';
      window.scrollTo(0, 0);
    }

    function initWS() {
      ws = new WebSocket('ws://' + window.location.hostname + ':81/');
      ws.onmessage = (e) => {
        try { let d = JSON.parse(e.data); upd(d); } catch(err) {}
      };
      ws.onclose = () => { setTimeout(initWS, 2000); }; 
    }

    function togglePs() {
      const p = document.getElementById('ps');
      if (p.type === 'password') {
        p.type = 'text'; document.getElementById('eye-open').style.display = 'none'; document.getElementById('eye-close').style.display = 'block';
      } else {
        p.type = 'password'; document.getElementById('eye-open').style.display = 'block'; document.getElementById('eye-close').style.display = 'none';
      }
    }
    
    window.onload = () => {
      initWS();
      fetch('/config').then(r=>r.json()).then(d=>{
        document.getElementById('ss').value = d.wifi_ssid;
        document.getElementById('mq_ip').value = d.mq_ip;
        document.getElementById('mq_port').value = d.mq_port;
        document.getElementById('mq_user').value = d.mq_user;
        if(d.is_ap) { changeView('fw'); setTimeout(() => { scanWiFi(); }, 600); }
      }).catch(e=>console.log("Config load failed"));
    };

    function cmd(a, v=0){ 
      isInteracting = true; clearTimeout(interactionTimeout);
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send((a === 'source' || a === 'brightness') ? a + ':' + v : a);
      }
      interactionTimeout = setTimeout(() => { isInteracting = false; }, 1500);
    }

    async function scanWiFiResults() {
      let url = '/scan?start=1';
      for (let i=0; i<30; i++) {
        const r = await fetch(url, {cache:'no-store'});
        if (r.status === 202) {
          await new Promise(resolve => setTimeout(resolve, 700));
          url = '/scan';
          continue;
        }
        if (!r.ok) throw new Error('Wi-Fi scan failed');
        return await r.json();
      }
      throw new Error('Wi-Fi scan timed out');
    }

    function scanWiFi() {
      isInteracting = true; clearTimeout(interactionTimeout);
      const btn = document.getElementById('btnScan');
      const listBox = document.getElementById('wifi_list_box');
      btn.innerText = "Scanning (3~5s)..."; btn.disabled = true;
      
      scanWiFiResults().then(d => {
          listBox.innerHTML = ''; d.sort((a, b) => b.rssi - a.rssi);
          d.forEach(w => {
            if (!w.ssid) return; 
            const item = document.createElement('div');
            item.className = 'wifi-item';
            item.onclick = () => {
              document.getElementById('ss').value = w.ssid;
              if (w.enc !== "OPEN") document.getElementById('ps').focus();
            };
            
            let sigColor = w.rssi >= -73 ? "#03dac6" : (w.rssi >= -80 ? "#ffb74d" : "#cf6679"); 
            
            // 單色灰階鎖頭 SVG 圖示
            const lockSvg = (w.enc === "OPEN") ? '' : `
              <svg viewBox="0 0 24 24" width="13" height="13" fill="#888" style="flex-shrink:0;">
                <path d="M18 8h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2zm-6 9c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2zm3.1-9H8.9V6c0-1.71 1.39-3.1 3.1-3.1 1.71 0 3.1 1.39 3.1 3.1v2z"/>
              </svg>`;
            
            const metaInfo = (w.enc === "OPEN") ? `CH${w.ch}` : `CH${w.ch}·${w.enc || 'WPA2'}`;

            item.innerHTML = `
              <div class="wifi-item-left">
                ${lockSvg}
                <span class="wifi-ssid"></span>
              </div>
              <div class="wifi-item-right">
                <span>${metaInfo}</span>
                <span class="wf-sig" style="color:${sigColor};">${w.rssi} dBm</span>
              </div>
            `;
            item.querySelector('.wifi-ssid').textContent = w.ssid;
            listBox.appendChild(item);
          });
          listBox.style.display = 'block'; 
          btn.innerText = "重新掃描"; btn.disabled = false;
          interactionTimeout = setTimeout(() => { isInteracting = false; }, 1500);
        }).catch(err => {
          btn.innerText = "Scan Failed, Retry"; btn.disabled = false;
          interactionTimeout = setTimeout(() => { isInteracting = false; }, 1500);
        });
    }
    
    async function postSettings(path, values) {
      const response = await fetch(path, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
        body: new URLSearchParams(values).toString()
      });
      const message = await response.text();
      if (!response.ok || message.trim() !== 'OK') {
        throw new Error(message || `HTTP ${response.status}`);
      }
    }

    async function saveWifi() {
      try {
        await postSettings('/savewifi', {
          ssid: document.getElementById('ss').value,
          pass: document.getElementById('ps').value
        });
        alert('Wi-Fi 儲存成功，設備即將重啟...');
      } catch (err) {
        alert('Wi-Fi 儲存失敗：' + err.message);
      }
    }

    async function saveMqtt() {
      try {
        await postSettings('/savemqtt', {
          ip: document.getElementById('mq_ip').value,
          port: document.getElementById('mq_port').value,
          user: document.getElementById('mq_user').value,
          pass: document.getElementById('mq_pw').value
        });
        alert('MQTT 儲存成功，設備即將重啟...');
      } catch (err) {
        alert('MQTT 儲存失敗：' + err.message);
      }
    }

    function rebootDevice() {
      if (confirm('重新啟動 Linear Acoustic 嗎？')) {
        fetch('/reboot').then(() => { setTimeout(() => { location.reload(); }, 5000); });
      }
    }

    function upd(d){
      window.lastPowerState = d.power;
      const mainControls = document.querySelector('.main-controls');
      if (mainControls) mainControls.classList.toggle('standby', !d.power || d.waiting); 
      let pwrText = document.getElementById('st');
      const statusEl = document.getElementById('st');
      let volDn = document.getElementById('volDn');
      let volUp = document.getElementById('volUp');
      let muteBtn = document.getElementById('mute');
      let spkIcon = document.getElementById('spk-icon');

      if (d.waiting) {
        pwrText.innerText = "Waiting..."; pwrText.style.color = "#ffb74d";
        pwrText.style.setProperty('--status-dot-color', '#ffb74d');
        document.getElementById('pwr').className = "btn-pwr off";
      } else {
        if (d.motor === 1) {
          statusEl.classList.add('motor-mode');
          pwrText.innerText = "VOLUME ▲"; pwrText.style.color = "#f5d431";
          pwrText.style.setProperty('--status-dot-color', '#03dac6'); 
          if (volUp) volUp.classList.add('active');
          if (volDn) volDn.classList.remove('active');
          if (muteBtn) muteBtn.classList.add('motor-running'); // 觸發馬達呼吸燈
          window.volUiActive = true; 
        } 
        else if (d.motor === -1) {
          statusEl.classList.add('motor-mode');
          pwrText.innerText = "VOLUME ▼"; pwrText.style.color = "#f5d431";
          pwrText.style.setProperty('--status-dot-color', '#03dac6'); 
          if (volDn) volDn.classList.add('active');
          if (volUp) volUp.classList.remove('active');
          if (muteBtn) muteBtn.classList.add('motor-running'); // 觸發馬達呼吸燈
          window.volUiActive = true; 
        }
        else {
          statusEl.classList.remove('motor-mode');
          if (volDn) volDn.classList.remove('active');
          if (volUp) volUp.classList.remove('active');
          if (muteBtn) muteBtn.classList.remove('motor-running'); // 停止呼吸燈
          pwrText.innerText = d.power ? "ON" : "Standby";
          pwrText.style.color = d.power ? "#03dac6" : "#cf6679";
          pwrText.style.setProperty('--status-dot-color', d.power ? '#03dac6' : '#cf6679');
        }
        document.getElementById('pwr').className = d.power ? "btn-pwr" : "btn-pwr off";
      }
      
      // Mute狀態及SVG圖標切換
      if (muteBtn) {
        muteBtn.classList.toggle('active', d.mute);
        muteBtn.classList.toggle('motor-running', d.motor !== 0);
      }
      if (spkIcon) {
        if (d.mute) {
          // 靜音劃線圖標
          spkIcon.innerHTML = '<path d="M16.5 12c0-1.77-1.02-3.29-2.5-4.03v2.21l2.45 2.45c.03-.2.05-.41.05-.63zm2.5 0c0 .94-.2 1.82-.54 2.64l1.51 1.51C20.63 14.91 21 13.5 21 12c0-4.28-2.99-7.86-7-8.77v2.06c2.89.86 5 3.54 5 6.71zM4.27 3L3 4.27 7.73 9H3v6h4l5 5v-6.73l4.25 4.25c-.67.52-1.42.93-2.25 1.18v2.06c1.38-.31 2.63-.95 3.69-1.81L19.73 21 21 19.73l-9-9L4.27 3zM12 4L9.91 6.09 12 8.18V4z"/>';
        } else {
          // 正常音量圖標
          spkIcon.innerHTML = '<path d="M3 9v6h4l5 5V4L7 9H3zm13.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02zM14 3.23v2.06c2.89.86 5 3.54 5 6.71s-2.11 5.85-5 6.71v2.06c4.01-.91 7-4.49 7-8.77s-2.99-7.86-7-8.77z"/>';
        }
      }
      
      for(let i=0;i<5;i++) document.getElementById('s'+i).className = (d.source==i && d.power && !d.waiting) ? "active" : "";
      for(let i=0;i<3;i++) document.getElementById('b'+i).className = (d.br==i && d.power && !d.waiting) ? "active" : "";
      
      document.getElementById('ipAddr').innerText = d.ip || "--";
      document.getElementById('tempBox').innerText = d.temp ? d.temp + " °C" : "--";
      document.getElementById('connSSID').innerText = d.ssid || "--";
      let r = d.rssi || 0;
      let isConn = !!d.ssid && d.ssid !== "--" && r !== 0;
      let wDot = document.getElementById('wf-dot');
      let wArc1 = document.getElementById('wf-arc1');
      let wArc2 = document.getElementById('wf-arc2');
      let wArc3 = document.getElementById('wf-arc3');
      if (wDot && wArc1 && wArc2 && wArc3) {
        if (!isConn) {
          wDot.style.opacity = '0.2';
          wArc1.style.opacity = '0.2';
          wArc2.style.opacity = '0.2';
          wArc3.style.opacity = '0.2';
        } else {
          wDot.style.opacity = '1';
          wArc1.style.opacity = (r >= -85) ? '1' : '0.2'; // >= -85 dBm 亮 2 格
          wArc2.style.opacity = (r >= -75) ? '1' : '0.2'; // >= -75 dBm 亮 3 格
          wArc3.style.opacity = (r >= -65) ? '1' : '0.2'; // >= -65 dBm 滿格 (4格)
        }
      }
      if (d.wf_type) document.getElementById('wifiTypeBox').innerText = d.wf_type;
      if (d.mac) document.getElementById('macBox').innerText = d.mac;
      document.getElementById('bssidBox').innerText = d.bssid || "--";
      document.getElementById('rssiBox').innerText = (d.rssi !== 0 && d.rssi) ? d.rssi + " dBm" : "--";
      
      let mqStatus = document.getElementById('mqttStatus');
      mqStatus.innerText = d.mqtt ? "(Connected)" : "(Disconnected)";
      mqStatus.style.color = d.mqtt ? "#03dac6" : "#cf6679";
      if (d.fw_date) document.getElementById('fwDate').innerText = d.fw_date;
    }

    // --- LED Brightness 滑動拖曳支援 ---
    const brSeg = document.getElementById('brSeg');
    let isDraggingBr = false;
    let lastBrIdx = -1;

    function handleBrSlide(e) {
      if (!brSeg) return;
      const rect = brSeg.getBoundingClientRect();
      const x = e.clientX - rect.left;
      let idx = Math.floor((x / rect.width) * 3);
      idx = Math.max(0, Math.min(2, idx));

      if (idx !== lastBrIdx) {
        lastBrIdx = idx;
        cmd('brightness', idx);
        for (let i = 0; i < 3; i++) {
          const btn = document.getElementById('b' + i);
          if (btn) btn.className = (i === idx) ? "active" : "";
        }
      }
    }

    if (brSeg) {
      brSeg.addEventListener('pointerdown', (e) => {
        isDraggingBr = true;
        brSeg.setPointerCapture(e.pointerId);
        lastBrIdx = -1;
        handleBrSlide(e);
      });

      brSeg.addEventListener('pointermove', (e) => {
        if (isDraggingBr) handleBrSlide(e);
      });

      brSeg.addEventListener('pointerup', (e) => {
        if (isDraggingBr) {
          isDraggingBr = false;
          brSeg.releasePointerCapture(e.pointerId);
        }
      });

      brSeg.addEventListener('pointercancel', () => {
        isDraggingBr = false;
      });
    }
  </script>
</body>
</html>
)rawliteral";

// ==========================================
// 馬達與核心邏輯
// ==========================================
void stopMotor() {
  digitalWrite(MOTOR_UP_PIN, LOW);
  digitalWrite(MOTOR_DOWN_PIN, LOW);
  isMotorRunning = false;
  
  if (currentMotorDir != 0) {    
    currentMotorDir = 0;         
    mqttPublishPending = true;   
  }
}

void moveMotorUp(unsigned long duration) {
  if (isMuted) { isMuted = false; switchSource(); }
  digitalWrite(MOTOR_DOWN_PIN, LOW);
  digitalWrite(MOTOR_UP_PIN, HIGH);
  motorStartTime = millis(); 
  motorDuration = duration;
  isMotorRunning = true;
  
  if (currentMotorDir != 1) {    
    currentMotorDir = 1;         
    mqttPublishPending = true;   
  }
}

void moveMotorDown(unsigned long duration) {
  if (isMuted) { isMuted = false; switchSource(); }
  digitalWrite(MOTOR_UP_PIN, LOW);
  digitalWrite(MOTOR_DOWN_PIN, HIGH);
  motorStartTime = millis(); 
  motorDuration = duration;
  isMotorRunning = true;
  
  if (currentMotorDir != -1) {   
    currentMotorDir = -1;        
    mqttPublishPending = true;   
  }
}

void switchSource() {
  isMuted = false; 
  for (int i = 0; i < numSources; i++) {
    digitalWrite(relayPins[i], LOW);
    analogWrite(ledPins[i], 255); 
  }
  digitalWrite(relayPins[currentSelection], HIGH);
  analogWrite(ledPins[currentSelection], 255 - getPWM()); 
}

void setSource(int index) {
  if (currentSelection != index || isMuted) {
    currentSelection = index;
    nvsSavePending = true;
    nvsSaveTimer = millis(); 
    switchSource();
    mqttPublishPending = true; 
  }
}

void executePowerOn() {
  digitalWrite(MAIN_PWR_RELAY, HIGH);  
  isTriggerPending = true;
  triggerStartTime = millis();  
  isPowerOn = true;
  isMuted = false;  
  isAnimating = true;  
  animLED = 0; animBrightness = 0;  
  animRound = 1; 
  isPowerOffProtected = false; 

  for (int i = 0; i < numSources; i++) analogWrite(ledPins[i], 255); 
  mqttPublishPending = true; 

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM); 
  Serial.println(">>> 開機：預設 Wi-Fi 一般模式");
}

void executePowerOff() {
  isDelayedPowerOnPending = false; 
  isTriggerPending = false; 
  digitalWrite(TRIGGER_RELAY_PIN, LOW);  
  delay(100); 
  if (prefAudio.getInt("source", 0) != currentSelection) prefAudio.putInt("source", currentSelection);
  if (prefAudio.getInt("br_level", 2) != brightnessLevel) prefAudio.putInt("br_level", brightnessLevel);
  nvsSavePending = false;
  for (int i = 0; i < numSources; i++) {
    digitalWrite(relayPins[i], LOW);
    analogWrite(ledPins[i], 255); 
  }
  digitalWrite(MAIN_PWR_RELAY, LOW);  
  isPowerOn = false;
  isAnimating = false;
  isMuted = false;
  mqttPublishPending = true; 

  lastPowerOffTime = millis();
  isPowerOffProtected = true;

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM); 
  Serial.println(">>> 待機：已啟用一般 Wi-Fi 省電模式");
}

void togglePower() {
  if (isPowerOn) {
    executePowerOff();
  } else {
    if (isPowerOffProtected && (millis() - lastPowerOffTime < POWER_ON_DELAY)) {
      isPowerOn = true;
      isDelayedPowerOnPending = true; 
      isMuted = false;  
      isAnimating = true;  
      animLED = 0; 
      animBrightness = 0;  
      animRound = 1; 
      isPowerOffProtected = false;    

      for (int i = 0; i < numSources; i++) analogWrite(ledPins[i], 255); 
      
      mqttPublishPending = true; 
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM); 
          
      Serial.println(">>> [安全保護] 偵測到立刻開機！啟動預熱動畫，暫緩主繼電器...");
          
      if (prefAudio.getBool("pwr_state", false) != isPowerOn) {
        prefAudio.putBool("pwr_state", isPowerOn);
      }
      return; 
    }
        
    executePowerOn();
  }
      
  if (prefAudio.getBool("pwr_state", false) != isPowerOn) {
    prefAudio.putBool("pwr_state", isPowerOn);
  }
}

void toggleMute() {
  isMuted = !isMuted;
  if (isMuted) {
    for (int i = 0; i < numSources; i++) {
      digitalWrite(relayPins[i], LOW);
      digitalWrite(ledPins[i], HIGH); 
    }
  } else { switchSource(); }
  mqttPublishPending = true; 
}

// ==========================================
// HA 自動發現機制 (Auto-Discovery)
// ==========================================
void setupMqttAutoDiscovery() {
  String suffix = deviceId.substring(deviceId.length() - 6);
  String devName = "Linear Acoustic - " + suffix;

  String devJson = "\"dev\":{\"ids\":[\"" + deviceId + "\"],\"name\":\"" + devName + "\",\"mf\":\"Linear Acoustic\",\"mdl\":\"ESP32-C6 PreAmp\"}";
  String stat_t = "linear_acoustic/" + deviceId + "/status";
  String cmd_t = "linear_acoustic/" + deviceId + "/cmd";

  String topicPwr = "homeassistant/switch/" + deviceId + "_pwr/config";
  String plPwr = "{\"name\":\"Power\",\"stat_t\":\"" + stat_t + "\",\"val_tpl\":\"{{'power_on' if value_json.power else 'power_off'}}\",\"cmd_t\":\"" + cmd_t + "\",\"pl_on\":\"power_on\",\"pl_off\":\"power_off\",\"icon\":\"mdi:power\",\"uniq_id\":\"" + deviceId + "_pwr\"," + devJson + "}";
  mqttClient.publish(topicPwr.c_str(), plPwr.c_str(), true);

  String topicMute = "homeassistant/switch/" + deviceId + "_mute/config";
  String plMute = "{\"name\":\"Mute\",\"stat_t\":\"" + stat_t + "\",\"val_tpl\":\"{{'mute_on' if value_json.mute else 'mute_off'}}\",\"cmd_t\":\"" + cmd_t + "\",\"pl_on\":\"mute_on\",\"pl_off\":\"mute_off\",\"icon\":\"mdi:volume-off\",\"uniq_id\":\"" + deviceId + "_mute\"," + devJson + "}";
  mqttClient.publish(topicMute.c_str(), plMute.c_str(), true);

  String topicSrc = "homeassistant/select/" + deviceId + "_src/config";
  String plSrc = "{\"name\":\"Source\",\"stat_t\":\"" + stat_t + "\",\"val_tpl\":\"{{value_json.source | int + 1}}\",\"cmd_t\":\"" + cmd_t + "\",\"cmd_tpl\":\"source:{{value | int - 1}}\",\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\"],\"icon\":\"mdi:video-input-component\",\"uniq_id\":\"" + deviceId + "_src\"," + devJson + "}";
  mqttClient.publish(topicSrc.c_str(), plSrc.c_str(), true);

  String topicBr = "homeassistant/select/" + deviceId + "_br/config";
  String plBr = "{\"name\":\"LED Brightness\",\"stat_t\":\"" + stat_t + "\",\"val_tpl\":\"{{['Low','Mid','High'][value_json.br | int]}}\",\"cmd_t\":\"" + cmd_t + "\",\"cmd_tpl\":\"br:{%if value=='Low'%}0{%elif value=='Mid'%}1{%else%}2{%endif%}\",\"options\":[\"Low\",\"Mid\",\"High\"],\"icon\":\"mdi:brightness-6\",\"uniq_id\":\"" + deviceId + "_br\"," + devJson + "}";
  mqttClient.publish(topicBr.c_str(), plBr.c_str(), true);

  String topicVolUp = "homeassistant/button/" + deviceId + "_volup/config";
  String plVolUp = "{\"name\":\"Volume Up\",\"cmd_t\":\"" + cmd_t + "\",\"pl_prs\":\"vol_up\",\"icon\":\"mdi:volume-plus\",\"uniq_id\":\"" + deviceId + "_volup\"," + devJson + "}";
  mqttClient.publish(topicVolUp.c_str(), plVolUp.c_str(), true);

  String topicVolDn = "homeassistant/button/" + deviceId + "_voldn/config";
  String plVolDn = "{\"name\":\"Volume Down\",\"cmd_t\":\"" + cmd_t + "\",\"pl_prs\":\"vol_dn\",\"icon\":\"mdi:volume-minus\",\"uniq_id\":\"" + deviceId + "_voldn\"," + devJson + "}";
  mqttClient.publish(topicVolDn.c_str(), plVolDn.c_str(), true);

  String topicTemp = "homeassistant/sensor/" + deviceId + "_temp/config";
  String plTemp = "{\"name\":\"Core Temp\",\"stat_t\":\"" + stat_t + "\",\"val_tpl\":\"{{value_json.temp}}\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"uniq_id\":\"" + deviceId + "_temp\"," + devJson + "}";
  mqttClient.publish(topicTemp.c_str(), plTemp.c_str(), true);

  String topicIp = "homeassistant/sensor/" + deviceId + "_ip/config";
  String plIp = "{\"name\":\"IP Address\",\"stat_t\":\"" + stat_t + "\",\"val_tpl\":\"{{value_json.ip}}\",\"icon\":\"mdi:ip-network\",\"ent_cat\":\"diagnostic\",\"uniq_id\":\"" + deviceId + "_ip\"," + devJson + "}";
  mqttClient.publish(topicIp.c_str(), plIp.c_str(), true);

  Serial.println(">>> 發送 HA MQTT 自動發現封包");
}

void buildStatusJSON(char* jsonBuffer, size_t bufferSize) {
  float tempC = temperatureRead(); 
  
  String currentSSID = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : (isConfigMode ? "AP Mode" : "Disconnected");
  String currentBSSID = (WiFi.status() == WL_CONNECTED) ? WiFi.BSSIDstr() : "--";
  int currentRSSI = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  String currentMAC = WiFi.macAddress();
  
  String wfType = "--";
  if (WiFi.status() == WL_CONNECTED) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
      if (ap_info.phy_11ax)      wfType = "Wi-Fi 6 (802.11ax)";
      else if (ap_info.phy_11n)  wfType = "Wi-Fi 4 (802.11n)";
      else if (ap_info.phy_11g)  wfType = "802.11g";
      else if (ap_info.phy_11b)  wfType = "802.11b";
    }
  }

  snprintf(jsonBuffer, bufferSize,
    "{\"power\":%s,\"waiting\":%s,\"mute\":%s,\"source\":%d,\"br\":%d,\"ip\":\"%s\",\"temp\":\"%.1f\",\"ssid\":\"%s\",\"bssid\":\"%s\",\"rssi\":%d,\"mqtt\":%s,\"mac\":\"%s\",\"motor\":%d,\"fw_date\":\"%s\",\"wf_type\":\"%s\"}", 
    isPowerOn ? "true" : "false",
    isDelayedPowerOnPending ? "true" : "false",
    isMuted ? "true" : "false",
    currentSelection,
    brightnessLevel,
    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "192.168.4.1",
    tempC,
    jsonEscape(currentSSID).c_str(),
    currentBSSID.c_str(),
    currentRSSI,
    mqttClient.connected() ? "true" : "false",
    currentMAC.c_str(),
    currentMotorDir,
    AUDIO_FW_VERSION,
    wfType.c_str()
  );
}

void publishMQTTStatus() {
  if (mqttClient.connected()) {
    char currentStatus[1024];
    buildStatusJSON(currentStatus, sizeof(currentStatus));
    String statTopic = "linear_acoustic/" + deviceId + "/status";
    mqttClient.publish(statTopic.c_str(), currentStatus, true); 
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (AudioOTA::holdRequested.load()) return;
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.println(">>> 收到 MQTT 指令: " + msg);

  if (msg == "power" || msg == "power_on" || msg == "power_off") { 
    if (msg == "power_on" && isPowerOn) return;  
    if (msg == "power_off" && !isPowerOn) return; 
    togglePower(); 
  }
  else if (isPowerOn) {
    if (msg == "mute" || msg == "mute_on" || msg == "mute_off") {
      if (msg == "mute_on" && isMuted) return;
      if (msg == "mute_off" && !isMuted) return;
      toggleMute();
    }
    else if (msg == "vol_up") moveMotorUp(MQTT_MOTOR_STEP); 
    else if (msg == "vol_dn") moveMotorDown(MQTT_MOTOR_STEP); 
    else if (msg.startsWith("source:")) {
      int val = msg.substring(7).toInt();
      if(val >= 0 && val < numSources) setSource(val);
    }
    else if (msg.startsWith("br:")) {
      int val = msg.substring(3).toInt();
      if(val >= 0 && val <= 2) {
        brightnessLevel = val;
        nvsSavePending = true;
        nvsSaveTimer = millis(); // 啟動/重置 5 秒倒數
        if (!isMuted && !isAnimating) switchSource(); 
        mqttPublishPending = true;
      }
    }
  }
}

// ==========================================
// Web Socket 事件處理器
// ==========================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: { 
      refreshWebActivity(); 
      char currentStatus[1024];
      buildStatusJSON(currentStatus, sizeof(currentStatus));
      webSocket.sendTXT(num, currentStatus);
      break;
    }
      
    case WStype_TEXT: {
      if (AudioOTA::holdRequested.load()) return; 
      refreshWebActivity(); 
      String msg = "";
      for (size_t i = 0; i < length; i++) msg += (char)payload[i];
      
      if (msg == "power") { 
        togglePower(); 
      }
      else if (isPowerOn) {
        if         (msg == "vol_up_start") moveMotorUp(WEB_MOTOR_SAFETY); 
        else if (msg == "vol_dn_start") moveMotorDown(WEB_MOTOR_SAFETY);
        else if (msg == "motor_stop")   stopMotor(); 
        else if (msg == "mute")         toggleMute();
        else if (msg.startsWith("source:")) {
          int val = msg.substring(7).toInt();
          if (val >= 0 && val < numSources) setSource(val);
        }
        else if (msg.startsWith("brightness:")) {
          int val = msg.substring(11).toInt();
          if (val >= 0 && val <= 2) {
            brightnessLevel = val;
            nvsSavePending = true;
            nvsSaveTimer = millis(); // 啟動/重置 5 秒倒數
            if (!isMuted && !isAnimating) switchSource(); 
            mqttPublishPending = true;
          }
        }
      }
      char currentStatus[1024];
      buildStatusJSON(currentStatus, sizeof(currentStatus));
      webSocket.broadcastTXT(currentStatus);
      break;
    }
      
    case WStype_DISCONNECTED:
    default:
      break;
  }
}

// ==========================================
// Web Server 路由處理
// ==========================================
void handleConfig() {
  String json = "{\"wifi_ssid\":\"" + jsonEscape(prefWifi.getString("ssid","")) + "\"," +
                "\"is_ap\":" + (isConfigMode ? "true" : "false") + "," +
                "\"mq_ip\":\"" + jsonEscape(prefAudio.getString("mq_ip","")) + "\"," +
                "\"mq_port\":" + String(prefAudio.getInt("mq_port", 1883)) + "," +
                "\"mq_user\":\"" + jsonEscape(prefAudio.getString("mq_user","")) + "\"}";
  server.send(200, "application/json", json);
}

void handleSaveWiFi() {
  if (AudioOTA::busy.load()) {
    server.send(409, "text/plain", "Firmware update in progress");
    return;
  }
  String reqSsid = server.arg("ssid");
  String reqPass = server.arg("pass");
  
  if (reqSsid.length() > 0) {
    prefWifi.putString("ssid", reqSsid);
    if (reqPass.length() > 0) {
      prefWifi.putString("pass", reqPass);
      Serial.println(">>> 偵測到新密碼，已更新 Wi-Fi 密碼。");
    } else {
      Serial.println(">>> 密碼欄位留空，維持原快閃記憶體中的 Wi-Fi 密碼不變。");
    }
  }
  
  server.send(200, "text/plain", "OK");
  delay(1500); ESP.restart();
}

void handleSaveMQTT() {
  if (AudioOTA::busy.load()) {
    server.send(409, "text/plain", "Firmware update in progress");
    return;
  }
  prefAudio.putString("mq_ip", server.arg("ip"));
  int p = server.arg("port").toInt();
  prefAudio.putInt("mq_port", p == 0 ? 1883 : p);
  prefAudio.putString("mq_user", server.arg("user"));
  prefAudio.putString("mq_pass", server.arg("pass"));
  server.send(200, "text/plain", "OK");
  delay(1500); ESP.restart();
}

void handleScanWiFi() {
  refreshWebActivity();
  if (AudioOTA::holdRequested.load()) {
    server.send(409, "text/plain", "Firmware update in progress");
    return;
  }
  int n = WiFi.scanComplete();
  if (server.hasArg("start") && n != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    n = WiFi.scanNetworks(true, false, true, 300);
  }
  if (n == WIFI_SCAN_RUNNING) {
    server.send(202, "application/json", "{\"scanning\":true}");
    return;
  }
  if (n < 0) {
    server.send(503, "application/json", "{\"error\":\"scan failed\"}");
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "[");

  if (n > 0) {
    for (int i = 0; i < n; ++i) {
      String encType;
      switch (WiFi.encryptionType(i)) {
        case WIFI_AUTH_OPEN: encType = "OPEN"; break; 
        case WIFI_AUTH_WEP: encType = "WEP"; break;
        case WIFI_AUTH_WPA_PSK: encType = "WPA"; break;
        case WIFI_AUTH_WPA2_PSK: encType = "WPA2"; break;
        case WIFI_AUTH_WPA_WPA2_PSK: encType = "WPA/WPA2"; break;
        case WIFI_AUTH_WPA3_PSK: encType = "WPA3"; break;
        case WIFI_AUTH_WPA2_WPA3_PSK: encType = "WPA2/WPA3"; break;
        default: encType = "WPAx"; break;
      }

      char buffer[512];
      snprintf(buffer, sizeof(buffer), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"enc\":\"%s\"}",
               (i == 0) ? "" : ",", 
               jsonEscape(WiFi.SSID(i)).c_str(),
               WiFi.RSSI(i),
               WiFi.channel(i),
               encType.c_str());
      server.sendContent(buffer);
    }
  }
  server.sendContent("]");
  server.sendContent("");
  WiFi.scanDelete();
}

void startAPMode() {
  isConfigMode = true;
  String apName = "LinearAcoustic-" + deviceId.substring(3); 
  WiFi.disconnect(true, true); 
  delay(100);
  WiFi.mode(WIFI_AP_STA); 
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  WiFi.softAP(apName.c_str(), "12345678");   
  dnsServer.start(53, "*", WiFi.softAPIP());
}

void handleReboot() {
  if (AudioOTA::busy.load()) {
    server.send(409, "text/plain", "Firmware update in progress");
    return;
  }
  server.send(200, "text/plain", "OK");
  Serial.println(">>> 接收到重啟指令，設備即將重啟！");
  delay(1500); 
  ESP.restart();
}

void startWebServerAndOTA() {
  server.on("/", []() { server.send(200, "text/html", index_html); });
  server.on("/config", handleConfig);      
  server.on("/savewifi", HTTP_POST, handleSaveWiFi);
  server.on("/savemqtt", HTTP_POST, handleSaveMQTT);  
  server.on("/scan", handleScanWiFi); 
  server.on("/reboot", handleReboot);
  
  server.onNotFound([]() { 
    if(isConfigMode) { server.send(200, "text/html", index_html); }
    else server.send(404, "text/plain", "Not Found");
  });
  
  server.begin();
  AudioOTA::startArduinoOTA();

  String suffix = deviceId.substring(deviceId.length() - 6); 
  String dnsName = "linearacoustic-" + suffix;              
  
  if (MDNS.begin(dnsName.c_str())) {
    MDNS.addService("http", "tcp", 80); 
    Serial.print(">>> [mDNS] 本機區網域名: http://");
    Serial.print(dnsName);
    Serial.println(".local");
  } else {
    Serial.println(">>> [mDNS] 啟動失敗");
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print(">>> [Event] 成功取得 IP: ");
      Serial.println(WiFi.localIP());
      wifiGotIpLedTimer = millis();
      wifiConnected = true;
      wifiShouldReconnect = false;
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println(">>> [Event] Wi-Fi 斷開！通知背景非同步嘗試重新連線");
      wifiConnected = false;
      if (hasWifiConfig) {
        wifiShouldReconnect = true; 
      }
      break;
    
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.println(">>> [Event] 偵測到手機連入熱點！解除省電封印，射頻全開");
      esp_wifi_set_ps(WIFI_PS_NONE);
      break;

    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.println(">>> [Event] 手機已離開熱點！恢復射頻省電");
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      break;

    default:
      break;
  }
}

// ==========================================
// Background Network Task (Core 0)
// ==========================================
void networkTask(void *pvParameters) {
  static unsigned long runningDisconnectStart = 0;
  
  for(;;) {
    if (!wifiInitDone) {
      if (wifiConnected) {
        startWebServerAndOTA();
        wifiInitDone = true;
      } else if (!hasWifiConfig || (millis() - wifiStartTime > 10000)) { 
        Serial.println(">>> [開機超時] 無法在限時內連上目標路由器，啟動 AP Mode");
        startAPMode();
        startWebServerAndOTA();
        wifiInitDone = true;
      }
    }   
    else {
      if (!wifiConnected && !isConfigMode) {
        if (runningDisconnectStart == 0) {
          runningDisconnectStart = millis(); 
        }
        
        if (millis() - runningDisconnectStart > 60000) { 
          Serial.println(">>> [運作中斷線超時] 與路由器斷開超過 60 秒，自動開啟 AP Mode");
          startAPMode();
          runningDisconnectStart = 0; 
        }
      } else {
        runningDisconnectStart = 0; 
      }

      bool allowReconnect = false;
      unsigned long reconnectInterval = 5000;

      if (!isConfigMode) {
        allowReconnect = true;
        reconnectInterval = 5000;
      } 
      else {
        int connectedStations = WiFi.softAPgetStationNum();
        if (connectedStations == 0) {
          allowReconnect = true;
          reconnectInterval = 30000; 
        } else {
          allowReconnect = false;
        }
      }

      if (wifiShouldReconnect && allowReconnect && (millis() - lastWifiReconnectAttempt > reconnectInterval)) {
        lastWifiReconnectAttempt = millis();
        if (isConfigMode) {
          Serial.println(">>> [自動復原] 熱點目前無人使用，嘗試連回主要基地台");
        } else {
          Serial.println(">>> 背景任務：嘗試進行非同步 Wi-Fi 重新連線...");
        }
        WiFi.setHostname(routerHostname.c_str());
        String s = prefWifi.getString("ssid", "");
        String p = prefWifi.getString("pass", "");
        WiFi.begin(s.c_str(), p.c_str()); 
      }

      if (isConfigMode && wifiConnected) {
        Serial.println(">>> [網路復原成功] 已自動連回主要基地台！解除 AP Mode");
        WiFi.mode(WIFI_STA);
        dnsServer.stop();
        isConfigMode = false;
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM); 
      }

      if (isConfigMode) dnsServer.processNextRequest();
      server.handleClient(); 
      webSocket.loop(); 
      AudioOTA::networkLoop();

      if (wifiConnected && mqttServer != "") {
        if (!mqttClient.connected()) {
          if (!AudioOTA::busy.load() && millis() - lastMqttReconnectAttempt >= mqttRetryInterval) { 
            if (mqttClient.connect(deviceId.c_str(), mqttUser.c_str(), mqttPass.c_str())) {
              setupMqttAutoDiscovery();
              String cmdTopic = "linear_acoustic/" + deviceId + "/cmd";
              mqttClient.subscribe(cmdTopic.c_str()); 
              mqttPublishPending = true;
              mqttRetryInterval = 5000;
            } else {
              mqttRetryInterval = mqttRetryInterval < 30000 ? mqttRetryInterval * 2 : 60000;
            }
            lastMqttReconnectAttempt = millis();
          }
        } else {
          mqttClient.loop(); 
        }
      }

      if (mqttPublishPending || (millis() - lastPeriodPublish > 60000)) {
        char currentStatus[1024];
        buildStatusJSON(currentStatus, sizeof(currentStatus));
        webSocket.broadcastTXT(currentStatus); 
        
        if (wifiConnected && mqttClient.connected()) {
          String statTopic = "linear_acoustic/" + deviceId + "/status";
          mqttClient.publish(statTopic.c_str(), currentStatus, true);
        }
        
        mqttPublishPending = false;
        lastPeriodPublish = millis();
      }
    }

    if (isWebHighPerformance && (millis() - lastWebActivityTick > 20000)) {
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      isWebHighPerformance = false;
      Serial.println(">>> 閒置超過 20 秒，射頻節能模式. ");
    }

    vTaskDelay(pdMS_TO_TICKS(1)); 
  }
}

// ==========================================
// Setup
// ==========================================
void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  
  String mac = WiFi.macAddress();
  WiFi.disconnect(true, false);
  mac.replace(":", "");
  mac.toLowerCase();
  String suffix = mac.substring(mac.length() - 6);
  deviceId = "la_" + suffix;
  routerHostname = "LinearAcoustic-" + suffix;
  WiFi.setHostname(routerHostname.c_str());
  WiFi.onEvent(onWiFiEvent);

  pinMode(MAIN_PWR_RELAY, OUTPUT);
  pinMode(TRIGGER_RELAY_PIN, OUTPUT);
  pinMode(MOTOR_UP_PIN, OUTPUT);
  pinMode(MOTOR_DOWN_PIN, OUTPUT);
  pinMode(PWR_BTN_PIN, INPUT_PULLUP);
  pinMode(CLK_PIN, INPUT_PULLUP);
  pinMode(DT_PIN, INPUT_PULLUP);

  for (int i = 0; i < numSources; i++) {
    pinMode(ledPins[i], OUTPUT);
    pinMode(relayPins[i], OUTPUT);
  }
  
  prefAudio.begin("audio_cfg", false);
  currentSelection = prefAudio.getInt("source", 0);
  isPowerOn = prefAudio.getBool("pwr_state", false);
  brightnessLevel = prefAudio.getInt("br_level", 2); 

  mqttServer = prefAudio.getString("mq_ip", "");
  mqttPort = prefAudio.getInt("mq_port", 1883);
  mqttUser = prefAudio.getString("mq_user", "");
  mqttPass = prefAudio.getString("mq_pass", "");
  if (mqttServer != "") {
    mqttClient.setServer(mqttServer.c_str(), mqttPort);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(1024);
    espClient.setConnectionTimeout(500);
    mqttClient.setSocketTimeout(1); 
  }

  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  pinMode(IR_RECEIVE_PIN, INPUT_PULLUP);

  if (isPowerOn) {
    neopixelWrite(RGB_BUILTIN, 25, 5, 0); 
    delay(1500); 
    executePowerOn();
  } else {
    executePowerOff();
  }
  
  lastClkState = digitalRead(CLK_PIN);

  AudioOTA::begin(server);

  prefWifi.begin("wifi_cfg", false);
  String storedSsid = prefWifi.getString("ssid", "");
  String storedPass = prefWifi.getString("pass", "");
 
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (storedSsid != "") {
    hasWifiConfig = true;
    WiFi.begin(storedSsid.c_str(), storedPass.c_str()); 
  }
  wifiStartTime = millis();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 1, &NetworkTaskHandle, 0);
  Serial.println(">>> 實體硬體控制啟動完成");
}

// ==========================================
// Loop (ESP32-C6: scheduled on the same application CPU as other tasks)
// ==========================================
void loop() {
  AudioOTA::serialHelp();
  // Only this task changes audio hardware to enter OTA maintenance.
  if (AudioOTA::holdRequested.load()) {
    if (!AudioOTA::holdReady.load()) {
      stopMotor();
      if (isPowerOn || isDelayedPowerOnPending || isTriggerPending) executePowerOff();
      // Updates reboot into standby; the existing Trigger -> 100 ms -> main
      // power-off sequence inside executePowerOff() remains unchanged.
      if (prefAudio.getBool("pwr_state", false)) prefAudio.putBool("pwr_state", false);
      IrReceiver.resume();
      AudioOTA::holdReady.store(true);
    }
    delay(1);
    return;
  }
  AudioOTA::holdReady.store(false);

  if (isDelayedPowerOnPending && (millis() - lastPowerOffTime >= POWER_ON_DELAY)) {
    digitalWrite(MAIN_PWR_RELAY, HIGH);
    isTriggerPending = true;
    triggerStartTime = millis(); 
    isDelayedPowerOnPending = false;

    animRound = 0;
    
    mqttPublishPending = true;
    Serial.println(">>> [安全保護] 預熱冷卻結束，啟動主電源");
  }

  handleButton(); 
  updateStatusRGB();

  if (IrReceiver.decode()) {
    Serial.print(F("IR遙控協定: "));
    Serial.print(IrReceiver.getProtocolString());
    Serial.printf(" | RawData: 0x%lX | Command: 0x%X\n", 
                  (unsigned long)IrReceiver.decodedIRData.decodedRawData, 
                  IrReceiver.decodedIRData.command);
    uint32_t code = IrReceiver.decodedIRData.decodedRawData;
    if (code != 0) { lastIrSignalMillis = millis(); } 
    if (isPowerOn) {
      if (code == IR_VOL_UP) moveMotorUp(IR_MOTOR_WINDOW); 
      else if (code == IR_VOL_DN) moveMotorDown(IR_MOTOR_WINDOW);
      else handleIRCommand(); 
    } else {
      handleIRCommand(); 
    }
    IrReceiver.resume(); 
  }

  if (isTriggerPending && (millis() - triggerStartTime >= triggerDelay)) {
    digitalWrite(TRIGGER_RELAY_PIN, HIGH);
    isTriggerPending = false; 
  }

  if (isMotorRunning && (millis() - motorStartTime >= motorDuration)) stopMotor();

  if (isPowerOn) {
    if (isAnimating) {
      if (millis() - lastAnimMillis > animSpeed) {
        lastAnimMillis = millis();
        if (animLED < numSources) { analogWrite(ledPins[animLED], 0); }
        if (animLED > 0 && (animLED-1) < numSources) { analogWrite(ledPins[animLED-1], animBrightness / 2); }
        if (animLED > 1 && (animLED-2) < numSources) { analogWrite(ledPins[animLED-2], 128 + (animBrightness / 2)); }
        animBrightness += 5; 
        if (animBrightness > 255) {
          if (animLED > 2 && (animLED-3) < numSources) analogWrite(ledPins[animLED-3], 255); 
          animBrightness = 0; animLED++;          
          if (animLED >= numSources + 2) { 
            if (isDelayedPowerOnPending) {
              animLED = 0; 
              animRound = 1; 
              Serial.println(">>> [安全保護] 硬件主電源尚未接通，預熱動畫持續循環等待");
            } 
            else {
              if (animRound < 2) {
                animRound++;
                animLED = 0; 
              } else {
                isAnimating = false; 
                if (isMuted) {
                    for (int i = 0; i < numSources; i++) {
                      digitalWrite(relayPins[i], LOW);
                      analogWrite(ledPins[i], 255); 
                    }
                  } else {
                    switchSource(); 
                  }
                }
              }
          }
        }
      }
    } else {
      handleEncoder();      
      if (isMuted) {
        int t = millis() % 1000;
        int maxBr = getPWM(); 
        int b = (t < 500) ? map(t, 0, 499, 0, maxBr) : map(t, 500, 999, maxBr, 0);
        analogWrite(ledPins[currentSelection], 255 - b); 
      }
    }
  }

  if (nvsSavePending && (millis() - nvsSaveTimer >= 120000)) {
    // 儲存訊源和亮度
    if (prefAudio.getInt("source", 0) != currentSelection) {
      prefAudio.putInt("source", currentSelection);
      Serial.println(">>> 閒置2分鐘，將最終訊源寫入 NVS");
    }
    if (prefAudio.getInt("br_level", 2) != brightnessLevel) {
      prefAudio.putInt("br_level", brightnessLevel);
      Serial.println(">>> 閒置2分鐘，將最終亮度寫入 NVS");
    }
    nvsSavePending = false;
  }
  delay(1); 
}

void updateStatusRGB() {
  uint8_t targetR = 0, targetG = 0, targetB = 0;
  if (wifiGotIpLedTimer != 0 && (millis() - wifiGotIpLedTimer < 1000)) {
    targetR = 0; targetG = 0; targetB = 150; }
  else if (millis() - lastIrSignalMillis < 100) { 
    targetR = 150; targetG = 0; targetB = 0; } 
  else if (isPowerOn) {
    targetG = 1; 
  }
  
  if (targetR != currentR || targetG != currentG || targetB != currentB) {
    neopixelWrite(RGB_BUILTIN, targetR, targetG, targetB);
    currentR = targetR; currentG = targetG; currentB = targetB;
  }
}

void handleIRCommand() {
  uint32_t code = IrReceiver.decodedIRData.decodedRawData;
  if (code == 0) return;
  bool canTrigger = (millis() - lastIrActionTime > 250);
  if (code == IR_POWER) {
    if (!canTrigger) return; 
    lastIrActionTime = millis();
    togglePower(); 
  } else if (isPowerOn && canTrigger) {
    lastIrActionTime = millis();
    if        (code == IR_BTN_1) setSource(0);
    else if (code == IR_BTN_2) setSource(1);
    else if (code == IR_BTN_3) setSource(2);
    else if (code == IR_BTN_4) setSource(3);
    else if (code == IR_BTN_5) setSource(4);
    else if (code == IR_MUTE)  toggleMute();
  }
}

void handleButton() {
  bool reading = digitalRead(PWR_BTN_PIN);
  if (reading != lastReading) lastDebounceTime = millis();
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != confirmedBtnState) {
      confirmedBtnState = reading;
      if (confirmedBtnState == LOW) { 
        togglePower(); 
      }
    }
  }
  lastReading = reading;
}

void handleEncoder() {
  int clkState = digitalRead(CLK_PIN);
  if (clkState == LOW && lastClkState == HIGH) {
    if (millis() - lastEncoderTime > encoderDebounce) {
      delay(1); 
      if (digitalRead(DT_PIN) == HIGH) encoderStepCount--; else encoderStepCount++;
      if (abs(encoderStepCount) >= 2) {
        if (encoderStepCount >= 2) currentSelection = (currentSelection + 1) % numSources;
        else currentSelection = (currentSelection - 1 + numSources) % numSources;
        nvsSavePending = true; nvsSaveTimer = millis(); 
        switchSource(); encoderStepCount = 0; mqttPublishPending = true; 
      }
      lastEncoderTime = millis();
    }
  }
  lastClkState = clkState;
}