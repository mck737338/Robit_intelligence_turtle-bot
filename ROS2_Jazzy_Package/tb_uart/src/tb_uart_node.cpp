#include "tb_uart/tb_uart_node.hpp"

#include <chrono>
#include <cstdio>
#include <unistd.h>
#include <vector>
#include <cerrno>

// ---------- topic 이름 ----------
#define TOPIC_COMMAND "TB_Uart_RX"   // 입력: 명령 수신
#define TOPIC_STATUS  "TB_Uart_TX"    // 출력: 상태/데이터 발행

namespace tb {

TbUartNode::TbUartNode() : Node("tb_uart_node")
{
  channel_ = declare_parameter<int>("channel", -1);   // -1: 자동 탐색

  pub_ = create_publisher<std_msgs::msg::String>(TOPIC_STATUS, 10);
  sub_ = create_subscription<std_msgs::msg::String>(
    TOPIC_COMMAND, 10,
    [this](const std_msgs::msg::String::SharedPtr msg) { on_command(msg); });

  rt_.set_velocity_callback([this](const int16_t v[2]) { on_velocity(v); });
  rt_.set_psd_callback([this](const uint16_t p[3]) { on_psd(p); });
  rt_.set_log_callback([this](const std::string &m) {
    RCLCPP_INFO(get_logger(), "%s", m.c_str());
  });

  // 노드 실행 시 UART 열기. 실패하면 열릴 때까지 1초마다 재시도
  if (!open_port()) {
    RCLCPP_WARN(get_logger(), "UART not found, retrying every 1s...");
    retry_timer_ = create_wall_timer(std::chrono::seconds(1), [this]() {
      if (open_port()) retry_timer_->cancel();
    });
  }
}

TbUartNode::~TbUartNode()
{
  running_ = false;
  if (rx_thread_.joinable()) rx_thread_.join();
}

bool TbUartNode::open_port()
{
  if (initialized_) return true;

  // 1. 후보 장치 탐색 (channel 지정 시 그 번호만, -1이면 ttyUSB0~9 중 존재하는 장치)
  std::vector<int> candidates;
  if (channel_ >= 0) {
    candidates.push_back(channel_);
  } else {
    for (int i = 0; i < 10; ++i) {
      const std::string dev = "/dev/ttyUSB" + std::to_string(i);
      if (access(dev.c_str(), F_OK) == 0) candidates.push_back(i);
    }
  }

  if (candidates.empty()) {
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 10000,
      "No /dev/ttyUSB* device found. Connect the USB-UART adapter.");
    return false;
  }

  // 2. 권한 검사 후 열기
  bool permission_denied = false;
  for (int ch : candidates) {
    const std::string dev = "/dev/ttyUSB" + std::to_string(ch);

    if (access(dev.c_str(), R_OK | W_OK) != 0) {
      if (errno == EACCES) permission_denied = true;
      continue;   // 권한이 없으면 열기를 시도하지 않고 다음 후보로
    }

    if (rt_.rt_u2s_init(ch, BAUDRATE)) {
      initialized_ = true;
      running_ = true;
      rx_thread_ = std::thread(&TbUartNode::rx_loop, this);
      RCLCPP_INFO(get_logger(), "UART opened: %s", dev.c_str());
      return true;
    }
  }

  if (permission_denied) {
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 10000,
      "UART found but permission denied. Add user to 'dialout' group "
      "(sudo usermod -aG dialout $USER, then re-login) or install the udev rule.");
  }
  return false;
}

void TbUartNode::publish_text(const std::string &text)
{
  std_msgs::msg::String m;
  m.data = text;
  pub_->publish(m);
  RCLCPP_INFO(get_logger(), "[TOPIC TX] '%s'", text.c_str());
}

