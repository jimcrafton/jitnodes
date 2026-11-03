#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace jitnodes {

// A MIDI-style channel message at a frame offset inside the block being rendered.
struct AudioEvent {
    uint32_t offset = 0;
    uint8_t status = 0;  // message type and channel, e.g. 0x90 note on
    uint8_t data1 = 0;
    uint8_t data2 = 0;

    uint8_t type() const { return status & 0xF0; }
    bool isNoteOn() const { return type() == 0x90 && data2 != 0; }
    bool isNoteOff() const { return type() == 0x80 || (type() == 0x90 && data2 == 0); }
};

// The events of one block, kept sorted by offset (equal offsets stay in arrival order). Fixed capacity, so filling it on the audio thread
// never allocates; push() returns false and drops the event when it is full.
class EventBuffer {
public:
    static constexpr size_t capacity = 256;

    bool push(const AudioEvent& e) {
        if (size_ == capacity) return false;
        size_t i = size_++;
        while (i > 0 && events_[i - 1].offset > e.offset) {
            events_[i] = events_[i - 1];
            --i;
        }
        events_[i] = e;
        return true;
    }

    void clear() { size_ = 0; }
    size_t size() const { return size_; }
    const AudioEvent& operator[](size_t i) const { return events_[i]; }
    const AudioEvent* begin() const { return events_.data(); }
    const AudioEvent* end() const { return events_.data() + size_; }

private:
    std::array<AudioEvent, capacity> events_{};
    size_t size_ = 0;
};

}  // namespace jitnodes
