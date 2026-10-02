# RT-SU(Robit Turtle-bot STM32-Ubuntu) UART Packet Protocol

Turtle-Bot 프로젝트에서 Ubuntu와 STM32 간 UART 통신에서 사용하는 packet 구조

## 1. Packet 구조

```
| Header (2 byte) | Packet ID (1 byte) | Data (0 ~ 4 byte) |
|     AA 55       |        ID          |   ID에 따라 결정   |
```

| 필드 | 크기 | 값 | 설명 |
|---|---|---|---|
| Header | 2 byte | `AA 55` | Packet 시작 표시 |
| Packet ID | 1 byte | 3장 참고 | 상위 nibble = 종류, 하위 nibble = 대상 |
| Data | 0 ~ 4 byte | 4장 참고 | 길이는 Packet ID로 결정되므로 Length 필드는 없다 |

## 2. 데이터 형식

| 항목 | 자료형 | 범위 | 비고 |
|---|---|---|---|
| Velocity | int16 (2의 보수) | -285 ~ 285 | 2 byte |
| PSD | uint16 | 0 ~ 4095 | 2 byte (12bit) |
| 시작/종료 | uint8 | `00`, `FF` | 1 byte |
| 발행 주기 | uint16 | 0 ~ 65535 | 2 byte, 단위 ms, `0` = 자동발행 비활성화 |

- 2 byte 이상의 값은 **little-endian**(하위 byte 먼저)으로 전송한다.

## 3. Packet ID

### 3.1 ID 구성

Packet ID는 `상위 nibble = 종류`, `하위 nibble = 대상`으로 구성한다.

| 상위 nibble | 종류 | 방향 |
|---|---|---|
| `0x` | 프로그램 시작/종료 | 제어 |
| `1x` | Velocity 송신 | 데이터 |
| `2x` | Velocity 요청 | 요청 |
| `3x` | PSD 송신 | 데이터 |
| `4x` | PSD 요청 | 요청 |
| `5x` | 데이터 자동발행 주기 설정 | 설정 |

| 하위 nibble | 대상 (`1x` ~ `4x`) |
|---|---|
| `0` | 채널 0 + 채널 1 |
| `1` | 채널 0 |
| `2` | 채널 1 |

`5x`는 하위 nibble의 의미가 다르다. 하위 nibble은 채널이 아니라 **자동발행 대상 데이터 종류**를 나타낸다.

| 하위 nibble (`5x`) | 대상 |
|---|---|
| `0` | velocity (velocity0, velocity1) |
| `1` | psd (psd0, psd1) |

### 3.2 ID 목록

| ID | 의미 | Data | Data 길이 | 전체 길이 |
|---|---|---|---|---|
| `00` | 프로그램 시작/종료 | 시작/종료 값 (1 byte) | 1 | 4 |
| `10` | velocity0, velocity1 송신 | v0 (int16), v1 (int16) | 4 | 7 |
| `11` | velocity0 송신 | v0 (int16) | 2 | 5 |
| `12` | velocity1 송신 | v1 (int16) | 2 | 5 |
| `20` | velocity0, velocity1 요청 | 없음 | 0 | 3 |
| `21` | velocity0 요청 | 없음 | 0 | 3 |
| `22` | velocity1 요청 | 없음 | 0 | 3 |
| `30` | psd0, psd1 송신 | p0 (uint16), p1 (uint16) | 4 | 7 |
| `31` | psd0 송신 | p0 (uint16) | 2 | 5 |
| `32` | psd1 송신 | p1 (uint16) | 2 | 5 |
| `40` | psd0, psd1 요청 | 없음 | 0 | 3 |
| `41` | psd0 요청 | 없음 | 0 | 3 |
| `42` | psd1 요청 | 없음 | 0 | 3 |
| `50` | velocity0, velocity1 자동발행 주기 설정 | 발행 주기 (uint16) | 2 | 5 |
| `51` | psd0, psd1 자동발행 주기 설정 | 발행 주기 (uint16) | 2 | 5 |

## 4. Data 필드

### 4.1 프로그램 시작/종료 (`00`)

| Data | 의미 |
|---|---|
| `00` | 시작 |
| `FF` | 종료 |

### 4.2 Velocity 송신 (`10`, `11`, `12`)

| ID | Data 배치 (byte 순서) |
|---|---|
| `10` | `v0_L` `v0_H` `v1_L` `v1_H` |
| `11` | `v0_L` `v0_H` |
| `12` | `v1_L` `v1_H` |

### 4.3 PSD 송신 (`30`, `31`, `32`)

| ID | Data 배치 (byte 순서) |
|---|---|
| `30` | `p0_L` `p0_H` `p1_L` `p1_H` |
| `31` | `p0_L` `p0_H` |
| `32` | `p1_L` `p1_H` |

### 4.4 요청 (`20`, `21`, `22`, `40`, `41`, `42`)

Data 필드가 없다. Header와 Packet ID만 전송한다.

### 4.5 자동발행 주기 설정 (`50`, `51`)

