#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pico_page::gb18030 {

class Decoder {
public:
    enum class Result { Pending, Complete, Invalid };

    Result feed(std::uint8_t byte, char32_t& scalar);
    bool pending() const { return count_ != 0; }

private:
    std::uint8_t bytes_[4]{};
    std::uint8_t count_ = 0;
};

bool decode(std::string_view bytes, std::u32string& text);

}
