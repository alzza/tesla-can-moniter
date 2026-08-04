#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// UiState는 화면에 그릴 "데이터 스냅샷"입니다.
// main.cpp가 최신 T2-CAN 모니터 API 응답으로 갱신하고,
// ui.cpp는 이 불변 스냅샷 1개를 기준으로 한 프레임을 그립니다.
struct UiState {
    uint32_t localUptime = 0;
    uint32_t t2Uptime = 0;
    char firmware[16] = "--";
    char build[32] = "--";
    uint32_t otaState = 0;
    int32_t wifiRssi = -127;
    uint32_t responseAgeMs = 0;
    uint32_t requestOk = 0;
    uint32_t requestFail = 0;
    uint32_t parseFail = 0;
    bool schemaOk = false;
    bool linked = false;

    float hzA = 0.0f;
    float hzB = 0.0f;
    uint8_t aHealthLevel = 2;
    char aHealthState[20] = "INIT";
    uint32_t aFrameAgeMs = 0;
    uint32_t aBusoffCount = 0;
    uint32_t aEflg = 0;
    uint32_t aTec = 0;
    uint32_t aRec = 0;
    uint32_t aRxOverrun = 0;
    uint32_t aTxQueued = 0;
    uint32_t aTxBusy = 0;
    uint32_t aTxHard = 0;
    bool aTxGuard = false;

    uint8_t bHealthLevel = 2;
    char bHealthState[20] = "INIT";
    uint32_t bFrameAgeMs = 0;
    uint32_t bBusoffCount = 0;
    uint32_t bTwaiState = 0;
    uint32_t bTec = 0;
    uint32_t bRec = 0;
    uint32_t bRecoveryQuietMs = 0;
    uint32_t bArbLost = 0;
    uint32_t bBusError = 0;
    uint32_t bTxFailed = 0;
    uint32_t bEchoCount = 0;

    bool eceR79 = false;
    bool summon = false;
    bool tsllc = false;
    bool nag = false;
    uint8_t nagMode = 0;
    bool nagApOnly = false;
    bool nagReady = false;

    bool gateOpen = false;
    char gateReason[20] = "UNKNOWN";
    bool apActive = false;
    uint8_t apState = 0;
    uint32_t apStableMs = 0;
    bool parked = false;
    bool summoning = false;
    uint32_t summonTxOk = 0;
    uint32_t summonTxFail = 0;
    uint32_t summonBlocked = 0;
    bool userMarkActive = false;
    uint32_t userMarkCount = 0;
};

// 화면 테마. 레이아웃은 유지하고 색상 팔레트만 교체합니다.
enum class UiTheme : uint8_t {
    Dark = 0,
    Light = 1,
};

// UiRenderContext는 "현재 프레임의 화면 설정 묶음"입니다.
// 페이지/편집 모드/색상 팔레트처럼 렌더링 규칙만 담습니다.
struct UiRenderContext {
    uint8_t currentPage = 0;
    uint8_t pageCount = 5;
    uint8_t brightnessPercent = 80;
    uint8_t backlightLevel = 1;
    uint8_t systemSelected = 0;
    uint8_t savedPageBeforeBrightness = 0;
    bool brightnessAdjustMode = false;
    bool systemEditMode = false;
    bool wifiRuntimeEnabled = false;
    bool bluetoothRuntimeEnabled = false;
    UiTheme theme = UiTheme::Dark;
    // 테마 토글 직후 토스트 표시 종료 시각(millis). 0이면 비표시.
    uint32_t themeToastUntilMs = 0;
    uint32_t noSignalBlinkMs = 450;

    uint16_t colBg = 0;
    uint16_t colPanel = 0;
    uint16_t colText = 0;
    uint16_t colMuted = 0;
    uint16_t colOn = 0;
    uint16_t colOff = 0;
    uint16_t colHz = 0;
    uint16_t colAccent = 0;
    uint16_t colTrack = 0;
};

// theme에 맞는 RGB565 팔레트를 context 색상 필드에 채웁니다.
void uiApplyThemePalette(UiRenderContext& ctx, UiTheme theme);

// 페이지 공통 유틸: 초 단위를 HH:MM:SS 문자열로 변환합니다.
void formatHms(char* out, size_t outLen, uint32_t totalSeconds);

// 캔버스에 한 프레임 전체를 그리고 TFT로 전송합니다.
// 호출자는 state/context를 완성된 스냅샷으로 넘겨야 합니다.
void uiRender(TFT_eSprite& canvas, const UiState& state, const UiRenderContext& ctx);

// 불필요한 재렌더를 줄이기 위한 경량 변경 감지 함수입니다.
// 화면에 보이는 값이 의미 있게 바뀌면 true를 반환합니다.
bool uiNeedsRender(const UiState& a, const UiState& b, uint8_t currentPage, uint8_t pageCount);
