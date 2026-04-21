#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <esp_arduino_version.h>
#include <esp_system.h>
#include <math.h>
#include "ui.h"

#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
#include <esp32-hal-bt.h>
#endif

#ifndef ENABLE_WIFI_RADIO
#define ENABLE_WIFI_RADIO 1
#endif

#ifndef UI_CPU_MHZ
#define UI_CPU_MHZ 80
#endif

#if ENABLE_WIFI_RADIO
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#endif

struct __attribute__((packed)) struct_message {
    // 송신기 업타임(초)
    uint32_t uptime;
    // 메인 A/B 채널 주기
    float hz_a;
    float hz_b;
    // 기능 활성 상태 플래그
    bool nag_active;
    bool eap_active;
    // 모드/상태 값(신규 페이로드 포맷)
    uint8_t nag_mode;
    uint8_t twai_state;
    // 진단 카운터
    uint32_t echo_count;
    uint32_t tx_fail_count;
    uint32_t a_frames_total;
    uint32_t a_frames_1021;
    uint32_t a_eap_modified;
    uint32_t b_frames_total;
    uint32_t b_frames_880;
    uint32_t b_frames_921;
    uint32_t b_busoff_count;
};

static_assert(sizeof(struct_message) == 52, "struct_message size mismatch");

struct __attribute__((packed)) legacy_message {
    uint32_t uptime;
    float hz_a;
    float hz_b;
    bool nag_active;
    bool eap_active;
    uint32_t echo_count;
    uint32_t tx_fail_count;
};

static_assert(sizeof(legacy_message) == 22, "legacy_message size mismatch");

static TFT_eSPI tft;
static TFT_eSprite canvas = TFT_eSprite(&tft);

static portMUX_TYPE gDataMux = portMUX_INITIALIZER_UNLOCKED;
static struct_message gLatestMsg = {};
static bool gHasData = false;
static uint32_t gLastRxMs = 0;
static uint8_t gLastSenderMac[6] = {0}; 
static uint32_t gRxPacketCount = 0;
static uint32_t gBadLengthPacketCount = 0;

static UiState gShown = {};

static constexpr uint8_t kWifiChannel = 1;
static constexpr uint32_t kRenderIntervalMs = 100;
static constexpr uint32_t kLinkTimeoutMs = 3200;
static constexpr uint32_t kNoSignalBlinkMs = 450;
static constexpr uint32_t kChannelCheckIntervalMs = 3000;
static constexpr uint32_t kPageButtonMinIntervalMs = 140;
static constexpr uint8_t kLinkMissDebounceFrames = 4;
static constexpr uint8_t kButtonUpPin = 14;
static constexpr uint8_t kButtonDownPin = 0;
static constexpr uint8_t kBacklightPin = 38;
static constexpr uint8_t kBrightnessStepPercent = 10;
static constexpr uint32_t kBrightnessEnterHoldMs = 3000;
static constexpr uint32_t kBrightnessIdleSaveMs = 3000;
static constexpr uint32_t kButtonDebounceMs = 30;
static constexpr uint32_t kBrightnessRepeatStartMs = 400;
static constexpr uint32_t kBrightnessRepeatIntervalMs = 120;
static constexpr uint8_t kBacklightLevelMax = 16;
static constexpr uint8_t kBacklightLevelMin = 1;
static constexpr uint8_t kNagModeDynamic = 0;
static constexpr uint8_t kNagModeFixed = 1;
static constexpr uint8_t kPageCount = 4;

enum class UiMode : uint8_t {
    PageView = 0,
    BrightnessAdjust = 1,
    SystemEdit = 2,
};

enum class SystemItem : uint8_t {
    CpuProfile = 0,
    WifiRuntime = 1,
    BluetoothBuild = 2,
    BrightnessQuick = 3,
};

static constexpr uint8_t kSystemItemCount = 4;
static constexpr uint32_t kSystemEditEnterHoldMs = 2000;
static constexpr uint32_t kSystemEditExecHoldMs = 1000;
static constexpr uint32_t kSystemEditExitHoldMs = 1500;

