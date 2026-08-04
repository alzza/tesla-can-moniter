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
#include <HTTPClient.h>
#include <cJSON.h>
#endif

static TFT_eSPI tft;
static TFT_eSprite canvas = TFT_eSprite(&tft);

static portMUX_TYPE gDataMux = portMUX_INITIALIZER_UNLOCKED;
static UiState gLatestState = {};
static bool gHasData = false;
static uint32_t gLastRxMs = 0;
static uint32_t gRequestOkCount = 0;
static uint32_t gRequestFailCount = 0;
static uint32_t gParseFailCount = 0;
static uint32_t gLastRequestMs = 0;
static uint32_t gLastReconnectMs = 0;

static UiState gShown = {};

static constexpr char kT2CanSsid[] = "TeslaCAN";
static constexpr char kT2CanPassword[] = "asdf1234";
static constexpr char kMonitorUrl[] = "http://192.168.4.1/api/monitor";
static constexpr uint32_t kMonitorSchema = 1;
static constexpr uint32_t kMonitorPollMs = 1000;
static constexpr uint32_t kWifiReconnectMs = 2000;
static constexpr uint32_t kRenderIntervalMs = 100;
static constexpr uint32_t kLinkTimeoutMs = 3200;
static constexpr uint32_t kNoSignalBlinkMs = 450;
static constexpr uint32_t kPageButtonMinIntervalMs = 140;
static constexpr uint8_t kButtonUpPin = 14;
static constexpr uint8_t kButtonDownPin = 0;
static constexpr uint8_t kBacklightPin = 38;
static constexpr uint8_t kBrightnessStepPercent = 10;
static constexpr uint32_t kBrightnessEnterHoldMs = 3000;
static constexpr uint32_t kThemeToggleHoldMs = 3000;
static constexpr uint32_t kThemeToastMs = 800;
static constexpr uint32_t kBrightnessIdleSaveMs = 3000;
static constexpr uint32_t kButtonDebounceMs = 30;
static constexpr uint32_t kBrightnessRepeatStartMs = 400;
static constexpr uint32_t kBrightnessRepeatIntervalMs = 120;
static constexpr uint8_t kBacklightLevelMax = 16;
static constexpr uint8_t kBacklightLevelMin = 1;
static constexpr uint8_t kPageCount = 5;

enum class UiMode : uint8_t {
    PageView = 0,
    BrightnessAdjust = 1,
    SystemEdit = 2,
};

enum class SystemItem : uint8_t {
    CpuProfile = 0,
    BrightnessQuick = 1,
    Theme = 2,
};

static constexpr uint8_t kSystemItemCount = 3;
static constexpr uint32_t kSystemEditEnterHoldMs = 2000;
static constexpr uint32_t kSystemEditExecHoldMs = 1000;
static constexpr uint32_t kSystemEditExitHoldMs = 1500;

static uint8_t gCurrentPage = 0;
static uint32_t gLastPageChangeMs = 0;
static bool gPageDirty = false;
static bool gPrevLinked = false;
static UiMode gUiMode = UiMode::PageView;
static bool gLongPressConsumed = false;
static uint8_t gSavedPageBeforeBrightness = 0;
static uint8_t gBrightnessPercent = 80;
static uint32_t gBrightnessLastInputMs = 0;
static uint8_t gBacklightLevel = 0;
static uint32_t gCpuTargetMhz = UI_CPU_MHZ;
static bool gWifiRuntimeEnabled = (ENABLE_WIFI_RADIO != 0);
static bool gBluetoothRuntimeEnabled = false;

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
static bool gBrightnessWaitRelease = false;
static bool gThemeHoldConsumed = false;
static UiTheme gTheme = UiTheme::Dark;
static uint32_t gThemeToastUntilMs = 0;

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

static UiTheme sanitizeTheme(uint8_t raw)
{
    return (raw == (uint8_t)UiTheme::Light) ? UiTheme::Light : UiTheme::Dark;
}

