#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// UiState는 화면에 그릴 "데이터 스냅샷"입니다.
// main.cpp가 최신 ESP-NOW 패킷(또는 대체값)으로 갱신하고,
// ui.cpp는 이 불변 스냅샷 1개를 기준으로 한 프레임을 그립니다.
struct UiState {
    // 장치 업타임(초). HH:MM:SS 표시용입니다.
    uint32_t uptime = 0;
    // 메인 페이지의 큰 Hz 타일에 표시할 A/B 채널 주기 정보입니다.
    float hzA = 0.0f;
    float hzB = 0.0f;
    // 송신기에서 전달되는 기능 상태 플래그입니다.
    bool nag = false;
    bool eap = false;
    // 송신기에서 전달되는 모드/상태 값입니다.
    uint8_t nagMode = 0;
    uint8_t twaiState = 0;
    // 상세 페이지에 표시할 진단 카운터들입니다.
    uint32_t echoCount = 0;
    uint32_t txFailCount = 0;
    uint32_t aFramesTotal = 0;
    uint32_t aFrames1021 = 0;
    uint32_t aEapModified = 0;
    uint32_t bFramesTotal = 0;
    uint32_t bFrames880 = 0;
    uint32_t bFrames921 = 0;
    uint32_t bBusoffCount = 0;
    float torqueNm = 0.0f;
    float stealthTorqueNm = 0.0f;
    // 수신 링크 연결 상태(main loop에서 계산).
    bool linked = false;
};

// UiRenderContext는 "현재 프레임의 화면 설정 묶음"입니다.
// 페이지/편집 모드/색상 팔레트처럼 렌더링 규칙만 담습니다.
struct UiRenderContext {
    uint8_t currentPage = 0;
    uint8_t pageCount = 4;
    uint8_t brightnessPercent = 80;
    uint8_t backlightLevel = 1;
    uint8_t systemSelected = 0;
    uint8_t savedPageBeforeBrightness = 0;
    bool brightnessAdjustMode = false;
    bool systemEditMode = false;
    bool wifiRuntimeEnabled = false;
    bool bluetoothRuntimeEnabled = false;
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

// 페이지 공통 유틸: 초 단위를 HH:MM:SS 문자열로 변환합니다.
void formatHms(char* out, size_t outLen, uint32_t totalSeconds);

// 캔버스에 한 프레임 전체를 그리고 TFT로 전송합니다.
// 호출자는 state/context를 완성된 스냅샷으로 넘겨야 합니다.
void uiRender(TFT_eSprite& canvas, const UiState& state, const UiRenderContext& ctx);

// 불필요한 재렌더를 줄이기 위한 경량 변경 감지 함수입니다.
// 화면에 보이는 값이 의미 있게 바뀌면 true를 반환합니다.
bool uiNeedsRender(const UiState& a, const UiState& b, uint8_t currentPage, uint8_t pageCount);
