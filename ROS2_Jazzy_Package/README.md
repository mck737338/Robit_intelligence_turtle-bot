# tb_uart ROS 2 노드 Topic 명령

`tb_uart_node`와 주고받는 ROS 2 topic 명령을 정리한 문서.

## 1. Topic

| 방향 | Topic | 타입 | 설명 |
|---|---|---|---|
| 송신 (사용자 → 노드) | TOPIC_COMMAND | `std_msgs/msg/String` | 노드에 보내는 명령 |
| 수신 (노드 → 사용자) | TOPIC_STATUS | `std_msgs/msg/String` | 노드가 발행하는 값 |

- topic 이름은 `src/tb_uart_node.cpp` 상단의 `#define TOPIC_COMMAND`, `#define TOPIC_STATUS`에 지정하여 설정한다.
- topic 이름 미수정시 TOPIC_COMMAND: "TB_Uart_RX", TOPIC_STATUS: "TB_Uart_TX"로 설정되어 있다.
- 두 topic 이름을 같게 설정하면 되먹임 루프가 생기므로 서로 다르게 유지한다.

## 2. 송신 명령 (TOPIC_COMMAND)

### 2.1 시작/종료

| 명령 | 동작 |
|---|---|
| `start` | 시리얼 포트를 열고(1000000 baud) 시작 신호 전송 |
| `quit` | 종료 신호 전송 |

### 2.2 자동 발행 주기 설정

| 명령 | 동작 |
|---|---|
| `velocity period <ms>` | velocity 자동 발행 주기 설정 |
| `psd period <ms>` | psd 자동 발행 주기 설정 |

- `<ms>`는 0 ~ 65535이며 `0`은 자동 발행 비활성화이다.

### 2.3 ID 설정

| 명령 | 의미 | 기본값 |
|---|---|---|
| `velocity id set <idL> <idR>` | velocity L, R에 대응하는 STM32 번호 | `0 1` |
| `psd id set <idF> <idL> <idR>` | psd F, L, R에 대응하는 STM32 번호 | `0 1 2` |

- velocity는 0~1, psd는 0~2 범위이며 서로 중복되면 안 된다.
- 예: `velocity id set: 1 0` 설정 시 L은 STM32 velocity1, R은 velocity0이다.

### 2.4 velocity 전송

| 명령 | 동작 |
|---|---|
| `velocity <L> <R>` | L, R velocity를 함께 전송 (id 순서에 맞게 변환) |
| `velocity L <v>` | L velocity만 전송 |
| `velocity R <v>` | R velocity만 전송 |

- 예 (idL=1, idR=0): `velocity: 50 100` → STM32에 {velocity0=100, velocity1=50} 전송

### 2.5 데이터 요청

| 명령 | 동작 |
|---|---|
| `get velocity` | velocity 전체 요청 |
| `get velocity L` | L velocity 요청 |
| `get velocity R` | R velocity 요청 |
| `get psd` | psd 전체 요청 |
| `get psd F` | F psd 요청 |
| `get psd L` | L psd 요청 |
| `get psd R` | R psd 요청 |

### 2.6 처리 조건

| 명령 | `start` 전 |
|---|---|
| `start`, `velocity id set`, `psd id set` | 처리됨 |
| 그 외 모든 명령 | `not started` 경고 후 무시 |

- 정의되지 않은 문자열은 `unknown command` 경고 후 무시한다.

## 3. 수신 값 (TOPIC_STATUS)

| 발행 값 | 의미 | 값 순서 |
|---|---|---|
| `velocity <L> <R>` | velocity 수신 값 | L, R |
| `psd <F> <L> <R>` | psd 수신 값 | F, L, R |

- 값은 정수이며 공백으로 구분한다.
- velocity 두 값, psd 세 값이 함께 수신될 때만 발행한다. 개별 값(`velocity x: ~`, `psd x: ~`)은 발행하지 않는다.
- `get velocity`, `get psd` 요청의 응답과 자동 발행 값이 발행된다. 개별 요청(`get velocity L` 등)의 응답은 발행되지 않는다.
- 수신 값에 범위 검사는 없으며 STM32가 보낸 값을 그대로 발행한다.
- 발행 순서는 2.3의 ID 설정에 따른다.

예 (기본 ID 설정): 

```
velocity: 285 -285
psd: 4095 2048 0
```

예 (`velocity id set: 1 0` 설정 후 같은 수신값):

```
velocity: -285 285
```

## 4. 사용 예
수정한 토픽으로 변경하여 실행
원활한 테스트는 test_node 참고

```bash
# 수신 값 확인
ros2 topic echo /TB_Uart_TX

# 명령 전송
ros2 topic pub --once /TB_Uart_RX std_msgs/msg/String "{data: 'start'}"
ros2 topic pub --once /TB_Uart_RX std_msgs/msg/String "{data: 'velocity id set: 1 0'}"
ros2 topic pub --once /TB_Uart_RX std_msgs/msg/String "{data: 'psd id set: 2 0 1'}"
ros2 topic pub --once /TB_Uart_RX std_msgs/msg/String "{data: 'velocity period: 10'}"
ros2 topic pub --once /TB_Uart_RX std_msgs/msg/String "{data: 'psd period: 100'}"
ros2 topic pub --once /TB_Uart_RX std_msgs/msg/String "{data: 'velocity: 50 100'}"
ros2 topic pub --once /TB_Uart_RX std_msgs/msg/String "{data: 'get psd'}"
ros2 topic pub --once /tb_uart/command std_msgs/msg/String "{data: 'quit'}"
```