void TbUartNode::on_command(const std_msgs::msg::String::SharedPtr msg)
{
  const std::string &s = msg->data;
  RCLCPP_INFO(get_logger(), "[TOPIC RX] '%s'", s.c_str());

  int a = 0, b = 0, c = 0;
  char ch = 0;

  // ---- 시작/종료 ----
  if (s == "start") {
    cmd_start();
    return;
  }
  if (s == "quit") {
    cmd_quit();
    return;
  }

  // ---- ID 설정 (포트 열기 전에도 가능) ----
  if (std::sscanf(s.c_str(), "velocity id set: %d %d", &a, &b) == 2) {
    if (a < 0 || a > 1 || b < 0 || b > 1 || a == b) {
      RCLCPP_WARN(get_logger(), "invalid velocity id: %d %d", a, b);
      return;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    idL_ = a;
    idR_ = b;
    RCLCPP_INFO(get_logger(), "velocity id set: idL=%d idR=%d", idL_, idR_);
    return;
  }
  if (std::sscanf(s.c_str(), "psd id set: %d %d %d", &a, &b, &c) == 3) {
    if (a < 0 || a > 2 || b < 0 || b > 2 || c < 0 || c > 2 ||
        a == b || b == c || a == c) {
      RCLCPP_WARN(get_logger(), "invalid psd id: %d %d %d", a, b, c);
      return;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    psdF_ = a;
    psdL_ = b;
    psdR_ = c;
    RCLCPP_INFO(get_logger(), "psd id set: psdF=%d psdL=%d psdR=%d", psdF_, psdL_, psdR_);
    return;
  }

  // 이하 명령은 UART 송신이 필요하므로 start 이후에만 처리
  auto require_started = [this]() {
    if (!initialized_) RCLCPP_WARN(get_logger(), "not started");
    return initialized_;
  };

  // ---- 자동 발행 주기 ----
  if (std::sscanf(s.c_str(), "velocity period: %d", &a) == 1) {
    if (!require_started()) return;
    if (a < 0 || a > 65535) { RCLCPP_WARN(get_logger(), "period out of range: %d", a); return; }
    rt_.send_velocity_period(static_cast<uint16_t>(a));
    return;
  }
  if (std::sscanf(s.c_str(), "psd period: %d", &a) == 1) {
    if (!require_started()) return;
    if (a < 0 || a > 65535) { RCLCPP_WARN(get_logger(), "period out of range: %d", a); return; }
    rt_.send_psd_period(static_cast<uint16_t>(a));
    return;
  }

  // ---- velocity 송신 ----
  if (std::sscanf(s.c_str(), "velocity: %d %d", &a, &b) == 2) {
    if (!require_started()) return;
    int vel[2];
    {
      std::lock_guard<std::mutex> lk(mtx_);
      vel[idL_] = a;   // Lvelocity -> idL 위치
      vel[idR_] = b;   // Rvelocity -> idR 위치
    }
    rt_.send_velocities(vel);
    return;
  }
  if (std::sscanf(s.c_str(), "velocity L: %d", &a) == 1) {
    if (!require_started()) return;
    int id;
    { std::lock_guard<std::mutex> lk(mtx_); id = idL_; }
    rt_.send_velocity(id, a);
    return;
  }
  if (std::sscanf(s.c_str(), "velocity R: %d", &a) == 1) {
    if (!require_started()) return;
    int id;
    { std::lock_guard<std::mutex> lk(mtx_); id = idR_; }
    rt_.send_velocity(id, a);
    return;
  }

  // ---- velocity 요청 ----
  if (s == "get velocity") {
    if (!require_started()) return;
    rt_.request_velocities();
    return;
  }
  if (std::sscanf(s.c_str(), "get velocity %c", &ch) == 1) {
    if (!require_started()) return;
    int id;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (ch == 'L') id = idL_;
      else if (ch == 'R') id = idR_;
      else { RCLCPP_WARN(get_logger(), "invalid velocity target: '%c' (L/R)", ch); return; }
    }
    rt_.request_velocity(id);
    return;
  }

  // ---- psd 요청 ----
  if (s == "get psd") {
    if (!require_started()) return;
    rt_.request_psds();
    return;
  }
  if (std::sscanf(s.c_str(), "get psd %c", &ch) == 1) {
    if (!require_started()) return;
    int id;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (ch == 'F') id = psdF_;
      else if (ch == 'L') id = psdL_;
      else if (ch == 'R') id = psdR_;
      else { RCLCPP_WARN(get_logger(), "invalid psd target: '%c' (F/L/R)", ch); return; }
    }
    rt_.request_psd(id);
    return;
  }

  RCLCPP_WARN(get_logger(), "unknown command: '%s'", s.c_str());
}

void TbUartNode::cmd_start()
{
  if (!open_port()) {
    RCLCPP_ERROR(get_logger(), "UART not opened");
    return;
  }
  rt_.send_start();
}

void TbUartNode::cmd_quit()
{
  if (!initialized_) { RCLCPP_WARN(get_logger(), "not started"); return; }
  rt_.send_end();
}

void TbUartNode::rx_loop()
{
  while (running_ && rclcpp::ok()) {
    if (rt_.receive(100) < 0) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000, "serial receive error");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }
}

void TbUartNode::on_velocity(const int16_t v[2])
{
  std::lock_guard<std::mutex> lk(mtx_);
  publish_text("velocity: " + std::to_string(v[idL_]) + " " + std::to_string(v[idR_]));
}

void TbUartNode::on_psd(const uint16_t p[3])
{
  std::lock_guard<std::mutex> lk(mtx_);
  publish_text("psd: " + std::to_string(p[psdF_]) + " " +
               std::to_string(p[psdL_]) + " " + std::to_string(p[psdR_]));
}

}  // namespace tb