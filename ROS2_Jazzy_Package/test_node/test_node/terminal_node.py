import threading

import rclpy
from rclpy.node import Node
from std_msgs.msg import String

tx_topic = 'TB_Uart_RX'
rx_topic = 'TB_Uart_TX'


class TerminalNode(Node):
    def __init__(self):
        super().__init__('terminal_node')
        self.pub = self.create_publisher(String, tx_topic, 10)
        self.sub = self.create_subscription(String, rx_topic, self.on_topic2, 10)

        # input()은 블로킹이므로 별도 스레드에서 실행
        self.input_thread = threading.Thread(target=self.input_loop, daemon=True)
        self.input_thread.start()

    def input_loop(self):
        while rclpy.ok():
            try:
                text = input()
            except EOFError:
                break
            msg = String()
            msg.data = text
            self.pub.publish(msg)

    def on_topic2(self, msg: String):
        print(f'[topic2] {msg.data}')


def main(args=None):
    rclpy.init(args=args)
    node = TerminalNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()