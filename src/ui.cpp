#include "ui.h"

#include <cstring>
#include <esp_system.h>

namespace {

static uint16_t levelColor(const UiRenderContext& ctx, uint8_t level)
{
    if (level >= 2) return ctx.colOff;
    if (level == 1) return ctx.colHz;
    return ctx.colOn;
}

static const char* onOff(bool value) { return value ? "ON" : "OFF"; }
static const char* yesNo(bool value) { return value ? "YES" : "NO"; }

static void drawBackdrop(TFT_eSprite& spr, const UiRenderContext& ctx)
{
    spr.fillSprite(ctx.colBg);
    spr.drawFastVLine(2, 0, 170, ctx.colTrack);
    spr.drawFastVLine(317, 0, 170, ctx.colTrack);
}

static void drawPageHeader(TFT_eSprite& spr, const UiRenderContext& ctx,
                           uint8_t page, const char* title)
{
    spr.fillRoundRect(8, 4, 304, 22, 6, ctx.colPanel);
    spr.drawRoundRect(8, 4, 304, 22, 6, ctx.colAccent);
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colPanel);
    spr.drawString("UP/DN", 14, 9);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(ctx.colText, ctx.colPanel);
    spr.drawString(title, 160, 15);
    char pageBuf[12];
    snprintf(pageBuf, sizeof(pageBuf), "%u/%u", (unsigned)(page + 1), (unsigned)ctx.pageCount);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(ctx.colHz, ctx.colPanel);
    spr.drawString(pageBuf, 304, 9);
}

static void drawRow(TFT_eSprite& spr, const UiRenderContext& ctx,
                    int y, const char* key, const char* value, uint16_t valueColor = 0)
{
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colBg);
    spr.drawString(key, 12, y);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(valueColor ? valueColor : ctx.colText, ctx.colBg);
    spr.drawString(value, 308, y);
    spr.drawFastHLine(10, y + 16, 300, ctx.colTrack);
}

static void drawHealthCard(TFT_eSprite& spr, const UiRenderContext& ctx,
                           int x, const char* channel, const char* state,
                           uint8_t level, float hz, uint32_t busoff)
{
    const uint16_t color = levelColor(ctx, level);
    spr.fillRoundRect(x, 32, 148, 65, 9, ctx.colPanel);
    spr.drawRoundRect(x, 32, 148, 65, 9, color);
    spr.fillCircle(x + 14, 46, 5, color);
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colPanel);
    spr.drawString(channel, x + 25, 39);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(color, ctx.colPanel);
    spr.drawString(state, x + 138, 39);
    char hzBuf[20];
    snprintf(hzBuf, sizeof(hzBuf), "%.0f Hz", (double)hz);
    spr.setTextFont(4);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(ctx.colText, ctx.colPanel);
    spr.drawString(hzBuf, x + 74, 69);
    char boBuf[18];
    snprintf(boBuf, sizeof(boBuf), "BO %lu", (unsigned long)busoff);
    spr.setTextFont(1);
    spr.setTextDatum(BR_DATUM);
    spr.setTextColor(busoff ? ctx.colOff : ctx.colMuted, ctx.colPanel);
    spr.drawString(boBuf, x + 139, 91);
}

static void drawFeatureChip(TFT_eSprite& spr, const UiRenderContext& ctx,
                            int x, int y, const char* label, const char* value, bool active)
{
    const uint16_t color = active ? ctx.colOn : ctx.colMuted;
    spr.fillRoundRect(x, y, 148, 25, 7, ctx.colPanel);
    spr.drawRoundRect(x, y, 148, 25, 7, active ? ctx.colTrack : ctx.colAccent);
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colPanel);
    spr.drawString(label, x + 8, y + 5);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(color, ctx.colPanel);
    spr.drawString(value, x + 140, y + 5);
}

static void drawMainPage(TFT_eSprite& spr, const UiRenderContext& ctx, const UiState& state)
{
    drawHealthCard(spr, ctx, 8, "A MCP2515", state.aHealthState,
                   state.aHealthLevel, state.hzA, state.aBusoffCount);
    drawHealthCard(spr, ctx, 164, "B TWAI", state.bHealthState,
                   state.bHealthLevel, state.hzB, state.bBusoffCount);
    drawFeatureChip(spr, ctx, 8, 103, "ECE R79", onOff(state.eceR79), state.eceR79);
    drawFeatureChip(spr, ctx, 164, 103, "SUMMON", onOff(state.summon), state.summon);
    char nag[16];
    snprintf(nag, sizeof(nag), "%s M%u", onOff(state.nag), (unsigned)state.nagMode);
    drawFeatureChip(spr, ctx, 8, 133, "NAG", nag, state.nag);
    char mark[16];
    snprintf(mark, sizeof(mark), "%s %lu", state.userMarkActive ? "RUN" : "IDLE",
             (unsigned long)state.userMarkCount);
    drawFeatureChip(spr, ctx, 164, 133, "USER MARK", mark, state.userMarkActive);
}

