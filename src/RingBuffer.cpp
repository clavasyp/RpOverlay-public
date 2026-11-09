#include "RingBuffer.h"

#include <algorithm>

RingBuffer::RingBuffer(int capacitySamples)
    : cap_(capacitySamples > 0 ? capacitySamples : 1) {
    buf_.assign(static_cast<size_t>(cap_), 0.0f);
}

void RingBuffer::pushOne(float v) {
    buf_[static_cast<size_t>(head_)] = v;
    head_ = (head_ + 1) % cap_;
    if (count_ < cap_) ++count_;
}

void RingBuffer::pushInt16(const int16_t *data, int n) {
    if (!data || n <= 0) return;
    std::lock_guard<std::mutex> lk(m_);
    // Если кадр длиннее ёмкости — нас интересует только его хвост (последние cap_ сэмплов).
    int start = (n > cap_) ? (n - cap_) : 0;
    for (int i = start; i < n; ++i) {
        // int16 → float [-1,1]: делим на 32768 (симметрично, без клиппинга на -32768).
        pushOne(static_cast<float>(data[i]) / 32768.0f);
    }
}

void RingBuffer::pushFloat(const float *data, int n) {
    if (!data || n <= 0) return;
    std::lock_guard<std::mutex> lk(m_);
    int start = (n > cap_) ? (n - cap_) : 0;
    for (int i = start; i < n; ++i) pushOne(data[i]);
}

int RingBuffer::snapshot(std::vector<float> &out, int n) const {
    std::lock_guard<std::mutex> lk(m_);
    int take = (n <= 0) ? count_ : std::min(n, count_);
    out.resize(static_cast<size_t>(take));
    if (take == 0) return 0;
    // Последние take сэмплов заканчиваются на (head_-1). Начало = head_-take (по модулю cap_).
    int begin = head_ - take;
    begin %= cap_;
    if (begin < 0) begin += cap_;
    for (int i = 0; i < take; ++i) {
        out[static_cast<size_t>(i)] = buf_[static_cast<size_t>((begin + i) % cap_)];
    }
    return take;
}

int RingBuffer::available() const {
    std::lock_guard<std::mutex> lk(m_);
    return count_;
}

void RingBuffer::clear() {
    std::lock_guard<std::mutex> lk(m_);
    head_ = 0;
    count_ = 0;
}