static void saveTheme(UiTheme theme)
{
    Preferences prefs;
    if (!prefs.begin("settings", false)) return;
    prefs.putUChar("theme", (uint8_t)theme);
    prefs.end();
}

static void cycleTheme()
{
    gTheme = (gTheme == UiTheme::Dark) ? UiTheme::Light : UiTheme::Dark;
    saveTheme(gTheme);
    gThemeToastUntilMs = millis() + kThemeToastMs;
    Serial.printf("[SYS] Theme -> %s\n", (gTheme == UiTheme::Light) ? "LIGHT" : "DARK");
}

static void loadRuntimeSettings()
{
    Preferences prefs;
    if (!prefs.begin("settings", true)) {
        gCpuTargetMhz = sanitizeCpuProfile(UI_CPU_MHZ);
        gWifiRuntimeEnabled = (ENABLE_WIFI_RADIO != 0);
        gBluetoothRuntimeEnabled = false;
        gTheme = UiTheme::Dark;
        return;
    }

    gCpuTargetMhz = sanitizeCpuProfile(prefs.getUInt("cpu_mhz", UI_CPU_MHZ));
    // 모니터는 자동 연결이 필수이므로 과거 WiFi/BT NVS 토글은 무시한다.
    gWifiRuntimeEnabled = (ENABLE_WIFI_RADIO != 0);
    gBluetoothRuntimeEnabled = false;
    gTheme = sanitizeTheme(prefs.getUChar("theme", (uint8_t)UiTheme::Dark));
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

static bool initMonitorWifi();
static void serviceMonitorClient();

static void applyWifiBtCoexistPolicy()
{
#if ENABLE_WIFI_RADIO
    if (WiFi.getMode() == WIFI_OFF) return;

    // 1초 HTTP 폴링의 재연결 지연을 줄이기 위해 WiFi sleep을 끄고 유지한다.
    WiFi.setSleep(false);
    Serial.println("[SYS] WiFi modem sleep OFF (T2-CAN monitor)");
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
    ctx.themeToastUntilMs = gThemeToastUntilMs;
    ctx.noSignalBlinkMs = kNoSignalBlinkMs;

    bool wifiOn = gWifiRuntimeEnabled;
#if ENABLE_WIFI_RADIO
    wifiOn = gWifiRuntimeEnabled && (WiFi.getMode() != WIFI_OFF);
#endif
    ctx.wifiRuntimeEnabled = wifiOn;

    uiApplyThemePalette(ctx, gTheme);
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
        // 밝기 화면 진입 직후에는 기존 롱프레스 입력을 무시하고,
        // 버튼을 한 번 떼야 다음 입력부터 반영한다.
        if (gBrightnessWaitRelease) {
            gBrightnessLastInputMs = now;
            if (!gUpStable && !gDownStable) {
                gBrightnessWaitRelease = false;
            }
            return false;
        }

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
            } else if (item == SystemItem::BrightnessQuick) {
                gSavedPageBeforeBrightness = gCurrentPage;
                gUiMode = UiMode::BrightnessAdjust;
                gBrightnessUpHoldConsumed = false;
                gBrightnessDownHoldConsumed = false;
                gBrightnessLastInputMs = now;
                gBrightnessWaitRelease = true;
            } else if (item == SystemItem::Theme) {
                cycleTheme();
                gPageDirty = true;
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

    // SYSTEM 페이지: DOWN 2초 → 편집 진입 (테마 단축키보다 우선)
    if (gCurrentPage == 4 && gDownStable && !gSystemDownExitConsumed && (now - gDownPressedStartMs >= kSystemEditEnterHoldMs)) {
        gUiMode = UiMode::SystemEdit;
        gSystemSelected = 0;
        gSystemUpExecConsumed = false;
        gSystemDownExitConsumed = true;
        gThemeHoldConsumed = true;
        gPageDirty = true;
        return true;
    }

    // A안: SYSTEM 이외 페이지에서 DOWN 3초 → 테마 토글
    if (gCurrentPage != 4 && gDownStable && !gThemeHoldConsumed &&
        (now - gDownPressedStartMs >= kThemeToggleHoldMs)) {
        cycleTheme();
        gThemeHoldConsumed = true;
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
            gBrightnessWaitRelease = true;
            gPageDirty = true;
            gLongPressConsumed = true;
            return true;
        }
    }

    if (downPressedEdge) {
        gThemeHoldConsumed = false;
    }

    // 클릭 1회당 페이지 1단계만 이동시켜 과도한 점프를 방지합니다.
    if (now - gLastPageChangeMs < kPageButtonMinIntervalMs) {
        return false;
    }

    bool changed = false;

    if (upReleasedEdge) {
        gCurrentPage = (uint8_t)((gCurrentPage + 1) % kPageCount);
        gPageDirty = true;
        changed = true;
    } else if (!gThemeHoldConsumed && !gLongPressConsumed && downReleasedEdge) {
        // 테마 롱프레스 직후 릴리즈는 페이지 이동으로 취급하지 않습니다.
        gCurrentPage = (uint8_t)((gCurrentPage + kPageCount - 1) % kPageCount);
        gPageDirty = true;
        changed = true;
    }

    if (changed) gLastPageChangeMs = now;
    if (!gUpStable) gLongPressConsumed = false;
    if (!gDownStable) {
        gSystemDownExitConsumed = false;
        // 홀드 소비 플래그는 다음 press에서 리셋 (릴리즈 직후 페이지 점프 방지)
    }

    (void)upReleasedEdge;
    (void)downReleasedEdge;
    return changed;
}

