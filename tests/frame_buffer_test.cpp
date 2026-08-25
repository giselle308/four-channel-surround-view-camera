#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

#include "frame_buffer.hpp"

int main()
{
    FrameBuffer buffer;
    std::shared_ptr<const std::vector<std::uint8_t>> fifth_image;

    for (std::uint64_t number = 1; number <= 5; ++number) {
        auto image = std::make_shared<std::vector<std::uint8_t>>(16, static_cast<std::uint8_t>(number));
        auto packet = std::make_shared<FramePacket>();
        packet->frame_number = number;
        packet->image_data = image;
        if (number == 5) {
            fifth_image = image;
        }
        buffer.push(std::move(packet));
    }

    const auto frames = buffer.snapshot();
    assert(frames.size() == FrameBuffer::kCapacity);
    assert(frames[0]->frame_number == 2);
    assert(frames[1]->frame_number == 3);
    assert(frames[2]->frame_number == 4);
    assert(frames[3]->frame_number == 5);
    assert(buffer.latest()->frame_number == 5);
    assert(buffer.latest()->image_data == fifth_image);
    assert(buffer.size() == 4);
    assert(buffer.publishedCount() == 5);
    assert(buffer.overwrittenCount() == 1);
    buffer.clear();
    assert(buffer.size() == 0);
    assert(buffer.latest() == nullptr);
    assert(buffer.overwrittenCount() == 0);
    return 0;
}