static void drawAChannelPage(TFT_eSprite& spr, const UiRenderContext& ctx, const UiState& s)
{
    char v[48];
    snprintf(v, sizeof(v), "%s / L%u", s.aHealthState, (unsigned)s.aHealthLevel);
    drawRow(spr, ctx, 34, "Health", v, levelColor(ctx, s.aHealthLevel));
    snprintf(v, sizeof(v), "%.1fHz / %lums", (double)s.hzA, (unsigned long)s.aFrameAgeMs);
    drawRow(spr, ctx, 54, "Rate / Age", v);
    snprintf(v, sizeof(v), "0x%02lX", (unsigned long)s.aEflg);
    drawRow(spr, ctx, 74, "EFLG", v, s.aEflg ? ctx.colHz : ctx.colOn);
    snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)s.aTec, (unsigned long)s.aRec);
    drawRow(spr, ctx, 94, "TEC / REC", v);
    snprintf(v, sizeof(v), "%lu / BO %lu", (unsigned long)s.aRxOverrun,
             (unsigned long)s.aBusoffCount);
    drawRow(spr, ctx, 114, "RX OVR / BUS-OFF", v,
            (s.aRxOverrun || s.aBusoffCount) ? ctx.colHz : ctx.colOn);
    snprintf(v, sizeof(v), "%lu / %lu / %lu%s", (unsigned long)s.aTxQueued,
             (unsigned long)s.aTxBusy, (unsigned long)s.aTxHard,
             s.aTxGuard ? " G" : "");
    drawRow(spr, ctx, 134, "TX Q / Busy / Hard", v, s.aTxGuard ? ctx.colHz : ctx.colText);
}

static void drawBChannelPage(TFT_eSprite& spr, const UiRenderContext& ctx, const UiState& s)
{
    char v[48];
    snprintf(v, sizeof(v), "%s / L%u", s.bHealthState, (unsigned)s.bHealthLevel);
    drawRow(spr, ctx, 34, "Health", v, levelColor(ctx, s.bHealthLevel));
    snprintf(v, sizeof(v), "%.1fHz / %lums", (double)s.hzB, (unsigned long)s.bFrameAgeMs);
    drawRow(spr, ctx, 54, "Rate / Age", v);
    snprintf(v, sizeof(v), "%lu / BO %lu", (unsigned long)s.bTwaiState,
             (unsigned long)s.bBusoffCount);
    drawRow(spr, ctx, 74, "TWAI / BUS-OFF", v, s.bTwaiState == 2 ? ctx.colOff : ctx.colText);
    snprintf(v, sizeof(v), "%lu / %lu", (unsigned long)s.bTec, (unsigned long)s.bRec);
    drawRow(spr, ctx, 94, "TEC / REC", v);
    snprintf(v, sizeof(v), "%lums / Echo %lu", (unsigned long)s.bRecoveryQuietMs,
             (unsigned long)s.bEchoCount);
    drawRow(spr, ctx, 114, "Recovery Quiet", v, s.bRecoveryQuietMs ? ctx.colHz : ctx.colText);
    snprintf(v, sizeof(v), "%lu / %lu / %lu", (unsigned long)s.bArbLost,
             (unsigned long)s.bBusError, (unsigned long)s.bTxFailed);
    drawRow(spr, ctx, 134, "ARB / Err / Fail", v,
            (s.bBusError || s.bTxFailed) ? ctx.colHz : ctx.colText);
}

static void drawFeaturePage(TFT_eSprite& spr, const UiRenderContext& ctx, const UiState& s)
{
    char v[56];
    snprintf(v, sizeof(v), "R79 %s / SUM %s / TS %s", onOff(s.eceR79),
             onOff(s.summon), onOff(s.tsllc));
    drawRow(spr, ctx, 34, "A Features", v);
    snprintf(v, sizeof(v), "%s M%u / AP-ONLY %s", onOff(s.nag),
             (unsigned)s.nagMode, yesNo(s.nagApOnly));
    drawRow(spr, ctx, 54, "Nag", v, s.nagReady ? ctx.colOn : ctx.colHz);
    snprintf(v, sizeof(v), "%s S%u / %lums", onOff(s.apActive),
             (unsigned)s.apState, (unsigned long)s.apStableMs);
    drawRow(spr, ctx, 74, "Autopilot", v);
    snprintf(v, sizeof(v), "P %s / SUM %s", yesNo(s.parked), yesNo(s.summoning));
    drawRow(spr, ctx, 94, "Vehicle Gate", v);
    snprintf(v, sizeof(v), "%s / %s", s.gateOpen ? "OPEN" : "CLOSED", s.gateReason);
    drawRow(spr, ctx, 114, "Gate", v, s.gateOpen ? ctx.colOn : ctx.colHz);
    snprintf(v, sizeof(v), "%lu / %lu / %lu", (unsigned long)s.summonTxOk,
             (unsigned long)s.summonTxFail, (unsigned long)s.summonBlocked);
    drawRow(spr, ctx, 134, "SUM TX O/F/Block", v, s.summonTxFail ? ctx.colOff : ctx.colText);
}