static uint8_t gCurrentPage = 0;
static bool gPrevBtnUp = true;
static bool gPrevBtnDown = true;
static uint32_t gLastPageChangeMs = 0;
static bool gPageDirty = false;
static uint32_t gLastChannelCheckMs = 0;
static bool gPrevLinked = false;
static uint8_t gLinkMissStreak = 0;
static UiMode gUiMode = UiMode::PageView;
static bool gLongPressConsumed = false;
static uint8_t gSavedPageBeforeBrightness = 0;
static uint8_t gBrightnessPercent = 80;
static uint32_t gBrightnessLastInputMs = 0;
static uint8_t gBacklightLevel = 0;
static uint32_t gCpuTargetMhz = UI_CPU_MHZ;
static bool gWifiRuntimeEnabled = (ENABLE_WIFI_RADIO != 0);
static bool gBluetoothRuntimeEnabled =
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
    true;
#else
    false;
#endif

static bool gUpRaw = false;
static bool gDownRaw = false;
static bool gUpStable = false;
static bool gDownStable = false;
static uint32_t gUpRawChangedMs = 0;
static uint32_t gDownRawChangedMs = 0;
static uint32_t gUpPressedStartMs = 0;
static uint32_t gDownPressedStartMs = 0;
static uint32_t gUpRepeatMs = 0;
static uint32_t gDownRepeatMs = 0;
static uint8_t gSystemSelected = 0;
static bool gSystemUpExecConsumed = false;
static bool gSystemDownExitConsumed = false;
static bool gBrightnessUpHoldConsumed = false;
static bool gBrightnessDownHoldConsumed = false;

static uint16_t colBg;
static uint16_t colPanel;
static uint16_t colText;
static uint16_t colMuted;
static uint16_t colOn;
static uint16_t colOff;
static uint16_t colHz;
static uint16_t colAccent;
static uint16_t colTrack;

static uint8_t clampBrightnessPercent(uint8_t value)
{
    if (value > 100) return 100;
    return (uint8_t)((value / kBrightnessStepPercent) * kBrightnessStepPercent);
}

static uint32_t sanitizeCpuProfile(uint32_t mhz)
{
    if (mhz <= 80) return 80;
    return 160;
}

static uint8_t brightnessPercentToLevel(uint8_t percent)
{
    uint8_t level = (uint8_t)(((uint32_t)percent * kBacklightLevelMax + 50u) / 100u);
    if (level < kBacklightLevelMin) level = kBacklightLevelMin;
    return level;
}

static void setBacklightLevel(uint8_t value)
{
    uint8_t target = (value > kBacklightLevelMax) ? kBacklightLevelMax : value;
    if (target < kBacklightLevelMin) target = kBacklightLevelMin;

    if (gBacklightLevel == 0) {
        digitalWrite(kBacklightPin, HIGH);
        gBacklightLevel = kBacklightLevelMax;
        delayMicroseconds(30);
    }

    const int from = (int)kBacklightLevelMax - (int)gBacklightLevel;
    const int to = (int)kBacklightLevelMax - (int)target;
    const int pulseCount = ((int)kBacklightLevelMax + to - from) % (int)kBacklightLevelMax;

    for (int i = 0; i < pulseCount; ++i) {
        digitalWrite(kBacklightPin, LOW);
        digitalWrite(kBacklightPin, HIGH);
    }

    gBacklightLevel = target;
}

static void applyBacklightPercent(uint8_t percent)
{
    gBrightnessPercent = clampBrightnessPercent(percent);
    setBacklightLevel(brightnessPercentToLevel(gBrightnessPercent));
}

static uint8_t loadBrightnessPercent(uint8_t fallback)
{
    Preferences prefs;
    uint8_t value = fallback;
    if (prefs.begin("settings", true)) {
        value = prefs.getUChar("bright", fallback);
        prefs.end();
    }
    return clampBrightnessPercent(value);
}

static void saveBrightnessPercent(uint8_t value)
{
    Preferences prefs;
    if (!prefs.begin("settings", false)) return;
    prefs.putUChar("bright", clampBrightnessPercent(value));
    prefs.end();
}

