#include "ui.h"

#include <esp_system.h>
#include <math.h>

namespace {

// 이 파일 내부에서만 사용하는 상수입니다.
static constexpr uint8_t kNagModeFixed = 1;
static const char* kNagStrategyText = "CTR+1 FIXED";

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

static const char* wifiStatusText(const UiRenderContext& ctx)
{
    return ctx.wifiRuntimeEnabled ? "ON" : "OFF(RUNTIME)";
}

static const char* btStatusText(const UiRenderContext& ctx)
{
    return ctx.bluetoothRuntimeEnabled ? "ON" : "OFF(RUNTIME)";
}

static int roundToInt(float v)
{
    return (int)lroundf(v);
}

static void formatTorqueNm(char* out, size_t outLen, float torqueNm)
{
    const float shownTorque = (fabsf(torqueNm) < 0.005f) ? 0.0f : torqueNm;
    snprintf(out, outLen, "%.2fNm", (double)shownTorque);
}

// 배경 레이어: 이전 프레임을 지우고 양쪽 고정 가이드 라인을 그립니다.
static void drawBackdrop(TFT_eSprite& spr, const UiRenderContext& ctx)
{
    spr.fillSprite(ctx.colBg);
    spr.drawFastVLine(2, 0, 170, ctx.colTrack);
    spr.drawFastVLine(317, 0, 170, ctx.colTrack);
}

static void drawStatusTiles(TFT_eSprite& spr, const UiRenderContext& ctx, bool nagOn, bool eapOn)
{
    const int y = 8;
    const int h = 76;
    const int w = 148;
    const uint16_t eapColor = eapOn ? ctx.colOn : ctx.colOff;
    const uint16_t nagColor = nagOn ? ctx.colOn : ctx.colOff;

    spr.fillRoundRect(8, y, w, h, 8, ctx.colPanel);
    spr.drawRoundRect(8, y, w, h, 8, ctx.colAccent);
    spr.drawRoundRect(9, y + 1, w - 2, h - 2, 8, ctx.colTrack);
    spr.fillCircle(22, y + 16, 5, eapColor);
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colPanel);
    spr.drawString("EAP", 34, y + 8);
    spr.setTextFont(4);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(eapColor, ctx.colPanel);
    spr.drawString(eapOn ? "ON" : "OFF", 82, y + 45);

    spr.fillRoundRect(164, y, w, h, 8, ctx.colPanel);
    spr.drawRoundRect(164, y, w, h, 8, ctx.colAccent);
    spr.drawRoundRect(165, y + 1, w - 2, h - 2, 8, ctx.colTrack);
    spr.fillCircle(178, y + 16, 5, nagColor);
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colPanel);
    spr.drawString("NAG", 190, y + 8);
    spr.setTextFont(4);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(nagColor, ctx.colPanel);
    spr.drawString(nagOn ? "ON" : "OFF", 238, y + 45);
}

static void drawPageHeader(TFT_eSprite& spr, const UiRenderContext& ctx, uint8_t page, const char* title)
{
    spr.fillRoundRect(8, 4, 304, 22, 6, ctx.colPanel);
    spr.drawRoundRect(8, 4, 304, 22, 6, ctx.colAccent);
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colPanel);
    spr.drawString("UP/DN: PAGE", 14, 9);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(ctx.colText, ctx.colPanel);
    spr.drawString(title, 160, 15);

    char pageBuf[16];
    snprintf(pageBuf, sizeof(pageBuf), "%u/%u", (unsigned)(page + 1), (unsigned)ctx.pageCount);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(ctx.colHz, ctx.colPanel);
    spr.drawString(pageBuf, 304, 9);
}

