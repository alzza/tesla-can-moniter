#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <TFT_eSPI.h>
#include <esp_arduino_version.h>
#include <math.h>

struct __attribute__((packed)) struct_message {
    uint32_t uptime;
    float hz_a;
    float hz_b;
    bool nag_active;
    bool eap_active;
    uint8_t nag_mode;
    uint8_t twai_state;
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

struct UiState {
    uint32_t uptime = 0;
    float hzA = 0.0f;
    float hzB = 0.0f;
    bool nag = false;
    bool eap = false;
    uint8_t nagMode = 0;
    uint8_t twaiState = 0;
    uint32_t echoCount = 0;
    uint32_t txFailCount = 0;
    uint32_t aFramesTotal = 0;
    uint32_t aFrames1021 = 0;
    uint32_t aEapModified = 0;
    uint32_t bFramesTotal = 0;
    uint32_t bFrames880 = 0;
    uint32_t bFrames921 = 0;
    uint32_t bBusoffCount = 0;
    bool linked = false;
};

static TFT_eSPI tft;
static TFT_eSprite canvas = TFT_eSprite(&tft);

static portMUX_TYPE gDataMux = portMUX_INITIALIZER_UNLOCKED;
static struct_message gLatestMsg = {};
static bool gHasData = false;
static uint32_t gLastRxMs = 0;
static uint8_t gLastSenderMac[6] = {0};

static UiState gShown = {};

static constexpr uint8_t kWifiChannel = 1;
static constexpr uint32_t kRenderIntervalMs = 100;
static constexpr uint32_t kLinkTimeoutMs = 2000;
static constexpr uint32_t kNoSignalBlinkMs = 450;
static constexpr uint32_t kPageButtonMinIntervalMs = 140;
static constexpr uint8_t kButtonUpPin = 0;
static constexpr uint8_t kButtonDownPin = 14;
static constexpr uint8_t kNagModeDynamic = 0;
static constexpr uint8_t kNagModeFixed = 1;

static uint8_t gCurrentPage = 0;
static bool gPrevBtnUp = true;
static bool gPrevBtnDown = true;
static uint32_t gLastPageChangeMs = 0;
static bool gPageDirty = false;

static uint16_t colBg;
static uint16_t colPanel;
static uint16_t colText;
static uint16_t colMuted;
static uint16_t colOn;
static uint16_t colOff;
static uint16_t colHz;
static uint16_t colAccent;
static uint16_t colTrack;

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

static void drawHzTiles(TFT_eSprite& spr, float hzA, float hzB);

static int roundToInt(float v)
{
    return (int)lroundf(v);
}

static void drawBackdrop(TFT_eSprite& spr)
{
    spr.fillSprite(colBg);
    spr.drawFastVLine(2, 0, 170, colTrack);
    spr.drawFastVLine(317, 0, 170, colTrack);
}

static void drawStatusTiles(TFT_eSprite& spr, bool nagOn, bool eapOn)
{
    const int y = 8;
    const int h = 76;
    const int w = 148;
    const uint16_t eapColor = eapOn ? colOn : colOff;
    const uint16_t nagColor = nagOn ? colOn : colOff;

    // EAP tile (left-top)
    spr.fillRoundRect(8, y, w, h, 8, colPanel);
    spr.drawRoundRect(8, y, w, h, 8, colAccent);
    spr.drawRoundRect(9, y + 1, w - 2, h - 2, 8, colTrack);
    spr.fillCircle(22, y + 16, 5, eapColor);
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(colMuted, colPanel);
    spr.drawString("EAP", 34, y + 8);
    spr.setTextFont(4);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(eapColor, colPanel);
    spr.drawString(eapOn ? "ON" : "OFF", 82, y + 45);

    // NAG tile (right-top)
    spr.fillRoundRect(164, y, w, h, 8, colPanel);
    spr.drawRoundRect(164, y, w, h, 8, colAccent);
    spr.drawRoundRect(165, y + 1, w - 2, h - 2, 8, colTrack);
    spr.fillCircle(178, y + 16, 5, nagColor);
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(colMuted, colPanel);
    spr.drawString("NAG", 190, y + 8);
    spr.setTextFont(4);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(nagColor, colPanel);
    spr.drawString(nagOn ? "ON" : "OFF", 238, y + 45);
}

static void drawPageHeader(TFT_eSprite& spr, uint8_t page, const char* title)
{
    spr.fillRoundRect(8, 4, 304, 22, 6, colPanel);
    spr.drawRoundRect(8, 4, 304, 22, 6, colAccent);
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(colMuted, colPanel);
    spr.drawString("UP/DN: PAGE", 14, 9);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(colText, colPanel);
    spr.drawString(title, 160, 15);

    char pageBuf[16];
    snprintf(pageBuf, sizeof(pageBuf), "%u/3", (unsigned)(page + 1));
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(colHz, colPanel);
    spr.drawString(pageBuf, 304, 9);
}

static void drawMainPage(TFT_eSprite& spr, const UiState& state)
{
    drawStatusTiles(spr, state.nag, state.eap);
    drawHzTiles(spr, state.hzA, state.hzB);

    spr.fillRoundRect(164, 66, 148, 16, 5, colPanel);
    spr.setTextFont(2);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(colMuted, colPanel);
    spr.drawString(nagModeToText(state.nagMode), 238, 74);
}

static void drawRow(TFT_eSprite& spr, int y, const char* key, const char* value)
{
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(colMuted, colBg);
    spr.drawString(key, 12, y);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(colText, colBg);
    spr.drawString(value, 308, y);
    spr.drawFastHLine(10, y + 16, 300, colTrack);
}

static void drawAChannelPage(TFT_eSprite& spr, const UiState& state)
{
    char v[32];
    snprintf(v, sizeof(v), "%d Hz", roundToInt(state.hzA));
    drawRow(spr, 34, "A Frame Rate", v);

    snprintf(v, sizeof(v), "%lu", (unsigned long)state.aFramesTotal);
    drawRow(spr, 54, "A Frames Total", v);

    snprintf(v, sizeof(v), "%lu", (unsigned long)state.aFrames1021);
    drawRow(spr, 74, "A ID 1021", v);

    snprintf(v, sizeof(v), "%lu", (unsigned long)state.aEapModified);
    drawRow(spr, 94, "A EAP Modified", v);

    drawRow(spr, 114, "EAP Runtime", state.eap ? "ON" : "OFF");

    snprintf(v, sizeof(v), "%lu s", (unsigned long)state.uptime);
    drawRow(spr, 134, "Uptime", v);
}

static void drawBChannelPage(TFT_eSprite& spr, const UiState& state)
{
    char v[32];
    snprintf(v, sizeof(v), "%d Hz", roundToInt(state.hzB));
    drawRow(spr, 34, "B Frame Rate", v);

    snprintf(v, sizeof(v), "%lu", (unsigned long)state.bFramesTotal);
    drawRow(spr, 54, "B Frames Total", v);

    snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)state.bFrames880, (unsigned long)state.bFrames921);
    drawRow(spr, 74, "B ID 880/921", v);

    snprintf(v, sizeof(v), "%lu", (unsigned long)state.echoCount);
    drawRow(spr, 94, "B Echo Count", v);

    drawRow(spr, 114, "Nag Mode", nagModeToText(state.nagMode));

    snprintf(v, sizeof(v), "%s / %lu", twaiStateToText(state.twaiState), (unsigned long)state.bBusoffCount);
    drawRow(spr, 134, "TWAI/BusOff", v);
}

