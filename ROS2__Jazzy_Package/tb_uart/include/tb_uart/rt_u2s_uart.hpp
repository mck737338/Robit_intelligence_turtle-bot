#ifndef TB_UART__RT_U2S_UART_HPP_
#define TB_UART__RT_U2S_UART_HPP_

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "tb_uart/ubuntu_uart.hpp"

namespace tb {

class RtU2sUart {
public:
  using VelocityCallback = std::function<void(const int16_t v[2])>;   // velocity0, velocity1
  using PsdCallback      = std::function<void(const uint16_t p[3])>;  // psd0, psd1, psd2
  using LogCallback      = std::function<void(const std::string &msg)>;

  // 초기화
  bool rt_u2s_init(int channel, unsigned int baudrate);

  // 시작/종료 (ID 00)
  bool send_start();
  bool send_end();

  // velocity 송신
  bool send_velocities(int velocity[]);                  // ID 10: velocity[0], velocity[1]
  bool send_velocity(int motor_id, int velocity);        // ID 11 (motor 0), 12 (motor 1)

  // velocity 요청
  bool request_velocities();                             // ID 20
  bool request_velocity(int motor_id);                   // ID 21, 22

  // psd 송신
  bool send_psds(int psd[]);                             // ID 30: psd[0], psd[1], psd[2]
  bool send_psd(int sensor_id, int psd);                 // ID 31, 32, 33

  // psd 요청
  bool request_psds();                                   // ID 40
  bool request_psd(int sensor_id);                       // ID 41, 42, 43

  // 데이터 자동 발행 주기 (ID 50 / 51), 0 = 비활성화
  bool send_velocity_period(uint16_t period_ms);
  bool send_psd_period(uint16_t period_ms);

  // 수신: 바이트를 파싱해 완성된 packet마다 처리. 수신 바이트 수 반환 (타임아웃 0, 오류 -1)
  // ID 10 수신 시 velocity callback, ID 30 수신 시 psd callback 호출
  int receive(int timeout_ms);
  void set_velocity_callback(VelocityCallback cb) { vel_cb_ = std::move(cb); }
  void set_psd_callback(PsdCallback cb) { psd_cb_ = std::move(cb); }
  void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }

private:
  enum class State { H1, H2, ID, DATA };

  bool send_packet(uint8_t id, const uint8_t *data, int len);
  bool send_u16_packet(uint8_t id, uint16_t value);
  void feed(uint8_t b);
  void handle_packet();
  void log(const std::string &msg) { if (log_cb_) log_cb_(msg); }

  UbuntuUart uart_;
  int channel_{0};

  VelocityCallback vel_cb_;
  PsdCallback psd_cb_;
  LogCallback log_cb_;

  State state_{State::H1};
  uint8_t id_{0};
  uint8_t len_{0};
  uint8_t idx_{0};
  uint8_t data_[6]{};
  uint8_t raw_[3 + 6]{};   // 수신 중인 packet 원본 (Header + ID + Data)
};

}  // namespace tb

#endif  // TB_UART__RT_U2S_UART_HPP_