static void drawBrightnessPage(TFT_eSprite& spr, const UiRenderContext& ctx)
{
    drawPageHeader(spr, ctx, ctx.savedPageBeforeBrightness, "BRIGHTNESS");

    spr.fillRoundRect(24, 42, 272, 84, 10, ctx.colPanel);
    spr.drawRoundRect(24, 42, 272, 84, 10, ctx.colAccent);
    spr.drawRoundRect(25, 43, 270, 82, 10, ctx.colTrack);

    const int barX = 40;
    const int barY = 76;
    const int barW = 240;
    const int barH = 22;
    // 밝기(0~100%)에 따라 바의 채움 폭을 계산합니다.
    const int fillW = (int)((barW * ctx.brightnessPercent) / 100);

    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colPanel);
    spr.drawString("LOW", barX, 52);
    spr.setTextDatum(TR_DATUM);
    spr.drawString("HIGH", barX + barW, 52);

    spr.fillRoundRect(barX, barY, barW, barH, 6, ctx.colBg);
    spr.drawRoundRect(barX, barY, barW, barH, 6, ctx.colTrack);
    if (fillW > 0) {
        spr.fillRoundRect(barX + 2, barY + 2, fillW - 4 > 0 ? fillW - 4 : 1, barH - 4, 4, ctx.colHz);
    }

    for (int i = 1; i < 10; ++i) {
        const int tx = barX + (barW * i) / 10;
        spr.drawFastVLine(tx, barY + barH + 4, 6, ctx.colTrack);
    }

    spr.setTextFont(2);
    spr.setTextColor(ctx.colMuted, ctx.colBg);
    spr.setTextDatum(MC_DATUM);
    spr.drawString("UP:+10  DOWN:-10", 160, 138);
}

static void drawMainPage(TFT_eSprite& spr, const UiRenderContext& ctx, const UiState& state)
{
    drawStatusTiles(spr, ctx, state.nag, state.eap);

    // 메인 화면은 B채널 핵심값 2개(Frame Rate / Torque)만 빠르게 보이도록 구성합니다.
    auto drawMetricTile = [&](int x, int y, const char* label, const char* value, const char* unit, uint8_t valueFont, uint16_t valueColor) {
        spr.fillRoundRect(x, y, 148, 76, 10, ctx.colPanel);
        spr.drawRoundRect(x, y, 148, 76, 10, ctx.colAccent);
        spr.drawRoundRect(x + 1, y + 1, 146, 74, 10, ctx.colTrack);

        spr.setTextFont(2);
        spr.setTextColor(ctx.colMuted, ctx.colPanel);
        spr.setTextDatum(TL_DATUM);
        spr.drawString(label, x + 10, y + 7);

        spr.setTextFont(valueFont);
        spr.setTextColor(valueColor, ctx.colPanel);
        spr.setTextDatum(MC_DATUM);
        spr.drawString(value, x + 74, y + 46);

        if (unit && unit[0]) {
            spr.setTextFont(2);
            spr.setTextDatum(TR_DATUM);
            spr.setTextColor(ctx.colMuted, ctx.colPanel);
            spr.drawString(unit, x + 138, y + 7);
        }
    };

    char bHzBuf[16];
    snprintf(bHzBuf, sizeof(bHzBuf), "%d", roundToInt(state.hzB));
    drawMetricTile(8, 86, "B RATE", bHzBuf, "Hz", 7, ctx.colHz);

    char torqueBuf[20];
    formatTorqueNm(torqueBuf, sizeof(torqueBuf), state.torqueNm);
    drawMetricTile(164, 86, "TORQUE", torqueBuf, "", 4, ctx.colText);

    char stealthBuf[24];
    if (state.stealthTorqueNm > 0.0f) {
        formatTorqueNm(stealthBuf, sizeof(stealthBuf), state.stealthTorqueNm);
    } else {
        snprintf(stealthBuf, sizeof(stealthBuf), "--");
    }

    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colPanel);
    spr.drawString("STEALTH", 174, 141);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(state.stealthTorqueNm > 0.0f ? ctx.colHz : ctx.colMuted, ctx.colPanel);
    spr.drawString(stealthBuf, 302, 141);

    spr.fillRoundRect(164, 66, 148, 16, 5, ctx.colPanel);
    spr.setTextFont(2);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colPanel);
    spr.drawString(kNagStrategyText, 238, 74);
}

