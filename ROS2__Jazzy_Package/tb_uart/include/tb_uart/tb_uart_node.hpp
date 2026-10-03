#ifndef TB_UART__TB_UART_NODE_HPP_
#define TB_UART__TB_UART_NODE_HPP_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

#include "tb_uart/rt_u2s_uart.hpp"

namespace tb {

class TbUartNode : public rclcpp::Node {
public:
  TbUartNode();
  ~TbUartNode() override;

private:
  void on_command(const std_msgs::msg::String::SharedPtr msg);
  bool open_port();   // 포트 열기 (channel_ == -1이면 ttyUSB0~9 자동 탐색)
  void cmd_start();
  void cmd_quit();

  void rx_loop();
  void on_velocity(const int16_t v[2]);
  void on_psd(const uint16_t p[3]);
  void publish_text(const std::string &text);

  int channel_{0};
  static constexpr unsigned int BAUDRATE = 1000000;

  RtU2sUart rt_;
  bool initialized_{false};

  std::thread rx_thread_;
  std::atomic<bool> running_{false};

  // ID 설정 (rx 스레드와 공유)
  std::mutex mtx_;
  int idL_{0}, idR_{1};
  int psdF_{0}, psdL_{1}, psdR_{2};

  rclcpp::TimerBase::SharedPtr retry_timer_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_;
};

}  // namespace tb

#endif  // TB_UART__TB_UART_NODE_HPP_