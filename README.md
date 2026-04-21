# Tesla CAN Monitor for T-Display-S3

ESP32-S3 LILYGO T-Display-S3 기반 Tesla CAN 상태 모니터 프로젝트입니다.
송신기(차량측)에서 ESP-NOW로 전달한 상태를 수신해 3개 페이지 UI로 표시합니다.

## 주요 기능

- ESP-NOW 텔레메트리 수신 (채널 고정: 1)
- 52바이트 신규 페이로드 수신
- 22바이트 레거시 페이로드 하위 호환 수신
- 3페이지 순환 UI
- 버튼 입력으로 페이지 전환 (GPIO0, GPIO14)
- 링크 끊김 시 NO SIGNAL 표시

## 하드웨어 기준

- 보드: LILYGO T-Display-S3
- 디스플레이: ST7789 170x320 (TFT_eSPI Setup206)
- 전원 핀: GPIO15 HIGH 필요
- 버튼: GPIO0(UP), GPIO14(DOWN)

## 페이지 구성

### 1) MAIN
- EAP 상태
- NAG 상태
- A 채널 Hz
- B 채널 Hz
- Nag 모드 (DYNAMIC/FIXED)

### 2) A CHANNEL DETAIL
- A Frame Rate
- A Frames Total
- A ID 1021
- A EAP Modified
- EAP Runtime
- Uptime

### 3) B CHANNEL DETAIL
- B Frame Rate
- B Frames Total
- B ID 880/921
- B Echo Count
- Nag Mode
- TWAI 상태 / BusOff

## 빌드

### 요구사항
- PlatformIO
- USB 연결된 T-Display-S3

### 빌드 명령

```bash
pio run -e lilygo-t-display-s3
```

### 업로드 명령

```bash
pio run -e lilygo-t-display-s3 -t upload
```

### 시리얼 모니터

```bash
pio device monitor -b 115200
```

## 수신 페이로드 버전

### 최신(52 bytes)
- uptime
- hz_a, hz_b
- nag_active, eap_active
- nag_mode, twai_state
- echo_count, tx_fail_count
- a_frames_total, a_frames_1021, a_eap_modified
- b_frames_total, b_frames_880, b_frames_921, b_busoff_count

### 레거시(22 bytes)
- uptime
- hz_a, hz_b
- nag_active, eap_active
- echo_count, tx_fail_count

레거시 수신 시 모드/상세 카운터는 기본값으로 표시됩니다.

## 릴리즈

변경 이력은 CHANGELOG.md를 참고하세요.
