# T2-CAN 모니터 API 연동 규칙

- AP: `TeslaCAN`
- 비밀번호: `asdf1234`
- 서버: `http://192.168.4.1`
- 상태: `GET /api/monitor`
- 폴링: 1초
- 신호 손실: 마지막 정상 응답 후 3.2초
- 지원 스키마: `1`
- 제어·CAN 송신·POST 요청은 사용하지 않는다.

## 응답 그룹

- 루트: 스키마, T2 업타임, 펌웨어, 빌드, OTA 상태, AP 접속 수
- `a`: MCP2515 상태, Hz, 수신 경과, BUS-OFF, EFLG, TEC/REC, RX 오버런, TX 결과·Guard
- `b`: TWAI 상태, Hz, 수신 경과, BUS-OFF, TEC/REC, 복구 안정화 잔여시간, ARB·버스·TX 오류, echo
- `features`: ECE R79, Summon, TSLLC, Nag, Nag 모드·AP 전용·준비 상태
- `gate`: 허용 여부·사유, AP 상태·안정시간, Parked, Summoning, Summon TX 결과
- `user_mark`: 진행 여부와 완료 횟수

## 호환 정책

- T-Display-S3는 루트 `schema`가 `1`일 때만 응답을 정상 데이터로 반영한다.
- 새 선택 필드는 기존 필드를 삭제·변경하지 않고 추가한다.
- 기존 필드 의미나 타입을 바꿀 때는 스키마 번호를 올리고 양쪽 펌웨어를 함께 갱신한다.
- 알 수 없는 추가 필드는 수신기가 무시한다.