static void drawRow(TFT_eSprite& spr, const UiRenderContext& ctx, int y, const char* key, const char* value)
{
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colBg);
    spr.drawString(key, 12, y);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(ctx.colText, ctx.colBg);
    spr.drawString(value, 308, y);
    spr.drawFastHLine(10, y + 16, 300, ctx.colTrack);
}

static void drawAChannelPage(TFT_eSprite& spr, const UiRenderContext& ctx, const UiState& state)
{
    char v[32];
    snprintf(v, sizeof(v), "%d Hz", roundToInt(state.hzA));
    drawRow(spr, ctx, 34, "A Frame Rate", v);

    snprintf(v, sizeof(v), "%lu", (unsigned long)state.aFramesTotal);
    drawRow(spr, ctx, 54, "A Frames Total", v);

    snprintf(v, sizeof(v), "%lu", (unsigned long)state.aFrames1021);
    drawRow(spr, ctx, 74, "A ID 1021", v);

    snprintf(v, sizeof(v), "%lu", (unsigned long)state.aEapModified);
    drawRow(spr, ctx, 94, "A EAP Modified", v);

    drawRow(spr, ctx, 114, "EAP Runtime", state.eap ? "ON" : "OFF");

    formatHms(v, sizeof(v), state.uptime);
    drawRow(spr, ctx, 134, "Uptime", v);
}

static void drawBChannelPage(TFT_eSprite& spr, const UiRenderContext& ctx, const UiState& state)
{
    char v[32];
    snprintf(v, sizeof(v), "%d Hz", roundToInt(state.hzB));
    drawRow(spr, ctx, 34, "B Frame Rate", v);

    snprintf(v, sizeof(v), "%lu", (unsigned long)state.bFramesTotal);
    drawRow(spr, ctx, 54, "B Frames Total", v);

    snprintf(v, sizeof(v), "%lu", (unsigned long)state.bFrames880);
    drawRow(spr, ctx, 74, "B ID 880", v);

    snprintf(v, sizeof(v), "%lu", (unsigned long)state.echoCount);
    drawRow(spr, ctx, 94, "B Echo Count", v);

    drawRow(spr, ctx, 114, "Nag Path", kNagStrategyText);

    snprintf(v, sizeof(v), "%s / %lu", twaiStateToText(state.twaiState), (unsigned long)state.bBusoffCount);
    drawRow(spr, ctx, 134, "TWAI/BusOff", v);
}

static void drawSystemPage(TFT_eSprite& spr, const UiRenderContext& ctx, const UiState& state)
{
    auto drawSystemRow = [&](int y, const char* key, const char* value, bool hl) {
        if (hl) {
            spr.fillRoundRect(10, y - 1, 300, 18, 3, ctx.colPanel);
            spr.drawRoundRect(10, y - 1, 300, 18, 3, ctx.colAccent);
        }
        spr.setTextFont(2);
        spr.setTextDatum(TL_DATUM);
        spr.setTextColor(hl ? ctx.colHz : ctx.colMuted, hl ? ctx.colPanel : ctx.colBg);
        spr.drawString(key, 12, y);
        spr.setTextDatum(TR_DATUM);
        spr.setTextColor(ctx.colText, hl ? ctx.colPanel : ctx.colBg);
        spr.drawString(value, 308, y);
        spr.drawFastHLine(10, y + 16, 300, ctx.colTrack);
    };

    char v[32];
    snprintf(v, sizeof(v), "%u MHz", (unsigned)getCpuFrequencyMhz());
    drawSystemRow(34, "CPU Profile", v, ctx.systemEditMode && ctx.systemSelected == 0);

    drawSystemRow(54, "WiFi Runtime", wifiStatusText(ctx), ctx.systemEditMode && ctx.systemSelected == 1);
    drawSystemRow(74, "Bluetooth Rt", btStatusText(ctx), ctx.systemEditMode && ctx.systemSelected == 2);

    snprintf(v, sizeof(v), "%u%% (%u/16)", (unsigned)ctx.brightnessPercent, (unsigned)ctx.backlightLevel);
    drawSystemRow(94, "Brightness", v, ctx.systemEditMode && ctx.systemSelected == 3);

    snprintf(v, sizeof(v), "%lu KB", (unsigned long)(esp_get_free_heap_size() / 1024u));
    drawSystemRow(114, "Heap Free", v, false);

    formatHms(v, sizeof(v), state.uptime);
    drawSystemRow(134, "Uptime", v, false);

    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colBg);
    if (ctx.systemEditMode) {
        spr.drawString("EDIT: UP/DN move, UP-hold exec, DN-hold exit", 10, 154);
    } else {
        spr.drawString("HOLD DN 2s to enter edit", 10, 154);
    }
}

} // 익명 네임스페이스