| ID | Data 배치 (byte 순서) |
|---|---|
| `50` | `T_L` `T_H` |
| `51` | `T_L` `T_H` |

| Data 값 (T) | 의미 |
|---|---|
| `0` | 자동발행 비활성화 |
| `1` ~ `65535` | 자동발행 활성화, 값은 발행 주기 (ms) |

- `50`은 velocity0, velocity1을 한 주기로 함께 발행한다.
- `51`은 psd0, psd1을 한 주기로 함께 발행한다.

## 5. 요청과 응답

요청 packet을 받으면 **ID에서 `10`을 뺀 값의 데이터 packet**으로 응답한다.

| 요청 | 응답 | 내용 |
|---|---|---|
| `20` | `10` | velocity0, velocity1 |
| `21` | `11` | velocity0 |
| `22` | `12` | velocity1 |
| `40` | `30` | psd0, psd1 |
| `41` | `31` | psd0 |
| `42` | `32` | psd1 |

### 5.1 자동발행

`50`, `51`로 주기가 설정되면 해당 주기마다 요청 없이 아래 데이터 packet을 자동으로 전송한다.

| 설정 ID | 자동발행 packet | 내용 |
|---|---|---|
| `50` | `10` | velocity0, velocity1 |
| `51` | `30` | psd0, psd1 |

- 주기 값 `0`을 받으면 해당 자동발행을 중지한다.
- 자동발행 중에도 요청 packet(`2x`, `4x`)은 정상적으로 응답한다.
- 자동발행 동작 구현은 별도 코드에서 처리하며, 본 문서는 packet 구조만 정의한다.

## 6. 예시

| 내용 | Packet |
|---|---|
| 프로그램 시작 | `AA 55 00 00` |
| 프로그램 종료 | `AA 55 00 FF` |
| velocity0 = 285, velocity1 = -285 | `AA 55 10 1D 01 E3 FE` |
| velocity0 = -100 | `AA 55 11 9C FF` |
| velocity1 = 50 | `AA 55 12 32 00` |
| velocity0, velocity1 요청 | `AA 55 20` |
| psd0 = 4095, psd1 = 2048 | `AA 55 30 FF 0F 00 08` |
| psd1 = 1000 | `AA 55 32 E8 03` |
| psd0 요청 | `AA 55 41` |
| velocity 자동발행 10ms | `AA 55 50 0A 00` |
| velocity 자동발행 비활성화 | `AA 55 50 00 00` |
| psd 자동발행 100ms | `AA 55 51 64 00` |
| psd 자동발행 1000ms | `AA 55 51 E8 03` |
| psd 자동발행 비활성화 | `AA 55 51 00 00` |

값 변환 근거

| 값 | 16진수 | 전송 byte |
|---|---|---|
| 285 | `0x011D` | `1D 01` |
| -285 | `0xFEE3` | `E3 FE` |
| -100 | `0xFF9C` | `9C FF` |
| 50 | `0x0032` | `32 00` |
| 4095 | `0x0FFF` | `FF 0F` |
| 2048 | `0x0800` | `00 08` |
| 1000 | `0x03E8` | `E8 03` |
| 10 | `0x000A` | `0A 00` |
| 100 | `0x0064` | `64 00` |
| 0 | `0x0000` | `00 00` |

## 7. 유효성 규칙

| 항목 | 유효 범위 | 범위 밖일 때 |
|---|---|---|
| Packet ID | 3.2의 목록 | Packet 폐기 후 Header 재탐색 |
| 시작/종료 Data | `00`, `FF` | Packet 폐기 |
| Velocity | -285 ~ 285 | Packet 폐기 또는 ±285로 제한 |
| PSD | 0 ~ 4095 | Packet 폐기 |
| 발행 주기 | 0 ~ 65535 | uint16 전 범위가 유효 (`0` = 비활성화) |

## 8. 가정 사항

원 요구사항에 명시되지 않아 아래와 같이 가정했다. 변경이 필요하면 해당 항목만 수정한다.

| 항목 | 가정 |
|---|---|
| Header 값 | `AA 55` (2 byte) |
| 바이트 순서 | little-endian |
| Velocity 자료형 | int16 (2의 보수) |
| PSD 자료형 | uint16 |
| 요청에 대한 응답 | 요청 ID - `10`에 해당하는 데이터 packet |
| 데이터 방향 | Velocity는 호스트 → STM32 명령, PSD는 STM32 → 호스트 센서값 |
| 발행 주기 자료형 | uint16, little-endian, 단위 ms |
| `5x` 하위 nibble | 채널이 아닌 데이터 종류 (`0` = velocity, `1` = psd) |
| 자동발행 대상 | `50` → `10` packet, `51` → `30` packet (두 채널 동시 발행) |
| 주기 설정 응답 | `50`, `51`에 대한 별도 응답(ACK) packet은 없음 |
| 원 요구사항의 `none` | 길이가 ID로 고정되므로 `50`, `51`은 항상 Data 2 byte를 가진다. Data 없는 형태는 정의하지 않음 |
| PSD 채널 수 | 기존 README 기준 psd0, psd1 (2채널)로 통일 |
