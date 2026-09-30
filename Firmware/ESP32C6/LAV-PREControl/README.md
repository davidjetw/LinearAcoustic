# Linear Acoustic ESP32-C6：GitHub OTA 開發版

版本：`26.09.30+1`。這份是以使用者提供的最新 `LAV-PREControl.ino` 整合的待實機驗證版本，不代表 GitHub 已發布可安裝的新韌體。

## 功能

- 音響主頁的 Firmware Update 按鈕開啟 `/ota`，登入後才能檢查或安裝。
- 顯示裝置版本、可用版本、下載進度與錯誤原因；只有使用者按安裝才會更新。
- GitHub HTTPS 檢查／下載使用獨立 FreeRTOS 任務，不在 HTTP/WebSocket 處理函式裡等待下載。
- 檢查產品、ESP32-C6 板型、日期版本、大小、OTA 分割區容量與 SHA-256；不允許相同版本或降版。
- SHA-256 與完整大小驗證通過後，才將新分割區設為下次開機目標。
- 不使用 `setInsecure()`；使用 ESP32 3.3.12 內建 CA 憑證庫與網域驗證，先透過 NTP 同步時間。
- 音響停止馬達、保留 Trigger 關閉 → 100 ms → 主電源關閉，然後進入更新待機。失敗時恢復控制但維持待機；成功後重啟並維持待機。
- 手動網頁上傳與 Arduino IDE OTA 都受密碼保護，並共用更新互斥與待機流程。
- MQTT 連線等待縮短並採漸增重試；Wi-Fi 掃描改成背景掃描及網頁輪詢。
- 保留 GPIO、亮度操作、正常開關機時序、IR 控制與已修正的 Mute 動畫。

## 開發環境

- Arduino IDE 使用者畫面：2.3.10。
- 本機實際套件：**esp32 by Espressif Systems 3.3.12**。
- 板型：**ESP32C6 Dev Module**，Flash 4 MB、CPU 160 MHz、QIO、USB CDC Disabled。
- 函式庫：IRremote 4.7.1、WebSockets 2.7.2、PubSubClient 2.8、ArduinoJson 7.4.3。
- HTTPS 憑證 API 使用 3.3.12 的 `useBuiltinCACertBundle()`，不要直接套用到舊 core 或 ESP8266。

### 分割區

使用者原設定是 `Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)`，每個 OTA APP 實際上限為 `0x140000 = 1,310,720 bytes`。

本版編譯已超過此限制，必須改用 `Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)`，每個 APP 為 `0x1E0000 = 1,966,080 bytes`。這仍保留雙 OTA 分割區；不要選 No OTA/Huge APP。首次更換分割區必須經 USB 上傳，GitHub OTA 不會替換分割表。兩種配置的 NVS 位置相同；本程式未使用 SPIFFS。

實際編譯結果與建議使用的選項見 `BUILD_VALIDATION.md`。

## 第一次安裝與取得密碼

1. 將整個 `LAV-PREControl` 資料夾放到你的草稿位置，開啟同名 `.ino`。使用這份完整專案，不要再混入之前示範用的 `protected_ota.h`。
2. 按驗證文件設定開發板與分割區，先經 USB 燒錄。此次工作沒有替你燒錄任何實體裝置。
3. 開啟序列埠監控視窗，選 **115200 baud**，結尾選「換行」或「NL & CR」。
4. 傳送 `OTA PASSWORD`，裝置會在 USB/UART 序列埠回覆帳號 `admin` 與該裝置的密碼。請自行保存，不要貼上 GitHub。
5. 在音響網頁按「GitHub 更新／手動上傳」，輸入上述帳密。

密碼首次啟動時由亂數產生，儲存在獨立 NVS `ota_auth`。應用程式 OTA 不會覆蓋 NVS，因此密碼會保留，也不會編入公開 `.bin`。清除全部 Flash/NVS 後會產生新密碼。USB 序列埠指令可再次查看，不經網頁傳回密碼。

