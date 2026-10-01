# LAV-PREControl 26.10.02+1

暖機 Waiting 時，電源按鈕使用與文字一致的橘色；ON 保留青綠色，Standby 保留粉紅色。

保留 26.09.30+4 OTA 下載修正：下載期間 Wi-Fi 不省電、總上限十分鐘、30 秒無資料停止、下載量與速度回報。保留免密碼更新、HTTPS 憑證、SHA-256 與映像校驗，以及原本 100 ms 關機繼電器時序。

## 編譯

完整下載此資料夾，開啟 LAV-PREControl.ino。

- esp32 by Espressif Systems 3.3.12。
- ESP32C6 Dev Module、Flash 4 MB、CPU 160 MHz、QIO、USB CDC Disabled。
- Partition Scheme：Minimal SPIFFS（1.9 MB APP with OTA）。首次從 Default 1.2 MB APP 改分割區必須使用 USB。
- IRremote 4.7.1、WebSockets 2.7.2、PubSubClient 2.8、ArduinoJson 7.4.3。
- 版本號只修改 ota_config.h 的 AUDIO_FW_VERSION，修改後重新編譯。

## 狀態

26.10.02+1 已檢查網頁 JavaScript 語法，尚未進行本版本完整 Arduino 編譯與實機驗證。正式 OTA version.json 仍為已發布的 26.09.30+4。

## 發布

匯出應用程式 LAV-PREControl.ino.bin，不要使用 merged、bootloader 或 partitions bin。

```text
python prepare_release.py "LAV-PREControl.ino.bin" "release-candidate" --slot-size 0x1E0000
```

工具核對 ESP32-C6 映像與內嵌版本，計算大小與 SHA-256。先上傳產生的 releases/<版本>/LAV60II.bin 至 OTA/ESP32C6，再用 version.candidate.json 更新 OTA/ESP32C6/version.json。已發布的版本 BIN 不要覆寫。