static void loadRuntimeSettings()
{
    Preferences prefs;
    if (!prefs.begin("settings", true)) {
        gCpuTargetMhz = sanitizeCpuProfile(UI_CPU_MHZ);
        gWifiRuntimeEnabled = (ENABLE_WIFI_RADIO != 0);
        gBluetoothRuntimeEnabled =
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
            true;
#else
            false;
#endif
        return;
    }

    gCpuTargetMhz = sanitizeCpuProfile(prefs.getUInt("cpu_mhz", UI_CPU_MHZ));
    gWifiRuntimeEnabled = prefs.getBool("wifi_on", (ENABLE_WIFI_RADIO != 0));
    gBluetoothRuntimeEnabled = prefs.getBool("bt_on", true);
    prefs.end();

#if !ENABLE_WIFI_RADIO
    gWifiRuntimeEnabled = false;
#endif

#if !(defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED)
    gBluetoothRuntimeEnabled = false;
#endif
}

static void saveCpuProfile(uint32_t mhz)
{
    Preferences prefs;
    if (!prefs.begin("settings", false)) return;
    prefs.putUInt("cpu_mhz", sanitizeCpuProfile(mhz));
    prefs.end();
}

static void saveWifiRuntimeEnabled(bool enabled)
{
    Preferences prefs;
    if (!prefs.begin("settings", false)) return;
    prefs.putBool("wifi_on", enabled);
    prefs.end();
}

static void saveBluetoothRuntimeEnabled(bool enabled)
{
    Preferences prefs;
    if (!prefs.begin("settings", false)) return;
    prefs.putBool("bt_on", enabled);
    prefs.end();
}

static const char* nagModeToText(uint8_t mode)
{
    return (mode == kNagModeFixed) ? "FIXED" : "DYNAMIC";
}

static const char* twaiStateToText(uint8_t state)
{
    if (state == 1) return "OK";
    if (state == 2) return "BUS_OFF";
    if (state == 3) return "RECOVER";
    return "INIT";
}

static bool initEspNowReceiver();

static const char* wifiStatusText()
{
#if ENABLE_WIFI_RADIO
    if (!gWifiRuntimeEnabled) return "OFF(RUNTIME)";
    if (WiFi.getMode() == WIFI_OFF) return "OFF(RUNTIME)";
    return "ON";
#else
    return "OFF(BUILD)";
#endif
}

static const char* btStatusText()
{
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
    if (!gBluetoothRuntimeEnabled) return "OFF(RUNTIME)";
    return "ON";
#else
    return "OFF(BUILD)";
#endif
}

static void applyWifiBtCoexistPolicy()
{
#if ENABLE_WIFI_RADIO
    if (WiFi.getMode() == WIFI_OFF) return;
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
    if (gBluetoothRuntimeEnabled) {
        WiFi.setSleep(true);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        Serial.println("[SYS] Coexist: WiFi modem sleep ON (BT enabled)");
        return;
    }
#endif
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.println("[SYS] Coexist: WiFi modem sleep OFF");
#endif
}

static void applyCpuProfile(uint32_t mhz)
{
    gCpuTargetMhz = sanitizeCpuProfile(mhz);
    if (getCpuFrequencyMhz() != gCpuTargetMhz) {
        if (!setCpuFrequencyMhz(gCpuTargetMhz)) {
            gCpuTargetMhz = 80;
            setCpuFrequencyMhz(gCpuTargetMhz);
            Serial.println("[SYS] CPU profile set failed, fallback to 80 MHz");
        }
        Serial.printf("[SYS] CPU profile -> %u MHz\n", (unsigned)getCpuFrequencyMhz());
    }
    saveCpuProfile(gCpuTargetMhz);
}

static void cycleCpuProfile()
{
    const uint32_t current = (uint32_t)getCpuFrequencyMhz();
    if (current <= 80) {
        applyCpuProfile(160);
    } else {
        applyCpuProfile(80);
    }
}

