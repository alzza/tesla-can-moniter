# Changelog

All notable changes to this project are documented in this file.

This project follows a Keep a Changelog style.

## [1.0.0] - 2026-04-22

### Added
- ESP-NOW 수신 기반 Tesla CAN 모니터 기본 구조
- 3페이지 순환 UI (MAIN / A CHANNEL DETAIL / B CHANNEL DETAIL)
- 버튼 페이지 전환 입력 처리 (GPIO0, GPIO14)
- NO SIGNAL 링크 상태 표시
- Nag 모드 표시 (DYNAMIC/FIXED)
- TWAI 상태 및 BusOff 표시
- 최신 52바이트 페이로드 파싱
- 레거시 22바이트 페이로드 하위 호환 파싱
- T-Display-S3 전용 프로젝트 프롬프트 문서

### Changed
- 페이지 전환 안정화: 클릭 이벤트 유실 방지 로직 적용
- 페이지 배경 대각선 장식 제거로 가독성 개선

### Build
- PlatformIO 환경 lilygo-t-display-s3 기준 빌드/업로드 설정 정리