static cJSON* jsonChild(cJSON* parent, const char* key)
{
    return parent ? cJSON_GetObjectItemCaseSensitive(parent, key) : nullptr;
}

static uint32_t jsonU32(cJSON* parent, const char* key, uint32_t fallback = 0)
{
    cJSON* item = jsonChild(parent, key);
    return cJSON_IsNumber(item) && item->valuedouble >= 0.0
        ? (uint32_t)item->valuedouble : fallback;
}

static float jsonFloat(cJSON* parent, const char* key, float fallback = 0.0f)
{
    cJSON* item = jsonChild(parent, key);
    return cJSON_IsNumber(item) ? (float)item->valuedouble : fallback;
}

static bool jsonBool(cJSON* parent, const char* key, bool fallback = false)
{
    cJSON* item = jsonChild(parent, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static void jsonText(cJSON* parent, const char* key, char* out, size_t outSize, const char* fallback)
{
    cJSON* item = jsonChild(parent, key);
    const char* value = cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
    strlcpy(out, value, outSize);
}

static bool parseMonitorPayload(const String& payload, UiState& out)
{
    cJSON* root = cJSON_ParseWithLength(payload.c_str(), payload.length());
    if (!root) return false;
    const uint32_t schema = jsonU32(root, "schema", 0);
    if (schema != kMonitorSchema) {
        cJSON_Delete(root);
        return false;
    }

    out = {};
    out.schemaOk = true;
    out.t2Uptime = jsonU32(root, "uptime_s");
    out.otaState = jsonU32(root, "ota_state");
    jsonText(root, "firmware", out.firmware, sizeof(out.firmware), "--");
    jsonText(root, "build", out.build, sizeof(out.build), "--");

    cJSON* a = jsonChild(root, "a");
    out.aHealthLevel = (uint8_t)jsonU32(a, "level", 2);
    jsonText(a, "state", out.aHealthState, sizeof(out.aHealthState), "INIT");
    out.hzA = jsonFloat(a, "hz");
    out.aFrameAgeMs = jsonU32(a, "age_ms");
    out.aBusoffCount = jsonU32(a, "busoff");
    out.aEflg = jsonU32(a, "eflg");
    out.aTec = jsonU32(a, "tec");
    out.aRec = jsonU32(a, "rec");
    out.aRxOverrun = jsonU32(a, "rx_overrun");
    out.aTxQueued = jsonU32(a, "tx_q");
    out.aTxBusy = jsonU32(a, "tx_busy");
    out.aTxHard = jsonU32(a, "tx_hard");
    out.aTxGuard = jsonBool(a, "tx_guard");

    cJSON* b = jsonChild(root, "b");
    out.bHealthLevel = (uint8_t)jsonU32(b, "level", 2);
    jsonText(b, "state", out.bHealthState, sizeof(out.bHealthState), "INIT");
    out.hzB = jsonFloat(b, "hz");
    out.bFrameAgeMs = jsonU32(b, "age_ms");
    out.bBusoffCount = jsonU32(b, "busoff");
    out.bTwaiState = jsonU32(b, "twai");
    out.bTec = jsonU32(b, "tec");
    out.bRec = jsonU32(b, "rec");
    out.bRecoveryQuietMs = jsonU32(b, "recovery_quiet_ms");
    out.bArbLost = jsonU32(b, "arb_lost");
    out.bBusError = jsonU32(b, "bus_error");
    out.bTxFailed = jsonU32(b, "tx_failed");
    out.bEchoCount = jsonU32(b, "echo");

    cJSON* f = jsonChild(root, "features");
    out.eceR79 = jsonBool(f, "ece_r79");
    out.summon = jsonBool(f, "summon");
    out.tsllc = jsonBool(f, "tsllc");
    out.nag = jsonBool(f, "nag");
    out.nagMode = (uint8_t)jsonU32(f, "nag_mode");
    out.nagApOnly = jsonBool(f, "nag_ap_only");
    out.nagReady = jsonBool(f, "nag_ready");

    cJSON* gate = jsonChild(root, "gate");
    out.gateOpen = jsonBool(gate, "open");
    jsonText(gate, "reason", out.gateReason, sizeof(out.gateReason), "UNKNOWN");
    out.apActive = jsonBool(gate, "ap");
    out.apState = (uint8_t)jsonU32(gate, "ap_state");
    out.apStableMs = jsonU32(gate, "ap_stable_ms");
    out.parked = jsonBool(gate, "parked");
    out.summoning = jsonBool(gate, "summoning");
    out.summonTxOk = jsonU32(gate, "tx_ok");
    out.summonTxFail = jsonU32(gate, "tx_fail");
    out.summonBlocked = jsonU32(gate, "blocked");

    cJSON* mark = jsonChild(root, "user_mark");
    out.userMarkActive = jsonBool(mark, "active");
    out.userMarkCount = jsonU32(mark, "count");
    cJSON_Delete(root);
    return true;
}

static bool initMonitorWifi()
{
#if !ENABLE_WIFI_RADIO
    Serial.println("[MON] WiFi disabled by build flag");
    return false;
#else
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
    if (btStarted()) btStop();
#endif
    gBluetoothRuntimeEnabled = false;
    gWifiRuntimeEnabled = true;
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    applyWifiBtCoexistPolicy();
    WiFi.begin(kT2CanSsid, kT2CanPassword);
    gLastReconnectMs = millis();
    Serial.printf("[MON] connecting to %s\n", kT2CanSsid);
    return true;
#endif
}

static void serviceMonitorClient()
{
#if ENABLE_WIFI_RADIO
    const uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        if (now - gLastReconnectMs >= kWifiReconnectMs) {
            gLastReconnectMs = now;
            WiFi.disconnect(false, false);
            WiFi.begin(kT2CanSsid, kT2CanPassword);
            Serial.println("[MON] TeslaCAN reconnect");
        }
        return;
    }
    if (now - gLastRequestMs < kMonitorPollMs) return;
    gLastRequestMs = now;

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(500);
    http.setTimeout(700);
    if (!http.begin(client, kMonitorUrl)) {
        gRequestFailCount++;
        return;
    }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        gRequestFailCount++;
        http.end();
        return;
    }
    const String payload = http.getString();
    http.end();
    UiState parsed;
    if (!parseMonitorPayload(payload, parsed)) {
        gParseFailCount++;
        return;
    }
    gRequestOkCount++;
    parsed.wifiRssi = WiFi.RSSI();
    portENTER_CRITICAL(&gDataMux);
    gLatestState = parsed;
    gHasData = true;
    gLastRxMs = millis();
    portEXIT_CRITICAL(&gDataMux);
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
    Serial.printf("[SYS] Theme %s\n", (gTheme == UiTheme::Light) ? "LIGHT" : "DARK");

    if (!canvas.createSprite(320, 170)) {
        Serial.println("[UI] sprite allocation failed");
        tft.fillScreen(TFT_BLACK);
    }

    UiState boot;
    boot.linked = false;
    // 부팅 직후 초기 화면을 즉시 그려 사용자에게 확정된 상태를 보여줍니다.
    uiRender(canvas, boot, buildUiRenderContext());

    initMonitorWifi();
}

