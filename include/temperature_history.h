#pragma once
#include <stdint.h>
#include <math.h>

// Fixed storage: one sample per minute, up to the last hour since boot.
class TemperatureHistory {
public:
    static constexpr uint32_t SAMPLE_MS = 60000;
    static constexpr uint32_t WINDOW_MS = 3600000;
    static constexpr uint32_t TREND_MS = 300000;
    static constexpr uint32_t MAX_GAP_MS = 90000;
    struct Sample { float celsius; uint32_t time; };
    enum class Trend { Unknown, Steady, Rising, Falling };

    bool record(float value, uint32_t now) {
        if (!isfinite(value)) return false;
        if (count_ && uint32_t(now - at(count_ - 1).time) < SAMPLE_MS) return false;
        // Circular buffer: new samples overwrite the oldest once storage is full.
        samples_[next_] = {value, now};
        next_ = (next_ + 1) % CAPACITY;
        if (count_ < CAPACITY) ++count_;
        return true;
    }
    uint8_t count() const { return count_; }
    const Sample &at(uint8_t index) const {
        // Present samples in time order: index 0 is the oldest stored reading.
        return samples_[(next_ + CAPACITY - count_ + index) % CAPACITY];
    }
    Trend trend(float current, uint32_t now) const {
        // Require a continuous recent five-minute baseline, not an old value
        // across an outage. Unsigned elapsed times work across millis wrap.
        for (int i = count_ - 1; i >= 0; --i) {
            const uint32_t age = now - at(i).time;
            if (i == count_ - 1 && age > MAX_GAP_MS) return Trend::Unknown;
            if (i < count_ - 1 && uint32_t(at(i + 1).time - at(i).time) > MAX_GAP_MS)
                return Trend::Unknown;
            if (age >= TREND_MS) {
                if (age > TREND_MS + MAX_GAP_MS) return Trend::Unknown;
                const float change = current - at(i).celsius;
                // Ignore small steps so the arrow does not react to sensor noise.
                if (change >= 0.2f) return Trend::Rising;
                if (change <= -0.2f) return Trend::Falling;
                return Trend::Steady;
            }
        }
        return Trend::Unknown;
    }
private:
    static constexpr uint8_t CAPACITY = 61;
    Sample samples_[CAPACITY] = {};
    uint8_t next_ = 0, count_ = 0;
};