static UiRenderContext buildUiRenderContext()
{
    // 프레임마다 불변 컨텍스트 스냅샷 1개를 만듭니다.
    // ui.cpp는 이 구조체만 읽도록 해서 렌더 코드와 런타임 전역 상태를 분리합니다.
    UiRenderContext ctx;
    ctx.currentPage = gCurrentPage;
    ctx.pageCount = kPageCount;
    ctx.brightnessPercent = gBrightnessPercent;
    ctx.backlightLevel = gBacklightLevel;
    ctx.systemSelected = gSystemSelected;
    ctx.savedPageBeforeBrightness = gSavedPageBeforeBrightness;
    ctx.brightnessAdjustMode = (gUiMode == UiMode::BrightnessAdjust);
    ctx.systemEditMode = (gUiMode == UiMode::SystemEdit);
    ctx.bluetoothRuntimeEnabled = gBluetoothRuntimeEnabled;
    ctx.noSignalBlinkMs = kNoSignalBlinkMs;

    bool wifiOn = gWifiRuntimeEnabled;
#if ENABLE_WIFI_RADIO
    wifiOn = gWifiRuntimeEnabled && (WiFi.getMode() != WIFI_OFF);
#endif
    ctx.wifiRuntimeEnabled = wifiOn;

    ctx.colBg = colBg;
    ctx.colPanel = colPanel;
    ctx.colText = colText;
    ctx.colMuted = colMuted;
    ctx.colOn = colOn;
    ctx.colOff = colOff;
    ctx.colHz = colHz;
    ctx.colAccent = colAccent;
    ctx.colTrack = colTrack;
    return ctx;
}

