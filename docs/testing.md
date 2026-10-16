# 빌드와 테스트

[사용 안내](../README.md) · [수정 일지](history/README.md)

## 검증 범위

ESP-IDF v6.1 / ESP32-S3 빌드와 ASan/UBSan 호스트 테스트를 사용합니다.
아래 명령은 저장소 최상위에서 실행합니다. 호스트 테스트는 드라이버 대역과 가상 시계를
사용하므로 실제 무선 연결, USB 전원, OS 재접속 정책까지 검증하지는 않습니다.
각 변경 당시의 실행 결과는 수정 일지에 기록합니다.

사용자 로그에서는 양쪽 7.5ms 연결과, Ubuntu 키 실패 후 Windows 키로 재접속·암호화 성공이
확인됐습니다. 양방향 OS 전환의 반복 안정성, 장시간 사용 및 절전 복귀는 별도 실기기 확인이 필요합니다.

```sh
idf.py -C test build
```

## 호스트 테스트

저장소 최상위에서 파서·전환 로직 테스트:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I test/main test/main/hid_input.c test/main/input_router.c test/main/status_pattern.c \
  test/main/mouse_pending.c test/main/keyboard_pending.c test/tests/test_input.c \
  -o /tmp/test-esp-hid
ASAN_OPTIONS=detect_leaks=0 /tmp/test-esp-hid
```

테스트는 사용자 로그의 05AC:024F 및 046D:C548 키보드 디스크립터, 합성 마우스
디스크립터, 짧거나 잘못된 데이터, 잘못된 디스크립터 10,000개, 키 해제·동시 입력,
전환 버튼 유지·재누름, 비연결 대상 전환 거부, 등록 단축키의 반복·해제·충돌 처리,
source 분리, USB 장치 없음/연결 대기/선택 PC/등록 모드의 LED 색과 점멸 주기를 확인합니다.
초기화 단축키는 3초 유지, 한 번만 실행, 키 해제·USB 분리 시 취소,
등록 단축키와의 충돌 및 일반 Backspace 전달을 확인합니다.
마우스 재시도 테스트는 전송 전 데이터 유지, 이동량 합산·분할, 버튼 순서,
큐 포화·순환 및 초기화를 확인합니다. 키보드 재시도 테스트는 전송 실패 중
누름·해제·재누름 순서, 중복 상태, 큐 포화·순환 및 초기화를 확인합니다.

USB 복구 테스트는 실제 펌웨어의 USB 처리 코드를 드라이버 대역으로 실행해
초기화 실패 후 재시도, 수신 오류 후 stop/start, 초기화 중 분리,
소스 재사용 및 입력 장치가 없을 때만 루트 포트를 복구하는 동작을 확인합니다.
실제 USB 전송이나 BLE 무선 동작을 재현하는 테스트는 아닙니다.

```sh
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
  -ffunction-sections -fdata-sections -fsanitize=address,undefined -g \
  -I test/tests/stubs -I test/main \
  test/tests/test_usb_recovery.c test/main/hid_input.c \
  -Wl,--gc-sections -o /tmp/test-usb-recovery
ASAN_OPTIONS=detect_leaks=0 /tmp/test-usb-recovery
```

BLE 절전 테스트는 실제 `ble_switch.c`의 이벤트 처리·라우팅·유지 관리 코드를
가상 시계와 실패를 주입할 수 있는 BLE 드라이버 대역으로 실행합니다. 실제 PC 절전과
무선 연결 자체를 검증하는 테스트는 아닙니다.

```sh
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
  -ffunction-sections -fdata-sections -fsanitize=address,undefined -g \
  -I test/tests/stubs -I test/main \
  test/tests/test_ble_sleep.c test/main/input_router.c \
  test/main/keyboard_pending.c test/main/mouse_pending.c \
  -Wl,--gc-sections -o /tmp/test-ble-sleep
ASAN_OPTIONS=detect_leaks=0 /tmp/test-ble-sleep
```

원본 `test/pytest_hello_world.py`는 이 펌웨어용 테스트가 아닙니다.

USB HID 드라이버의 실제 완료 콜백에 수신 재예약 실패를 주입하는 테스트:

```sh
python3 test/tests/test_hid_rearm.py
```

비동기 USB 상태 감시와 자동 복구 판단 테스트:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I test/tests/stubs -I test/main test/tests/test_usb_health.c -o /tmp/test-usb-health
ASAN_OPTIONS=detect_leaks=0 /tmp/test-usb-health
```

OS 프로필 저장소 테스트:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I test/tests/stubs -I test/main test/tests/test_os_profile.c \
  -Wl,--wrap=nvs_open -o /tmp/test-os-profile
ASAN_OPTIONS=detect_leaks=0 /tmp/test-os-profile
```

키보드 LED Output 디스크립터 테스트:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I test/main test/tests/test_hid_led.c test/main/hid_input.c -o /tmp/test-hid-led
ASAN_OPTIONS=detect_leaks=0 /tmp/test-hid-led
```

B OS별 키 저장 및 순차 인증 테스트:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I test/tests/stubs -I test/main test/tests/test_b_os_bonds.c -o /tmp/test-b-os-bonds
ASAN_OPTIONS=detect_leaks=0 /tmp/test-b-os-bonds
```

등록 시 조기 LTK 요청의 보관·취소·재생 테스트:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I test/tests/stubs -I test/main test/tests/test_b_auth_gate.c -o /tmp/test-b-auth-gate
ASAN_OPTIONS=detect_leaks=0 /tmp/test-b-auth-gate
```
