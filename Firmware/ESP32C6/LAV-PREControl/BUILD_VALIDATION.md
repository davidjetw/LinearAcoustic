# 編譯與驗證紀錄

版本：26.09.30+1；日期：2026-09-30。

## 編譯成功

- Arduino-ESP32 3.3.12；ESP32C6 Dev Module。
- FQBN：`esp32:esp32:esp32c6:PartitionScheme=min_spiffs,FlashSize=4M`。
- 程式空間：1,439,444 / 1,966,080 bytes（73%）。
- 全域變數：52,812 / 327,680 bytes（16%）；餘 274,868 bytes，不等於執行時可用 heap。
- 產生的應用程式 bin：1,439,584 bytes。
- 此次驗證 bin SHA-256：`c6f8cb202f89821d7244b59494ca95b8238fa07ac7d8fba41ba6d2beb92884fe`。重新編譯可能不同，發布時必須重新計算。

**首次安裝必須選 Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)，再經 USB 上傳。** 原 Default 4MB 的 1.2MB APP 放不下本版；不能只用 OTA 更換分割表。

## 已通過的軟體檢查

- 日期版本解析的 C++ static_assert：數字版次排序、日期排序與不合法輸入。
- 發布工具 7 項測試：日期、晶片、容量、版本標記、映像類型、真實 hash/size、拒絕覆寫。
- 更新頁面狀態、忙碌防重送、舊資訊拒絕、POST token、取消安裝、斷線控制測試。
- 對實際編譯 bin 執行發布工具，成功辨識 ESP32-C6、版本 26.09.30+1、大小與摘要。
- 與提供的原稿比對：executePowerOff 保持原內容，包含刻意的 100 ms 延遲；Mute 動畫修正保留。

## 尚未完成的實機驗證

沒有對實體板燒錄，沒有驗證真實網路下載、GPIO/音響輸出、斷電或 OTA 後開機。請按 README 的實機清單驗證。

本次只上傳原始碼與文件。既有 GitHub 舊 binary 與正式 version.json 未改成這個版本；本機產生的候選發布資訊也未發布。
