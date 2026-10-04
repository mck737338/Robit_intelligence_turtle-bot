# tb_uart 설치 가이드

Ubuntu 24.04 + ROS 2 Jazzy 기준으로 `tb_uart` 패키지를 설치하고 UART 접근 권한을 설정하는 방법.

## 1. 사전 준비

```bash
# ROS 2 Jazzy 설치 확인
source /opt/ros/jazzy/setup.bash
ros2 --version

# 빌드 도구 (없으면 설치)
sudo apt update
sudo apt install -y git python3-colcon-common-extensions
```

## 2. 워크스페이스 생성

이미 `~/ros2_ws`가 있거나 다른 워크스페이스를 사용하면 건너뛴다.

```bash
mkdir -p ~/ros2_ws/src
```

## 3. tb_uart 패키지 설치 (git clone)

워크스페이스 이름이 ros2_ws가 아니라면 첫 줄의 ros2_ws를 해당 워크스페이스 이름으로 변경하여 진행한다.

```bash
path=~/ros2_ws/src   # 설치할 경로

git clone --depth 1 --filter=blob:none --sparse https://github.com/mck737338/Robit_intelligence_turtle-bot.git /tmp/repo_tmp
cd /tmp/repo_tmp
git sparse-checkout set ROS2_Jazzy_Package/tb_uart

cp -r ROS2_Jazzy_Package/tb_uart "$path"/
cd ~ && rm -rf /tmp/repo_tmp
```

- 설치 후 구조:
```
  ~/ros2_ws/src/tb_uart/
  ├── CMakeLists.txt
  ├── package.xml
  ├── setup_uart.sh
  ├── include/tb_uart/
  └── src/
```

tb_uart의 topic에 관한 내용은 [tb_uart/README.md](ROS2_Jazzy_Package/README.md) 참고

## 3-1. test_node 패키지 설치(선택)

위와 동일하게 워크스페이스 이름 확인하여 진행

```bash
path=~/ros2_ws/src

git clone --depth 1 --filter=blob:none --sparse https://github.com/mck737338/Robit_intelligence_turtle-bot.git /tmp/repo_tmp
cd /tmp/repo_tmp
git sparse-checkout set ROS2_Jazzy_Package/test_node

cp -r ROS2_Jazzy_Package/test_node "$path"/
cd ~ && rm -rf /tmp/repo_tmp
```

test_node의 사용방법은 [test_node/README.md](ROS2_Jazzy_Package/test_node/README.md) 참고

## 4. UART 접근 권한 설정 (setup_uart.sh)

`setup_uart.sh`는 다음을 설정한다.

- udev 규칙 추가: 로그인한 사용자가 `/dev/ttyUSB*`를 열 수 있게 함
- 현재 사용자를 `dialout` 그룹에 추가
- `brltty` 제거 (USB-UART 장치를 가로채는 것을 방지)
- 위와 동일하게 ros2_ws를 워크스페이스 이름으로 변경하여 실행

```bash
cd ~/ros2_ws/src/tb_uart
chmod +x setup_uart.sh
./setup_uart.sh
```

실행 후 **USB-UART 어댑터를 뽑았다가 다시 꽂는다.**

- udev 규칙은 어댑터를 다시 꽂을 때 적용된다. 재부팅은 필요 없다.
- `dialout` 그룹은 로그아웃 후 재로그인해야 적용된다. SSH로 접속해 사용하는 경우에 필요하다.

## 5. 빌드

```bash
cd ~/ros2_ws
source /opt/ros/jazzy/setup.bash
colcon build --symlink-install --packages-select tb_uart
source install/setup.bash
```

터미널을 열 때마다 자동으로 불러오려면 `~/.bashrc`에 추가한다.

```bash
echo "source /opt/ros/jazzy/setup.bash" >> ~/.bashrc
echo "source ~/ros2_ws/install/setup.bash" >> ~/.bashrc
```

## 6. 실행

```bash
ros2 run tb_uart tb_uart_node
```

- 노드가 실행되면 `/dev/ttyUSB0~9`를 자동 탐색해 UART를 연다.
- 어댑터가 없거나 권한이 없으면 1초마다 재시도하며, 연결되는 즉시 자동으로 열린다.
- 포트 번호를 고정하려면 다음과 같이 실행한다.
```bash
  ros2 run tb_uart tb_uart_node --ros-args -p channel:=0
```

## 7. 설치 확인

```bash
ls -l /dev/ttyUSB0
getfacl /dev/ttyUSB0     # user:<계정>:rw- 가 보이면 udev 규칙 적용됨
groups                   # dialout 이 보이면 그룹 적용됨
```

둘 중 하나만 적용되어도 노드는 포트를 열 수 있다.

정상 실행 시 노드 로그:

```
[INFO] [UART] init channel 0 @ 1000000 baud : OK
[INFO] UART opened: /dev/ttyUSB0
```

## 8. 동작 테스트

```bash
# 터미널 1: 수신 값 확인
ros2 topic echo /tb_uart/status

# 터미널 2: 명령 전송
ros2 topic pub --once /tb_uart/command std_msgs/msg/String "{data: 'start'}"
```

## 9. 문제 해결

| 증상 | 원인 | 조치 |
|---|---|---|
| `No /dev/ttyUSB* device found` | 어댑터 미인식 | 케이블과 포트 확인, `lsusb`, `sudo dmesg \| tail` |
| `permission denied` 경고 반복 | 권한 미적용 | 어댑터 재연결, 그래도 안 되면 로그아웃 후 재로그인 |
| 장치 번호가 `ttyUSB1`로 바뀜 | 어댑터 재연결 | 자동 탐색으로 처리됨, 여러 개면 `channel` 지정 |
| 빌드 시 다른 패키지 오류 | 워크스페이스의 다른 패키지 | `--packages-select tb_uart`로 단독 빌드 |

## 10. 설정 제거

```bash
sudo rm -f /etc/udev/rules.d/60-tb-uart.rules
sudo udevadm control --reload-rules
sudo gpasswd -d $USER dialout      # 재로그인 후 적용
```
