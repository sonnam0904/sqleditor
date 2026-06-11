#pragma once

#include <ctime>
#include <string>

// result of a datetime picker interaction
struct DateTimePickerResult {
    bool changed = false;   // value was modified
    bool committed = false; // user wants to commit (day click for date-only, or popup closed)
    std::string value;      // formatted date or datetime string
    bool setNull = false;   // user clicked NULL
};

struct DateTimePickerState {
    tm date{};
    int hour = 0;
    int minute = 0;
    int second = 0;
    int kind = 0; // 1 = date only, 2 = date+time
    std::string suffix;
    bool allowNull = true;
    bool scrollTime = false; // scroll time spinners to selection on first frame
};

namespace DateTimePicker {

    // returns 0 = not a date type, 1 = date only, 2 = date+time
    int columnKind(const std::string& columnType);

    // parse "YYYY-MM-DD [HH:MM:SS[.fff[+-tz]]]" into state; returns false on failure
    bool parse(const std::string& s, DateTimePickerState& state);

    // format state to string
    std::string format(const DateTimePickerState& state);

    // render the popup content (must be called between BeginPopup/EndPopup).
    // returns interaction result.
    DateTimePickerResult render(DateTimePickerState& state);

} // namespace DateTimePicker