static bool handlePageButtons()
{
    // 입력 처리 전략:
    // 1) 원시 입력 변화 감지, 2) 디바운스로 안정 엣지 확정,
    // 3) 홀드/릴리즈 시간 기반 모드별 동작(페이지/시스템/밝기) 실행.
    const uint32_t now = millis();
    const bool upRawNow = (digitalRead(kButtonUpPin) == LOW);
    const bool downRawNow = (digitalRead(kButtonDownPin) == LOW);

    if (upRawNow != gUpRaw) {
        gUpRaw = upRawNow;
        gUpRawChangedMs = now;
    }
    if (downRawNow != gDownRaw) {
        gDownRaw = downRawNow;
        gDownRawChangedMs = now;
    }

    bool upPressedEdge = false;
    bool upReleasedEdge = false;
    bool downPressedEdge = false;
    bool downReleasedEdge = false;

    if ((now - gUpRawChangedMs >= kButtonDebounceMs) && (gUpStable != gUpRaw)) {
        gUpStable = gUpRaw;
        upPressedEdge = gUpStable;
        upReleasedEdge = !gUpStable;
        if (upPressedEdge) {
            gUpPressedStartMs = now;
            gUpRepeatMs = now;
        }
    }

    if ((now - gDownRawChangedMs >= kButtonDebounceMs) && (gDownStable != gDownRaw)) {
        gDownStable = gDownRaw;
        downPressedEdge = gDownStable;
        downReleasedEdge = !gDownStable;
        if (downPressedEdge) {
            gDownPressedStartMs = now;
            gDownRepeatMs = now;
        }
    }

    if (gUiMode == UiMode::BrightnessAdjust) {
        bool changed = false;
        if (upPressedEdge) {
            gBrightnessUpHoldConsumed = false;
            gBrightnessLastInputMs = now;
        }

        if (downPressedEdge) {
            gBrightnessDownHoldConsumed = false;
            gBrightnessLastInputMs = now;
        }

        if (gUpStable && (now - gUpPressedStartMs >= kBrightnessRepeatStartMs) &&
            (now - gUpRepeatMs >= kBrightnessRepeatIntervalMs)) {
            if (gBrightnessPercent <= (uint8_t)(100 - kBrightnessStepPercent)) {
                applyBacklightPercent((uint8_t)(gBrightnessPercent + kBrightnessStepPercent));
            }
            gUpRepeatMs = now;
            gBrightnessUpHoldConsumed = true;
            gBrightnessLastInputMs = now;
            gPageDirty = true;
            changed = true;
        }

        if (gDownStable && (now - gDownPressedStartMs >= kBrightnessRepeatStartMs) &&
            (now - gDownRepeatMs >= kBrightnessRepeatIntervalMs)) {
            if (gBrightnessPercent >= kBrightnessStepPercent) {
                applyBacklightPercent((uint8_t)(gBrightnessPercent - kBrightnessStepPercent));
            }
            gDownRepeatMs = now;
            gBrightnessDownHoldConsumed = true;
            gBrightnessLastInputMs = now;
            gPageDirty = true;
            changed = true;
        }

        if (upReleasedEdge) {
            if (!gBrightnessUpHoldConsumed && gBrightnessPercent <= (uint8_t)(100 - kBrightnessStepPercent)) {
                applyBacklightPercent((uint8_t)(gBrightnessPercent + kBrightnessStepPercent));
                gBrightnessLastInputMs = now;
                gPageDirty = true;
                changed = true;
            }
            gBrightnessUpHoldConsumed = false;
        }

        if (downReleasedEdge) {
            if (!gBrightnessDownHoldConsumed && gBrightnessPercent >= kBrightnessStepPercent) {
                applyBacklightPercent((uint8_t)(gBrightnessPercent - kBrightnessStepPercent));
                gBrightnessLastInputMs = now;
                gPageDirty = true;
                changed = true;
            }
            gBrightnessDownHoldConsumed = false;
        }

        if (now - gBrightnessLastInputMs >= kBrightnessIdleSaveMs) {
            saveBrightnessPercent(gBrightnessPercent);
            gUiMode = UiMode::PageView;
            gCurrentPage = gSavedPageBeforeBrightness;
            gPageDirty = true;
            gLongPressConsumed = true;
            changed = true;
        }
        return changed;
    }

    if (gUiMode == UiMode::SystemEdit) {
        bool changed = false;

        if (upReleasedEdge && !gSystemUpExecConsumed) {
            gSystemSelected = (uint8_t)((gSystemSelected + kSystemItemCount - 1) % kSystemItemCount);
            gPageDirty = true;
            changed = true;
        }
        if (downReleasedEdge && !gSystemDownExitConsumed) {
            gSystemSelected = (uint8_t)((gSystemSelected + 1) % kSystemItemCount);
            gPageDirty = true;
            changed = true;
        }

        if (gUpStable && !gSystemUpExecConsumed && (now - gUpPressedStartMs >= kSystemEditExecHoldMs)) {
            const SystemItem item = (SystemItem)gSystemSelected;
            if (item == SystemItem::CpuProfile) {
                cycleCpuProfile();
            } else if (item == SystemItem::WifiRuntime) {
#if ENABLE_WIFI_RADIO
                if (!gWifiRuntimeEnabled || WiFi.getMode() == WIFI_OFF) {
                    if (initEspNowReceiver()) {
                        gWifiRuntimeEnabled = true;
                        saveWifiRuntimeEnabled(true);
                    }
                } else {
                    esp_now_deinit();
                    WiFi.mode(WIFI_OFF);
                    gWifiRuntimeEnabled = false;
                    saveWifiRuntimeEnabled(false);
                    Serial.println("[SYS] WiFi runtime -> OFF");
                }
#endif
            } else if (item == SystemItem::BluetoothBuild) {
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
                if (gBluetoothRuntimeEnabled) {
                    if (btStarted()) {
                        btStop();
                    }
                    gBluetoothRuntimeEnabled = false;
                    saveBluetoothRuntimeEnabled(false);
                    Serial.println("[SYS] Bluetooth runtime -> OFF");
                } else {
                    if (!btStarted()) {
                        btStart();
                    }
                    gBluetoothRuntimeEnabled = true;
                    saveBluetoothRuntimeEnabled(true);
                    Serial.println("[SYS] Bluetooth runtime -> ON");
                }
                applyWifiBtCoexistPolicy();
#endif
            } else if (item == SystemItem::BrightnessQuick) {
                gSavedPageBeforeBrightness = gCurrentPage;
                gUiMode = UiMode::BrightnessAdjust;
                gBrightnessUpHoldConsumed = false;
                gBrightnessDownHoldConsumed = false;
                gBrightnessLastInputMs = now;
            }
            gSystemUpExecConsumed = true;
            gPageDirty = true;
            changed = true;
        }

        if (gDownStable && !gSystemDownExitConsumed && (now - gDownPressedStartMs >= kSystemEditExitHoldMs)) {
            gUiMode = UiMode::PageView;
            gSystemDownExitConsumed = true;
            gPageDirty = true;
            changed = true;
        }

        if (upReleasedEdge) gSystemUpExecConsumed = false;
        if (downReleasedEdge) gSystemDownExitConsumed = false;
        return changed;
    }

    if (gCurrentPage == 3 && gDownStable && !gSystemDownExitConsumed && (now - gDownPressedStartMs >= kSystemEditEnterHoldMs)) {
        gUiMode = UiMode::SystemEdit;
        gSystemSelected = 0;
        gSystemUpExecConsumed = false;
        gSystemDownExitConsumed = true;
        gPageDirty = true;
        return true;
    }

    if (gUpStable) {
        if (upPressedEdge) {
            gLongPressConsumed = false;
        } else if (!gLongPressConsumed && (now - gUpPressedStartMs >= kBrightnessEnterHoldMs)) {
            gSavedPageBeforeBrightness = gCurrentPage;
            gUiMode = UiMode::BrightnessAdjust;
            gBrightnessUpHoldConsumed = false;
            gBrightnessDownHoldConsumed = false;
            gBrightnessLastInputMs = now;
            gPageDirty = true;
            gLongPressConsumed = true;
            return true;
        }
    }

    // 클릭 1회당 페이지 1단계만 이동시켜 과도한 점프를 방지합니다.
    if (now - gLastPageChangeMs < kPageButtonMinIntervalMs) {
        return false;
    }

    bool changed = false;

    if (downReleasedEdge) {
        gCurrentPage = (uint8_t)((gCurrentPage + 1) % kPageCount);
        gPageDirty = true;
        changed = true;
    } else if (!gLongPressConsumed && upReleasedEdge) {
        gCurrentPage = (uint8_t)((gCurrentPage + kPageCount - 1) % kPageCount);
        gPageDirty = true;
        changed = true;
    }

    if (changed) gLastPageChangeMs = now;
    if (!gUpStable) gLongPressConsumed = false;
    if (!gDownStable) gSystemDownExitConsumed = false;

    (void)upReleasedEdge;
    (void)downReleasedEdge;
    return changed;
}