static void drawHzTile(TFT_eSprite& spr, int x, int y, const char* label, float hz)
{
    spr.fillRoundRect(x, y, 148, 76, 10, colPanel);
    spr.drawRoundRect(x, y, 148, 76, 10, colAccent);
    spr.drawRoundRect(x + 1, y + 1, 146, 74, 10, colTrack);

    char hzBuf[16];
    snprintf(hzBuf, sizeof(hzBuf), "%d", roundToInt(hz));

    spr.setTextFont(2);
    spr.setTextColor(colMuted, colPanel);
    spr.setTextDatum(TL_DATUM);
    spr.drawString(label, x + 10, y + 7);

    spr.setTextFont(7);
    spr.setTextColor(colHz, colPanel);
    spr.setTextDatum(MC_DATUM);
    spr.drawString(hzBuf, x + 74, y + 46);

    spr.setTextFont(2);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(colMuted, colPanel);
    spr.drawString("Hz", x + 138, y + 7);
}

static void drawHzTiles(TFT_eSprite& spr, float hzA, float hzB)
{
    drawHzTile(spr, 8, 86, "A CH", hzA);
    drawHzTile(spr, 164, 86, "B CH", hzB);
}

static void renderUi(const UiState& state)
{
    const uint32_t now = millis();
    const bool showNoSignal = ((now / kNoSignalBlinkMs) % 2) == 0;

    drawBackdrop(canvas);
    if (gCurrentPage == 0) {
        drawPageHeader(canvas, gCurrentPage, "MAIN");
        drawMainPage(canvas, state);
    } else if (gCurrentPage == 1) {
        drawPageHeader(canvas, gCurrentPage, "A CHANNEL DETAIL");
        drawAChannelPage(canvas, state);
    } else {
        drawPageHeader(canvas, gCurrentPage, "B CHANNEL DETAIL");
        drawBChannelPage(canvas, state);
    }

    if (!state.linked && showNoSignal) {
        canvas.setTextFont(4);
        canvas.setTextDatum(TC_DATUM);
        canvas.setTextColor(TFT_BLACK, colBg);
        canvas.drawString("NO SIGNAL", 161, 98);
        canvas.setTextColor(colOff, colBg);
        canvas.drawString("NO SIGNAL", 160, 97);
    }

    canvas.pushSprite(0, 0);
}