void formatHms(char* out, size_t outLen, uint32_t totalSeconds)
{
    // 초 단위를 읽기 쉬운 HH:MM:SS 형식으로 변환합니다.
    const uint32_t hh = totalSeconds / 3600u;
    const uint32_t mm = (totalSeconds % 3600u) / 60u;
    const uint32_t ss = totalSeconds % 60u;
    snprintf(out, outLen, "%02lu:%02lu:%02lu", (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
}

void uiRender(TFT_eSprite& canvas, const UiState& state, const UiRenderContext& ctx)
{
    // NO SIGNAL 문구를 점멸시켜 경고 상태를 더 명확히 보여줍니다.
    const uint32_t now = millis();
    const bool showNoSignal = ((now / ctx.noSignalBlinkMs) % 2) == 0;

    drawBackdrop(canvas, ctx);
    if (ctx.brightnessAdjustMode) {
        drawBrightnessPage(canvas, ctx);
    } else if (ctx.currentPage == 0) {
        drawPageHeader(canvas, ctx, ctx.currentPage, "MAIN");
        drawMainPage(canvas, ctx, state);
    } else if (ctx.currentPage == 1) {
        drawPageHeader(canvas, ctx, ctx.currentPage, "A CHANNEL DETAIL");
        drawAChannelPage(canvas, ctx, state);
    } else if (ctx.currentPage == 2) {
        drawPageHeader(canvas, ctx, ctx.currentPage, "B CHANNEL DETAIL");
        drawBChannelPage(canvas, ctx, state);
    } else {
        drawPageHeader(canvas, ctx, ctx.currentPage, "SYSTEM STATUS");
        drawSystemPage(canvas, ctx, state);
    }

    // 일반 페이지에서 링크가 끊기면 NO SIGNAL 오버레이를 표시합니다.
    if (!ctx.brightnessAdjustMode && !state.linked && showNoSignal) {
        canvas.setTextFont(4);
        canvas.setTextDatum(TC_DATUM);
        canvas.setTextColor(TFT_BLACK, ctx.colBg);
        canvas.drawString("NO SIGNAL", 161, 98);
        canvas.setTextColor(ctx.colOff, ctx.colBg);
        canvas.drawString("NO SIGNAL", 160, 97);
    }

    canvas.pushSprite(0, 0);
}

bool uiNeedsRender(const UiState& a, const UiState& b, uint8_t currentPage, uint8_t pageCount)
{
    // 페이지 인덱스가 비정상이면 강제 렌더를 유도해 안전하게 복구합니다.
    if (currentPage >= pageCount) return true;
    if (a.uptime != b.uptime) return true;
    if (a.linked != b.linked) return true;
    if (a.nag != b.nag) return true;
    if (a.eap != b.eap) return true;
    if (a.nagMode != b.nagMode) return true;
    if (a.twaiState != b.twaiState) return true;
    if ((a.torqueNm - b.torqueNm > 0.01f) || (b.torqueNm - a.torqueNm > 0.01f)) return true;
    if ((a.stealthTorqueNm - b.stealthTorqueNm > 0.01f) || (b.stealthTorqueNm - a.stealthTorqueNm > 0.01f)) return true;
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
