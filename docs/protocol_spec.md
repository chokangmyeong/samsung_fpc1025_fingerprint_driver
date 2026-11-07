# 삼성 지문인식 센서 (`04e8:7301` / FPC1025) USB-SPI 통신 프로토콜 명세서

본 문서는 공식 윈도우 바이너리 드라이버(`fpc_wbdi.dll`)의 전체 어셈블리 역공학 및 실제 하드웨어 검증을 통해 도출한 `FpcG2Usb` 브릿지 및 `FPC1025`(`FPC1021` + `FPC2050` 컴패니언 드라이브 IC) 통신 프로토콜 명세서입니다.

---

## 1. 하드웨어 및 USB 인터페이스

- **USB VID / PID**: `0x04e8` / `0x7301`
- **센서 HWID (`0xFC`)**: `0x021B` (내부 모델 ID `0x03FD`, `swing = 1` -> **FPC1025**, 160x160 픽셀)
- **엔드포인트 (Endpoints)**:
  - `EP 0 (Control)`: `FpcG2Usb` USB-SPI 브릿지 제어 (GPIO 리셋, IRQ 핀 폴링, SPI 클럭 설정, SPI CS_N 트랜잭션 길이 지정)
  - `EP 1 OUT (0x01)`: Bulk Transfer (SPI MOSI 스트림 송신)
  - `EP 2 IN (0x82)`: Bulk Transfer (SPI MISO 스트림 수신)

---

## 2. `FpcG2Usb` 브릿지 Vendor Control Transfer 규격

모든 SPI Bulk OUT/IN 트랜잭션 전에 브릿지 MCU에 SPI 버스 설정(`0xC3`)과 CS_N 트랜잭션 총 바이트 길이(`0xCA`)를 반드시 지정해야 합니다.

| `bmRequestType` | `bRequest` | `wValue` | `wIndex` | `wLength` | 역할 및 디스어셈블리 주소 |
| :---: | :---: | :---: | :---: | :---: | :--- |
| `0x40` | `0xDB` | `0x0006` | `0` / `1` | `0` | 센서 하드웨어 RST_N GPIO 핀 제어 (`0x18000801c`) |
| `0xC0` | `0xDA` | `0x0007` | `0x0000` | `2` | 센서 IRQ GPIO 상태 읽기 (`buf[0]==0, buf[1]=irq_state`, `0x180008074`) |
| `0x40` | `0xC3` | `0x0000` | `0x0000` | `16` | SPI 버스 설정 (3 MHz = `0x002DC6C0`, 8-bit, `0x180008458`) |
| `0x40` | `0xCA` | `0x0003` | `len` | `0` | 단일 SPI CS_N 트랜잭션의 총 바이트 길이 지정 (`0x1800086fc`) |

> [!IMPORTANT]
> **단일 프레임(25,602 바이트) 연속 SPI 읽기**:
> `0xC4`(`read_image_data`) 호출 시 `0xCA`의 `wIndex`에 `0x6402`(25,602 = `[0xC4, 0x00]` + 25,600 픽셀)를 설정하고, 브릿지 내부 576바이트 FIFO가 가득 차지 않도록 Bulk OUT(25,602B)을 비동기로 제출하면서 Bulk IN(25,602B)을 동시에 수신해야 CS_N이 끊기지 않고 160x160 전체 프레임이 단일 SPI 트랜잭션으로 읽힙니다.

---

## 3. FPC1025 (`swing = 1`) 초기화 및 캘리브레이션 시퀀스

### 3.1 레지스터 초기화 (`0x180005ee4`)
- `{0x9C, 0x55, 0x40, 0x00, 0x2D, 0x24}` (`adcsetup`)
- `{0x6C, 0x29}` (`pxlrstdly`)
- `{0x90, 0x32}` (`fngrdrivedly`)
- `{0x68, 0x31, 0x31, 0x31, 0x31, 0x3D, 0x3D, 0x3D, 0x3D}` (`sampledly`)
- `{0x98, 0x03, 0xEB, 0x3F, 0xB8, 0x0A, 0x07, 0x11, 0x22, 0x6A, 0x5D}` (`adcweighttbl`)
- `{0x8C, 0x12}` (`fngrdriveconf`: 외부 FPC2050 드라이브 활성화; `0x04`는 내부 테스트 패턴 모드이므로 실제 지문 캡처 시 절대 사용 금지)
- `{0x5C, 0x0B}` (`imagesetup`)
- `{0xD8, 0x20}` (`fngrdetthresh`)
- `{0xDC, 0x00, 0x0A}` (`fngrdetcntr`)

### 3.2 AFD (Auto Finger Detection) 터치 감지 모드 (`0x180004074` / `0x180007324`)
- `0x20` (`FNGR_PRESENT_QUERY`) 실행 전 반드시 AFD 전용 픽셀/ADC 설정을 적용해야 오작동(유휴 상태 `0x0FFF`)이 발생하지 않습니다:
  - `{0xA8, 0x0F, 0x0A}` (`pxlctrl` index 3)
  - `{0xA0, 0x0A, 0x02}` (`adcshiftgain`)
- 이후 `0x20` 명령 실행 -> IRQ 대기 및 `0x1C` 클리어 -> `0xD4` (4바이트) 읽기:
  - `mask = ((d4[0] << 8) | d4[1]) & 0x0FFF` (12개 센서 영역 비트마스크)
  - 손가락 미접촉 시 `mask = 0x000` (`popcount = 0`), 손가락 접촉 시 `popcount >= 6`.

### 3.3 폐루프 AGC (Auto Gain/Shift Control, `0x180003a30`) 및 컬럼 보정 (`0x180006fe8`)
- 드라이버 초기화 시 `0x180003694` (`fpc_calibrate_adc`)를 통해 `shift_table[32]` 및 `gain_table[16]`을 측정합니다.
- 지문 터치 감지 직후 중앙 서브 윈도우(`{0x54, 73, 14, 64, 32}`, `0x64=0x0C`, `0x5C=0x0A`)를 캡처하여 최적의 `pxlctrl`(`0xA8`), `shift`/`gain`(`0xA0`)을 계산한 뒤 전체 프레임(`{0x54, 0x00, 0xA0, 0x00, 0xA0}`, `0x64=0x1E`, `0x5C=0x0B`)을 캡처합니다.
- 캡처된 160x160 프레임에 대해 8컬럼 ADC 멀티플렉서 경계 오프셋 보정(`0x180006fe8`)을 수행하고 `fpi_image_resize(orig, 2, 2)`를 거쳐 NBIS 특징점 추출기로 전달합니다.