static bool initEspNowReceiver()
{
#if !ENABLE_WIFI_RADIO
    Serial.println("[ESP-NOW] disabled by ENABLE_WIFI_RADIO=0");
    return false;
#else
    WiFi.mode(WIFI_STA);
    applyWifiBtCoexistPolicy();

    // 송신기/수신기 안정 페어링을 위해 WiFi 채널을 고정합니다.
    // 일부 보드는 promiscuous 토글을 거쳐야 채널 적용이 더 안정적입니다.
    esp_wifi_set_promiscuous(true);
    esp_err_t chRet = esp_wifi_set_channel(kWifiChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    if (chRet != ESP_OK) {
        Serial.printf("[ESP-NOW] channel set failed: %d\n", (int)chRet);
    }

    Serial.printf("[ESP-NOW] Local MAC (STA): %s\n", WiFi.macAddress().c_str());
    Serial.printf("[ESP-NOW] WiFi channel fixed: %u\n", (unsigned)kWifiChannel);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] init failed");
        return false;
    }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_now_register_recv_cb([](const esp_now_recv_info_t* info, const uint8_t* data, int len) {
        struct_message parsed = {};
        if (len == (int)sizeof(struct_message)) {
            memcpy(&parsed, data, sizeof(parsed));
        } else if (len == (int)sizeof(legacy_message)) {
            legacy_message legacy = {};
            memcpy(&legacy, data, sizeof(legacy));
            parsed.uptime = legacy.uptime;
            parsed.hz_a = legacy.hz_a;
            parsed.hz_b = legacy.hz_b;
            parsed.nag_active = legacy.nag_active;
            parsed.eap_active = legacy.eap_active;
            parsed.nag_mode = kNagModeDynamic;
            parsed.twai_state = 0;
            parsed.echo_count = legacy.echo_count;
            parsed.tx_fail_count = legacy.tx_fail_count;
        } else {
            gBadLengthPacketCount++;
            return;
        }

        static bool firstRxLogged = false;
        portENTER_CRITICAL_ISR(&gDataMux);
        gLatestMsg = parsed;
        gHasData = true;
        gLastRxMs = millis();
        gRxPacketCount++;
        if (info && info->src_addr) memcpy(gLastSenderMac, info->src_addr, 6);
        portEXIT_CRITICAL_ISR(&gDataMux);

        if (!firstRxLogged && info && info->src_addr) {
            firstRxLogged = true;
            Serial.printf("[ESP-NOW] first packet from %02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
                          info->src_addr[0], info->src_addr[1], info->src_addr[2],
                          info->src_addr[3], info->src_addr[4], info->src_addr[5], len);
        }
    });