void loop()
{
    static uint32_t lastRenderMs = 0;

    // 홀드/릴리즈 타이밍 정확도를 위해 버튼은 매 loop마다 폴링합니다.
    const bool buttonChanged = handlePageButtons();
    serviceMonitorClient();

    uint32_t now = millis();
    const uint32_t renderInterval = (gUiMode == UiMode::BrightnessAdjust) ? 33 : kRenderIntervalMs;
    if (now - lastRenderMs < renderInterval) {
        return;
    }
    lastRenderMs = now;

    UiState next = {};
    bool hasData = false;
    uint32_t lastRx = 0;

    portENTER_CRITICAL(&gDataMux);
    next = gLatestState;
    hasData = gHasData;
    lastRx = gLastRxMs;
    portEXIT_CRITICAL(&gDataMux);

    next.localUptime = now / 1000U;
    next.responseAgeMs = hasData ? now - lastRx : 0;
    next.requestOk = gRequestOkCount;
    next.requestFail = gRequestFailCount;
    next.parseFail = gParseFailCount;
    next.wifiRssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
    next.linked = hasData && next.schemaOk && next.responseAgeMs <= kLinkTimeoutMs;

    if (next.linked != gPrevLinked) {
        if (next.linked) {
            Serial.printf("[LINK] T2-CAN connected. ok=%lu fail=%lu parse=%lu\n",
                          (unsigned long)gRequestOkCount,
                          (unsigned long)gRequestFailCount,
                          (unsigned long)gParseFailCount);
        } else {
            const uint32_t age = hasData ? (now - lastRx) : 0;
            Serial.printf("[LINK] T2-CAN lost. age=%lums ok=%lu fail=%lu parse=%lu\n",
                          (unsigned long)age,
                          (unsigned long)gRequestOkCount,
                          (unsigned long)gRequestFailCount,
                          (unsigned long)gParseFailCount);
        }
        gPrevLinked = next.linked;
    }

    // 테마 토스트 표시 중/종료 시에도 한 프레임 갱신합니다.
    bool themeToastActive = false;
    if (gThemeToastUntilMs != 0) {
        if (now < gThemeToastUntilMs) {
            themeToastActive = true;
        } else {
            gThemeToastUntilMs = 0;
            gPageDirty = true;
        }
    }

    // 버튼 이벤트가 있거나 화면 데이터가 바뀐 경우에만 렌더링합니다.
    if (buttonChanged || gPageDirty || themeToastActive || (gUiMode == UiMode::BrightnessAdjust) ||
        uiNeedsRender(gShown, next, gCurrentPage, kPageCount) || !next.linked) {
        uiRender(canvas, next, buildUiRenderContext());
        gShown = next;
        gPageDirty = false;
    }
}
