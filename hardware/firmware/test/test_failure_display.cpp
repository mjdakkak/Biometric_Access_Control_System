// Host-only test: c++ -std=c++11 -I src test/test_failure_display.cpp -o /tmp/test_errors
#include "FailureDisplay.h"
#include <assert.h>
#include <string>
#include <random>
#include <iostream>

static size_t checks = 0;
static void verify(const char *input) {
    const auto result = FailureDisplay::format(input);
    assert(result.count >= 1 && result.count <= FailureDisplay::MAX_PAGES);
    ++checks;
    for (size_t p = 0; p < result.count; ++p) {
        const std::string text(result.text[p]);
        assert(text.size() < FailureDisplay::PAGE_BYTES); ++checks;
        size_t linePixels = 0, lines = 1;
        for (unsigned char c : text) {
            if (c == '\n') {
                assert(linePixels <= FailureDisplay::LINE_PIXELS);
                ++checks; linePixels = 0; ++lines;
            } else {
                assert(c >= 32 && c <= 126 && c != '\\' && c != '"');
                linePixels += FailureDisplay::advance(c);
            }
        }
        assert(linePixels <= FailureDisplay::LINE_PIXELS && lines <= 4); ++checks;
        char command[FailureDisplay::COMMAND_TEXT_BYTES];
        assert(FailureDisplay::encode(text.c_str(), command, sizeof(command))); ++checks;
        for (size_t i=0; command[i]; ++i) {
            assert(command[i] != '"' && command[i] != '\n' && command[i] != '\r');
            if (command[i] == '\\') assert(command[++i] == 'r');
        }
    }
}
int main() {
    const char *cases[] = {nullptr, "", "   ", "INVALID_PIN", "FACE_MISMATCH",
        "FINGERPRINT_MISMATCH", "Fingerprint hardware disabled", "Camera hardware disabled",
        "No face found. Try again", "Authentication session expired", "Enrollment session expired",
        "Server error; result may be unknown", "Fingerprint session needs reconciliation",
        "UNEXPECTED_FINGERPRINT_SLOT", "\"; page page6 \\r \\n", "A\nB\rC\tD",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_!?./()[]<>"};
    for (const char *s : cases) verify(s);
    std::string longToken(80,'W'); verify(longToken.c_str());
    assert(FailureDisplay::format(longToken.c_str()).count > 1); ++checks;
    std::string longer(300,'W'); verify(longer.c_str());
    char out[8]; assert(!FailureDisplay::encode("abc",out,3)); ++checks;
    assert(FailureDisplay::encode("a\nb",out,sizeof(out))); ++checks;
    assert(std::string(out)=="a\\rb"); ++checks;
    std::mt19937 random(90217);
    for (unsigned n=0; n<10000; ++n) {
        std::string input;
        const size_t len=random()%81;
        for(size_t i=0;i<len;++i)input.push_back(static_cast<char>(1+random()%255));
        verify(input.c_str());
    }
    std::cout << checks << " format/escape/layout checks passed\n";
    for (const char *s : {"Fingerprint hardware disabled", "No face found. Try again", "FINGERPRINT_MISMATCH"}) {
        auto pages=FailureDisplay::format(s);
        std::cout << "\n" << s << " ->\n" << pages.text[0] << "\n";
    }
}