#else
    esp_now_register_recv_cb([](const uint8_t* mac, const uint8_t* data, int len) {
        struct_message parsed = {};
        if (len == (int)sizeof(struct_message)) {
            memcpy(&parsed, data, sizeof(parsed));
        } else if (len == (int)sizeof(legacy_message)) {
            legacy_message legacy = {};
            memcpy(&legacy, data, sizeof(legacy));
            parsed.uptime = legacy.uptime;
            parsed.hz_a = legacy.hz_a;
            parsed.hz_b = legacy.hz_b;
            parsed.nag_active = legacy.nag_active;
            parsed.eap_active = legacy.eap_active;
            parsed.nag_mode = kNagModeDynamic;
            parsed.twai_state = 0;
            parsed.echo_count = legacy.echo_count;
            parsed.tx_fail_count = legacy.tx_fail_count;
        } else {
            gBadLengthPacketCount++;
            return;
        }

        static bool firstRxLogged = false;
        portENTER_CRITICAL_ISR(&gDataMux);
        gLatestMsg = parsed;
        gHasData = true;
        gLastRxMs = millis();
        gRxPacketCount++;
        if (mac) memcpy(gLastSenderMac, mac, 6);
        portEXIT_CRITICAL_ISR(&gDataMux);

        if (!firstRxLogged && mac) {
            firstRxLogged = true;
            Serial.printf("[ESP-NOW] first packet from %02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
                          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], len);
        }
    });
#endif

    Serial.println("[ESP-NOW] receiver ready");
    return true;
#endif
}

static void ensureReceiverChannel()
{
#if ENABLE_WIFI_RADIO
    const uint32_t now = millis();
    if (now - gLastChannelCheckMs < kChannelCheckIntervalMs) return;
    gLastChannelCheckMs = now;

    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) != ESP_OK) return;
    if (primary == kWifiChannel) return;

    Serial.printf("[ESP-NOW] channel drift detected: %u -> %u\n", (unsigned)primary, (unsigned)kWifiChannel);
    esp_wifi_set_promiscuous(true);
    esp_err_t chRet = esp_wifi_set_channel(kWifiChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    if (chRet != ESP_OK) {
        Serial.printf("[ESP-NOW] channel restore failed: %d\n", (int)chRet);
    } else {
        Serial.println("[ESP-NOW] channel restored");
    }
#endif
}

void setup()
{
    // 1) 무선/화면 초기화 전에 저장된 런타임 설정을 먼저 복원합니다.
    Serial.begin(115200);

    loadRuntimeSettings();

#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
    if (gBluetoothRuntimeEnabled) {
        if (!btStarted()) {
            btStart();
        }
    } else {
        if (btStarted()) {
            btStop();
        }
    }
#endif

    if (getCpuFrequencyMhz() != gCpuTargetMhz) {
        setCpuFrequencyMhz(gCpuTargetMhz);
    }
    Serial.printf("[SYS] CPU %u MHz (target=%u)\n", (unsigned)getCpuFrequencyMhz(), (unsigned)gCpuTargetMhz);

    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);

    pinMode(kButtonUpPin, INPUT_PULLUP);
    pinMode(kButtonDownPin, INPUT_PULLUP);
    gUpRaw = (digitalRead(kButtonUpPin) == LOW);
    gDownRaw = (digitalRead(kButtonDownPin) == LOW);
    gUpStable = gUpRaw;
    gDownStable = gDownRaw;
    gUpRawChangedMs = millis();
    gDownRawChangedMs = millis();

    pinMode(kBacklightPin, OUTPUT);
    applyBacklightPercent(loadBrightnessPercent(gBrightnessPercent));

    tft.init();
    tft.setRotation(1);

    colBg = tft.color565(8, 11, 15);
    colPanel = tft.color565(20, 28, 38);
    colText = tft.color565(232, 238, 245);
    colMuted = tft.color565(145, 160, 178);
    colOn = tft.color565(40, 220, 120);
    colOff = tft.color565(245, 74, 74);
    colHz = tft.color565(250, 208, 58);
    colAccent = tft.color565(45, 70, 94);
    colTrack = tft.color565(30, 125, 188);

    if (!canvas.createSprite(320, 170)) {
        Serial.println("[UI] sprite allocation failed");
        tft.fillScreen(TFT_BLACK);
    }

    UiState boot;
    boot.linked = false;
    // 부팅 직후 초기 화면을 즉시 그려 사용자에게 확정된 상태를 보여줍니다.
    uiRender(canvas, boot, buildUiRenderContext());

    if (gWifiRuntimeEnabled) {
        initEspNowReceiver();
    } else {
        Serial.println("[ESP-NOW] skipped by persisted runtime setting");
    }
}