static bool needsRender(const UiState& a, const UiState& b)
{
    if (gCurrentPage > 2) return true;
    if (a.linked != b.linked) return true;
    if (a.nag != b.nag) return true;
    if (a.eap != b.eap) return true;
    if (a.nagMode != b.nagMode) return true;
    if (a.twaiState != b.twaiState) return true;
    if (a.echoCount != b.echoCount) return true;
    if (a.txFailCount != b.txFailCount) return true;
    if (a.aFramesTotal != b.aFramesTotal) return true;
    if (a.aFrames1021 != b.aFrames1021) return true;
    if (a.aEapModified != b.aEapModified) return true;
    if (a.bFramesTotal != b.bFramesTotal) return true;
    if (a.bFrames880 != b.bFrames880) return true;
    if (a.bFrames921 != b.bFrames921) return true;
    if (a.bBusoffCount != b.bBusoffCount) return true;
    if ((a.hzA - b.hzA > 0.05f) || (b.hzA - a.hzA > 0.05f)) return true;
    if ((a.hzB - b.hzB > 0.05f) || (b.hzB - a.hzB > 0.05f)) return true;
    return false;
}

static bool handlePageButtons()
{
    const uint32_t now = millis();
    const bool upNow = (digitalRead(kButtonUpPin) == HIGH);
    const bool downNow = (digitalRead(kButtonDownPin) == HIGH);

    // Keep one-step page changes per click to avoid jumpy transitions.
    if (now - gLastPageChangeMs < kPageButtonMinIntervalMs) {
        gPrevBtnUp = upNow;
        gPrevBtnDown = downNow;
        return false;
    }

    bool changed = false;

    if (gPrevBtnDown && !downNow) {
        gCurrentPage = (uint8_t)((gCurrentPage + 1) % 3);
        gPageDirty = true;
        changed = true;
    } else if (gPrevBtnUp && !upNow) {
        gCurrentPage = (uint8_t)((gCurrentPage + 2) % 3);
        gPageDirty = true;
        changed = true;
    }

    if (changed) gLastPageChangeMs = now;

    gPrevBtnUp = upNow;
    gPrevBtnDown = downNow;
    return changed;
}

static bool initEspNowReceiver()
{
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    delay(50);

    // Force fixed channel for reliable sender/receiver pairing.
    // Some boards apply channel changes more reliably with a brief promiscuous toggle.
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
            return;
        }

        static bool firstRxLogged = false;
        portENTER_CRITICAL_ISR(&gDataMux);
        gLatestMsg = parsed;
        gHasData = true;
        gLastRxMs = millis();
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
            return;
        }

        static bool firstRxLogged = false;
        portENTER_CRITICAL_ISR(&gDataMux);
        gLatestMsg = parsed;
        gHasData = true;
        gLastRxMs = millis();
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
}

void setup()
{
    Serial.begin(115200);
    delay(300);

    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);

    pinMode(kButtonUpPin, INPUT_PULLUP);
    pinMode(kButtonDownPin, INPUT_PULLUP);
    gPrevBtnUp = (digitalRead(kButtonUpPin) == HIGH);
    gPrevBtnDown = (digitalRead(kButtonDownPin) == HIGH);

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
    renderUi(boot);

    initEspNowReceiver();
}

void loop()
{
    static uint32_t lastRenderMs = 0;

    handlePageButtons();

    uint32_t now = millis();
    if (now - lastRenderMs < kRenderIntervalMs) {
        delay(5);
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

    UiState next;
    next.uptime = msg.uptime;
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
    next.linked = hasData && (now - lastRx <= kLinkTimeoutMs);

    if (!next.linked && now > kLinkTimeoutMs) {
        next.hzA = 0.0f;
        next.hzB = 0.0f;
    }

    if (gPageDirty || needsRender(gShown, next) || !next.linked) {
        renderUi(next);
        gShown = next;
        gPageDirty = false;
    }
}
