#pragma once

#include <QByteArray>

// dmdbg emits a linear transcript. ConPTY additionally emits display controls
// (cursor visibility, erase, attributes and window title); those are not protocol
// text. Keep state across reads, including an ESC or UTF-8 sequence split by pipes.
class DmdbgTerminalDecoder {
public:
    QByteArray decode(const QByteArray& bytes) {
        QByteArray text;
        text.reserve(bytes.size());
        for (const unsigned char value : bytes) {
            switch (state_) {
            case State::Text:
                if (value == 0x1b) state_ = State::Escape;
                else text += static_cast<char>(value);
                break;
            case State::Escape:
                if (value == '[') { state_ = State::Csi; cursorColumns_ = 0; }
                else if (value == ']' || value == 'P' || value == '^' || value == '_') state_ = State::String;
                else state_ = State::Text;
                break;
            case State::Csi:
                if (value >= 0x40 && value <= 0x7e) {
                    // Wine ConPTY emits cursor-forward for blanks within a line.
                    if (value == 'C' && cursorColumns_ >= 0)
                        text.append(QByteArray(qMax(1, cursorColumns_), ' '));
                    state_ = State::Text;
                } else if (value >= '0' && value <= '9' && cursorColumns_ >= 0) {
                    cursorColumns_ = qMin(256, cursorColumns_ * 10 + value - '0');
                } else {
                    cursorColumns_ = -1;
                }
                break;
            case State::String:
                if (value == 0x07) state_ = State::Text;
                else if (value == 0x1b) state_ = State::StringEscape;
                break;
            case State::StringEscape:
                if (value == '\\' || value == 0x07) state_ = State::Text;
                else if (value != 0x1b) state_ = State::String;
                break;
            }
        }
        return text;
    }

private:
    enum class State { Text, Escape, Csi, String, StringEscape };
    State state_ = State::Text;
    int cursorColumns_ = 0;
};
