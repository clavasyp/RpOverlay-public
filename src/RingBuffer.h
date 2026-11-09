#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

// RingBuffer — потокобезопасный кольцевой буфер моно-аудио (float32 в диапазоне [-1,1])
// фиксированной ёмкости (по умолчанию ~120 с при 16 кГц). Пишет аудио-поток захвата
// (push из слота AudioCapture::frame), читает compute-поток по хоткею (snapshot последних
// N секунд). Один экземпляр = один источник ("mic" или "other"); микс источников делается
// на чтении (см. SherpaPipeline / владельца буферов).
//
// Семантика: при переполнении старые сэмплы затираются (как у кольца) — храним только
// последние cap_ сэмплов. snapshot() копирует последние n сэмплов (или меньше, если ещё
// не накопилось) в хронологическом порядке (старый→новый).
class RingBuffer {
public:
    // capacitySamples — ёмкость в сэмплах (напр. 120*16000 = 1 920 000 = ~120 с при 16 кГц).
    explicit RingBuffer(int capacitySamples);

    // Дозапись int16 LE (как эмитит AudioCapture): конвертация в float [-1,1] на лету.
    void pushInt16(const int16_t *data, int n);
    // Дозапись готового float [-1,1].
    void pushFloat(const float *data, int n);

    // Копирует последние n сэмплов (или меньше, если накоплено < n) в out, в хронологическом
    // порядке. Возвращает фактически записанное число сэмплов. n<=0 → весь доступный буфер.
    int snapshot(std::vector<float> &out, int n) const;

    int available() const;  // сколько валидных сэмплов сейчас в буфере (<= capacity)
    int capacity() const { return cap_; }
    void clear();

private:
    void pushOne(float v);

    mutable std::mutex m_;
    std::vector<float> buf_;
    int cap_;
    int head_ = 0;   // индекс следующей записи
    int count_ = 0;  // число валидных сэмплов (<= cap_)
};