void loop()
{
    static uint32_t lastRenderMs = 0;

    // 홀드/릴리즈 타이밍 정확도를 위해 버튼은 매 loop마다 폴링합니다.
    const bool buttonChanged = handlePageButtons();
    ensureReceiverChannel();

    uint32_t now = millis();
    const uint32_t renderInterval = (gUiMode == UiMode::BrightnessAdjust) ? 33 : kRenderIntervalMs;
    if (now - lastRenderMs < renderInterval) {
        return;
    }
    lastRenderMs = now;

    struct_message msg = {};
    bool hasData = false;
    uint32_t lastRx = 0;

    portENTER_CRITICAL(&gDataMux);
    msg = gLatestMsg;
    hasData = gHasData;
    lastRx = gLastRxMs;
    portEXIT_CRITICAL(&gDataMux);

    // 최신 수신 패킷 스냅샷으로 다음 프레임 상태를 구성합니다.
    UiState next;
    next.uptime = now / 1000u;
    next.hzA = msg.hz_a;
    next.hzB = msg.hz_b;
    next.nag = msg.nag_active;
    next.eap = msg.eap_active;
    next.nagMode = msg.nag_mode;
    next.twaiState = msg.twai_state;
    next.echoCount = msg.echo_count;
    next.txFailCount = msg.tx_fail_count;
    next.aFramesTotal = msg.a_frames_total;
    next.aFrames1021 = msg.a_frames_1021;
    next.aEapModified = msg.a_eap_modified;
    next.bFramesTotal = msg.b_frames_total;
    next.bFrames880 = msg.b_frames_880;
    next.bFrames921 = msg.b_frames_921;
    next.bBusoffCount = msg.b_busoff_count;

    // RF 지터로 생기는 짧은 패킷 공백에서 오탐(NO SIGNAL) 방지를 위해 디바운스를 둡니다.
    const bool linkFresh = hasData && (now - lastRx <= kLinkTimeoutMs);
    if (linkFresh) {
        gLinkMissStreak = 0;
        next.linked = true;
    } else {
        if (gLinkMissStreak < 255) gLinkMissStreak++;
        next.linked = (gLinkMissStreak < kLinkMissDebounceFrames);
    }

    if (next.linked != gPrevLinked) {
        if (next.linked) {
            Serial.printf("[LINK] reconnected. rx_pkts=%lu bad_len=%lu tx_fail=%lu\n",
                          (unsigned long)gRxPacketCount,
                          (unsigned long)gBadLengthPacketCount,
                          (unsigned long)next.txFailCount);
        } else {
            const uint32_t age = hasData ? (now - lastRx) : 0;
            Serial.printf("[LINK] lost. age=%lums rx_pkts=%lu bad_len=%lu last_tx_fail=%lu\n",
                          (unsigned long)age,
                          (unsigned long)gRxPacketCount,
                          (unsigned long)gBadLengthPacketCount,
                          (unsigned long)next.txFailCount);
        }
        gPrevLinked = next.linked;
    }

    if (!next.linked && now > kLinkTimeoutMs) {
        next.hzA = 0.0f;
        next.hzB = 0.0f;
    }

    // 버튼 이벤트가 있거나 화면 데이터가 바뀐 경우에만 렌더링합니다.
    if (buttonChanged || gPageDirty || (gUiMode == UiMode::BrightnessAdjust) || uiNeedsRender(gShown, next, gCurrentPage, kPageCount) || !next.linked) {
        uiRender(canvas, next, buildUiRenderContext());
        gShown = next;
        gPageDirty = false;
    }
}