本地網頁使用 HTTP Digest 驗證與表單 token，**不是 HTTPS 加密**，請限可信區網使用。這次僅保護更新入口，沒有替既有音響 WebSocket、Wi-Fi/MQTT 設定與一般重啟路由全面加上登入。SHA-256 是檔案完整性校驗，不等同韌體數位簽章。

## 目前 GitHub 舊韌體

既有 `OTA/ESP32C6/LAV60II.bin` 是使用者確認的早期測試檔。舊 `version.json` 只有 `0.1.1` 和 URL，缺少產品、板型、檔案大小及 SHA-256。

新程式遇到它會顯示「尚未發布具備完整校驗資訊的新韌體」，不會把舊檔裝回裝置。不要只修改舊 JSON 的版本號，讓舊 bin 假裝成新版。

## 正式發布流程

1. 在 `ota_config.h` 指定發布版本，例如 `26.09.30+1`；同一天下一版 `26.09.30+2`，隔天重設為 `26.10.01+1`。
2. 確認沒有把任何 Wi-Fi、MQTT、OTA 密碼硬編碼進來源或 binary。
3. 先經 USB 在實機驗證音響操作、開關機時序、網頁、密碼及錯誤更新流程。
4. Arduino IDE 選「Sketch → 匯出已編譯的二進位檔」。取 **應用程式 `.ino.bin`**，不要使用 `.merged.bin`、bootloader 或 partitions。
5. 在有 Python 3 的電腦執行：

```text
python prepare_release.py "路徑/LAV-PREControl.ino.bin" "release-candidate" --slot-size 0x1E0000
```

如果實機保留 Default 分割區，`--slot-size` 改為 `0x140000`。工具從 bin 中讀取內嵌版本，檢查 C6 應用程式標頭與容量，計算真實大小與 SHA-256，不需要手填。

6. 工具產生 `releases/<版本>/LAV60II.bin` 以及 `version.candidate.json`。先把 `releases` 資料夾上傳到 GitHub 的 `OTA/ESP32C6/`，每個已發布版本的檔案不要覆寫。
7. 確認下載檔案與實機驗證完成後，才用 `version.candidate.json` 的內容更新 `OTA/ESP32C6/version.json`。先上傳 bin、最後更新 JSON，避免裝置看到尚不存在的韌體。
8. 裝置按「檢查更新」後應看到新版本。按安裝才會進入待機與更新。版本資訊檢查結果有效十分鐘，過期須重新檢查。

`prepare_release.py` 只在本機產生待發布資料，不會登入 GitHub、不會上傳、不會自動更新正式 JSON。

## 更新資訊格式

必要欄位為：`schema`（整數 1）、`product`（`linear-acoustic-lav60ii`）、`board`（`esp32c6`）、`version`（日期版本）、`url`（本儲存庫 raw HTTPS .bin 網址）、`size`（真實 bytes）、`sha256`（真實 64 位十六進位摘要）。不要用示意數字或假 hash 發布。

裝置只接受本儲存庫的 raw HTTPS 下載，拒絕轉址、其他網域及過大檔案；目前支援公開儲存庫，沒有內嵌 GitHub token。若日後改成 Releases 附件或私人儲存庫，需另外調整下載策略。

## 必須進行的實機驗證

- USB 初次啟動、密碼查詢、一般電源／音源／音量／Mute／亮度操作。
- 正確／錯誤密碼，更新頁刷新與裝置重啟後的 token 過期。
- 舊 manifest、錯板型、過大檔案、相同／較舊版本、SHA 不符、404、下載斷線。
- 檢查更新期間音響控制可用；安裝期間控制暫停，馬達停止且保留原關機時序。
- 更新失敗後可以重新操作；更新成功後待機啟動，版本正確且密碼保留。
- 關閉 Broker、Wi-Fi 掃描時的網頁音量反應。MQTT 仍是同步短等待，並非保證毫秒級停止。
- 目前沒有自動開機健康檢查／失敗回滾，也沒有承諾已完成實機測試。先保留 USB 救援方式。