static void drawSystemPage(TFT_eSprite& spr, const UiRenderContext& ctx, const UiState& s)
{
    // 상태 5행 + 편집 3행. 간격 17px로 170px 안에 맞춤.
    auto drawSystemRow = [&](int y, const char* key, const char* value, bool hl,
                             uint16_t valueColor = 0) {
        if (hl) {
            spr.fillRoundRect(10, y - 1, 300, 17, 3, ctx.colPanel);
            spr.drawRoundRect(10, y - 1, 300, 17, 3, ctx.colAccent);
        }
        spr.setTextFont(2);
        spr.setTextDatum(TL_DATUM);
        spr.setTextColor(hl ? ctx.colHz : ctx.colMuted, hl ? ctx.colPanel : ctx.colBg);
        spr.drawString(key, 12, y);
        spr.setTextDatum(TR_DATUM);
        const uint16_t bg = hl ? ctx.colPanel : ctx.colBg;
        spr.setTextColor(valueColor ? valueColor : ctx.colText, bg);
        spr.drawString(value, 308, y);
        spr.drawFastHLine(10, y + 15, 300, ctx.colTrack);
    };

    char v[56];
    snprintf(v, sizeof(v), "%s / OTA %lu", s.firmware, (unsigned long)s.otaState);
    drawSystemRow(32, "T2 Firmware", v, false);
    drawSystemRow(49, "Build", s.build, false);
    snprintf(v, sizeof(v), "%lddBm / age %lums", (long)s.wifiRssi,
             (unsigned long)s.responseAgeMs);
    drawSystemRow(66, "WiFi / Data", v, false, s.linked ? ctx.colOn : ctx.colOff);
    snprintf(v, sizeof(v), "%lu / %lu / %lu", (unsigned long)s.requestOk,
             (unsigned long)s.requestFail, (unsigned long)s.parseFail);
    drawSystemRow(83, "HTTP O/F/Parse", v, false, s.parseFail ? ctx.colHz : ctx.colText);
    formatHms(v, sizeof(v), s.t2Uptime);
    drawSystemRow(100, "T2 Uptime", v, false);

    // 편집 가능 항목: CPU / Brightness / Theme
    snprintf(v, sizeof(v), "%u MHz", (unsigned)getCpuFrequencyMhz());
    drawSystemRow(117, "CPU Profile", v, ctx.systemEditMode && ctx.systemSelected == 0);
    snprintf(v, sizeof(v), "%u%% (%u/16)", (unsigned)ctx.brightnessPercent,
             (unsigned)ctx.backlightLevel);
    drawSystemRow(134, "Brightness", v, ctx.systemEditMode && ctx.systemSelected == 1);
    drawSystemRow(151, "Theme",
                  (ctx.theme == UiTheme::Light) ? "LIGHT" : "DARK",
                  ctx.systemEditMode && ctx.systemSelected == 2);

    // E안: SYSTEM 하단 조작 힌트 (font1로 한 줄)
    spr.setTextFont(1);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(ctx.colMuted, ctx.colBg);
    if (ctx.systemEditMode) {
        spr.drawString("EDIT: UP/DN move  UP-hold exec  DN-hold exit", 10, 161);
    } else {
        spr.drawString("DN3s:THEME  DN2s:EDIT  Theme row: UP-hold toggle", 10, 161);
    }
}

static void drawBrightnessPage(TFT_eSprite& spr, const UiRenderContext& ctx)
{
    drawPageHeader(spr, ctx, ctx.savedPageBeforeBrightness, "BRIGHTNESS");
    spr.fillRoundRect(24, 42, 272, 84, 10, ctx.colPanel);
    spr.drawRoundRect(24, 42, 272, 84, 10, ctx.colAccent);
    const int barX = 40, barY = 76, barW = 240, barH = 22;
    const int fillW = (barW * ctx.brightnessPercent) / 100;
    spr.fillRoundRect(barX, barY, barW, barH, 6, ctx.colBg);
    spr.drawRoundRect(barX, barY, barW, barH, 6, ctx.colTrack);
    if (fillW > 4) spr.fillRoundRect(barX + 2, barY + 2, fillW - 4, barH - 4, 4, ctx.colHz);
    char v[16];
    snprintf(v, sizeof(v), "%u%%", (unsigned)ctx.brightnessPercent);
    spr.setTextFont(4);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(ctx.colText, ctx.colPanel);
    spr.drawString(v, 160, 57);
    spr.setTextFont(2);
    spr.setTextColor(ctx.colMuted, ctx.colBg);
    spr.drawString("UP:+10  DOWN:-10", 160, 138);
}

} // namespace

