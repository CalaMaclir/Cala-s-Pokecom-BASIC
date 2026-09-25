#pragma once

namespace rmb::platform::detail {

class SerialCrLfFilter {
public:
    bool should_ignore(int code) {
        if (swallow_lf_ && code == '\n') {
            swallow_lf_ = false;
            return true;
        }
        swallow_lf_ = false;
        if (code == '\r') swallow_lf_ = true;
        return false;
    }

private:
    bool swallow_lf_ = false;
};

} // namespace rmb::platform::detail
