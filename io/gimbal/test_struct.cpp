#include <iostream>
#include <cstdint>

#pragma pack(push, 1)
struct FrameHead {
    uint8_t header = 0xff;
    uint8_t length = 0;
    uint8_t id = 0;
};
#pragma pack(pop)

int main() {
    // 模拟接收到的数据
    uint8_t data[] = {0xff, 0x11, 0x14, 0x01};

    FrameHead head;
    memcpy(&head, data, sizeof(FrameHead));

    std::cout << "解析结果：" << std::endl;
    std::cout << "header: 0x" << std::hex << (int)head.header << std::endl;
    std::cout << "length: 0x" << std::hex << (int)head.length << std::endl;
    std::cout << "id: 0x" << std::hex << (int)head.id << std::endl;

    std::cout << "\n期望值：" << std::endl;
    std::cout << "header: 0xff" << std::endl;
    std::cout << "length: 0x11" << std::endl;
    std::cout << "id: 0x14" << std::endl;

    if (head.id == 0x14) {
        std::cout << "\n✓ 解析正确！" << std::endl;
    } else {
        std::cout << "\n✗ 解析错误！ID字段不匹配" << std::endl;
        std::cout << "可能原因：下位机的字段顺序不同" << std::endl;
    }

    return 0;
}