void formatHms(char* out, size_t outLen, uint32_t totalSeconds)
{
    const uint32_t hh = totalSeconds / 3600U;
    const uint32_t mm = (totalSeconds % 3600U) / 60U;
    const uint32_t ss = totalSeconds % 60U;
    snprintf(out, outLen, "%02lu:%02lu:%02lu", (unsigned long)hh,
             (unsigned long)mm, (unsigned long)ss);
}

void uiApplyThemePalette(UiRenderContext& ctx, UiTheme theme)
{
    auto rgb565 = [](uint8_t r, uint8_t g, uint8_t b) -> uint16_t {
        return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    };

    ctx.theme = theme;
    if (theme == UiTheme::Light) {
        ctx.colBg = rgb565(242, 245, 248);
        ctx.colPanel = rgb565(255, 255, 255);
        ctx.colText = rgb565(22, 28, 36);
        ctx.colMuted = rgb565(95, 110, 128);
        ctx.colOn = rgb565(18, 150, 88);
        ctx.colOff = rgb565(210, 55, 55);
        ctx.colHz = rgb565(185, 130, 10);
        ctx.colAccent = rgb565(170, 190, 210);
        ctx.colTrack = rgb565(40, 120, 175);
        return;
    }

    ctx.colBg = rgb565(8, 11, 15);
    ctx.colPanel = rgb565(20, 28, 38);
    ctx.colText = rgb565(232, 238, 245);
    ctx.colMuted = rgb565(145, 160, 178);
    ctx.colOn = rgb565(40, 220, 120);
    ctx.colOff = rgb565(245, 74, 74);
    ctx.colHz = rgb565(250, 208, 58);
    ctx.colAccent = rgb565(45, 70, 94);
    ctx.colTrack = rgb565(30, 125, 188);
}

void uiRender(TFT_eSprite& canvas, const UiState& state, const UiRenderContext& ctx)
{
    drawBackdrop(canvas, ctx);
    if (ctx.brightnessAdjustMode) {
        drawBrightnessPage(canvas, ctx);
    } else {
        static const char* titles[] = {"OVERVIEW", "A CHANNEL", "B CHANNEL", "FEATURES", "SYSTEM"};
        drawPageHeader(canvas, ctx, ctx.currentPage, titles[ctx.currentPage < 5 ? ctx.currentPage : 0]);
        if (ctx.currentPage == 0) drawMainPage(canvas, ctx, state);
        else if (ctx.currentPage == 1) drawAChannelPage(canvas, ctx, state);
        else if (ctx.currentPage == 2) drawBChannelPage(canvas, ctx, state);
        else if (ctx.currentPage == 3) drawFeaturePage(canvas, ctx, state);
        else drawSystemPage(canvas, ctx, state);
    }

    if (!ctx.brightnessAdjustMode && !state.linked) {
        const bool show = ((millis() / ctx.noSignalBlinkMs) % 2U) == 0U;
        if (show) {
            canvas.fillRoundRect(72, 70, 176, 34, 8, ctx.colPanel);
            canvas.drawRoundRect(72, 70, 176, 34, 8, ctx.colOff);
            canvas.setTextFont(4);
            canvas.setTextDatum(MC_DATUM);
            canvas.setTextColor(ctx.colOff, ctx.colPanel);
            canvas.drawString("NO SIGNAL", 160, 87);
        }
    }

    // A안: 테마 토글 직후 짧은 확인 토스트
    if (ctx.themeToastUntilMs != 0 && millis() < ctx.themeToastUntilMs) {
        canvas.fillRoundRect(60, 68, 200, 34, 8, ctx.colPanel);
        canvas.drawRoundRect(60, 68, 200, 34, 8, ctx.colHz);
        canvas.setTextFont(4);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextColor(ctx.colHz, ctx.colPanel);
        canvas.drawString((ctx.theme == UiTheme::Light) ? "THEME LIGHT" : "THEME DARK", 160, 85);
    }

    canvas.pushSprite(0, 0);
}

bool uiNeedsRender(const UiState& a, const UiState& b, uint8_t currentPage, uint8_t pageCount)
{
    if (currentPage >= pageCount) return true;
    return std::memcmp(&a, &b, sizeof(UiState)) != 0;
}
