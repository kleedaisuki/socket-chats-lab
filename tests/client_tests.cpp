#include "../src/client/common.hpp"
#include <random>
#include <stdexcept>

int
main() {
    std::mt19937 generator(731);
    for (int trial = 0; trial < 2000; ++trial) {
        chat::Frame frame{trial % 2 ? chat::Type::message : chat::Type::receipt, 0, {}};
        auto fields = generator() % 12;
        for (unsigned i = 0; i < fields; ++i) {
            std::string value;
            auto length = generator() % 300;
            for (unsigned j = 0; j < length; ++j) {
                if (generator() % 7 == 0)
                    value += "中文🌷";
                else
                    value += static_cast<char>(generator() % 128);
            }
            frame.fields.push_back(std::move(value));
        }
        auto line = client::journal_line(frame);
        auto decoded = client::journal_frame(line);
        if (decoded.type != frame.type || decoded.fields != frame.fields)
            throw std::runtime_error("JSONL round-trip property failed");
        if (line.find('\n') != line.npos || line.find('\r') != line.npos)
            throw std::runtime_error("JSONL record contains a raw newline");
    }
    std::cout << "PASS 2000 seeded JSONL round-trip cases with UTF-8 and all ASCII controls\n";
}
