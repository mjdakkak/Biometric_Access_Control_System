#pragma once
// Presentation only. Never feed this text back into authentication decisions.
// Layout matches the supplied HMI: page7.t1 = 222x74 px; font 5 = Arial16.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace FailureDisplay {
constexpr uint8_t FONT_ID = 5;
constexpr uint32_t PAGE_HOLD_MS = 6000;
constexpr size_t MAX_INPUT = 80;
constexpr size_t MAX_PAGES = 3;
constexpr size_t LINES_PER_PAGE = 3; // Leave a fourth line for a page indicator.
constexpr size_t LINE_PIXELS = 210; // Allow margins inside the 222-pixel box.
constexpr size_t PAGE_BYTES = 101;  // Supplied HMI txt_maxl is 100.
constexpr size_t COMMAND_TEXT_BYTES = PAGE_BYTES * 2;

// Advance widths from the user's embedded Arial16 ASCII glyph table, plus a
// conservative one-pixel gap. These are metrics, not an additional font file.
inline size_t advance(unsigned char c) {
    static const uint8_t widths[95] = {4, 4, 5, 8, 8, 12, 9, 3, 5, 5, 6, 8, 4, 5, 4, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 8, 8, 8, 8, 14, 9, 9, 10, 10, 9, 9, 11, 10, 4, 7, 9, 8, 12, 10, 11, 9, 11, 10, 9, 9, 10, 9, 13, 9, 9, 9, 4, 4, 4, 7, 8, 5, 8, 8, 7, 8, 8, 4, 8, 8, 3, 3, 7, 3, 12, 8, 8, 8, 8, 5, 7, 4, 8, 7, 10, 7, 7, 7, 5, 4, 5, 8};
    return (c >= 32 && c <= 126 ? widths[c - 32] : widths[0]) + 1U;
}
inline const char *friendly(const char *reason) {
    struct Translation { const char *code; const char *text; };
    static const Translation translations[] = {
        {"INVALID_PIN", "Incorrect PIN. Try again."},
        {"UNKNOWN_PIN", "PIN not recognized. Check with your administrator."},
        {"UNKNOWN_USER", "Employee ID not found."},
        {"USER_INACTIVE", "Account inactive. Contact your administrator."},
        {"UNKNOWN_RFID", "Card not registered. Use ID and PIN."},
        {"RFID_ALREADY_REGISTERED", "This card is already registered."},
        {"SAME_RFID", "Use a different replacement card."},
        {"FACE_MISMATCH", "Face did not match the enrolled user."},
        {"FINGERPRINT_MISMATCH", "Fingerprint did not match the enrolled user."},
        {"NO_FACE_DETECTED", "Server found no face. Check framing and lighting."},
        {"MULTIPLE_FACES_DETECTED", "Only one person should be in view."},
        {"INVALID_IMAGE", "Server could not read the captured image."},
        {"UNKNOWN_SESSION_ID", "Authentication session unavailable. Start again."},
        {"UNKNOWN_ENROLLMENT_SESSION", "Enrollment session unavailable. Start again."},
        {"WRONG_BIOMETRIC_CHECK", "Unexpected biometric step. Start again."},
        {"REENROLLMENT_REQUIRED", "Credential update required. Use ID and PIN."},
        {"TEMPLATE_SLOT_ALREADY_USED", "Fingerprint slot already used. Contact your administrator."},
        {"NO_FINGERPRINT_SLOTS_AVAILABLE", "No fingerprint slots available. Contact your administrator."},
        {"UNEXPECTED_FINGERPRINT_SLOT", "Server and sensor slot do not agree."},
        {"NO_FINGERPRINT_SLOT_ASSIGNED", "Server did not assign a fingerprint slot."},
        {"WRONG_ENROLLMENT_STEP", "Enrollment step does not agree with the server."}
    };
    if (!reason || !*reason) return "Request denied. Please try again.";
    for (const auto &item : translations)
        if (strcmp(reason, item.code) == 0) return item.text;
    return reason; // Preserve other validated local/server reasons, within limits.
}

struct Pages {
    char text[MAX_PAGES][PAGE_BYTES] = {};
    size_t count = 0;
};

inline Pages format(const char *reason) {
    Pages pages{};
    char clean[MAX_INPUT + 1] = {};
    const char *message = friendly(reason);
    size_t n = 0;
    // Literal input never supplies Nextion escapes. Collapse whitespace and
    // replace command delimiters/non-ASCII characters before adding OUR newlines.
    for (size_t i = 0; message[i] && i < MAX_INPUT; ++i) {
        const unsigned char c = static_cast<unsigned char>(message[i]);
        const char safe = c >= 32 && c <= 126 && c != '"' && c != '\\'
                        ? (c == '_' ? ' ' : static_cast<char>(c)) : ' ';
        if (safe == ' ' && (n == 0 || clean[n-1] == ' ')) continue;
        clean[n++] = safe;
    }
    while (n && clean[n-1] == ' ') clean[--n] = '\0';
    clean[n] = '\0';
    if (!n) { strcpy(clean, "Request denied."); n = strlen(clean); }
    // Calls in this project are bounded to <=80 bytes; explicitly mark a longer
    // future reason instead of allowing an overrun or an unmarked truncation.
    if (strlen(message) > MAX_INPUT && n >= 3) {
        clean[n-3] = '.'; clean[n-2] = '.'; clean[n-1] = '.';
    }
    size_t pos = 0;
    while (pos < n && pages.count < MAX_PAGES) {
        char *page = pages.text[pages.count++];
        size_t used = 0;
        for (size_t line = 0; line < LINES_PER_PAGE && pos < n; ++line) {
            while (pos < n && clean[pos] == ' ') ++pos;
            if (pos == n) break;
            const size_t start = pos;
            size_t end = start, pixels = 0, lastSpace = static_cast<size_t>(-1);
            while (end < n && pixels + advance(clean[end]) <= LINE_PIXELS) {
                pixels += advance(clean[end]);
                if (clean[end] == ' ') lastSpace = end;
                ++end;
            }
            // Prefer a word boundary; split an oversized token when necessary.
            if (end < n && lastSpace != static_cast<size_t>(-1) && lastSpace > start)
                end = lastSpace;
            if (end == start) ++end; // Defensive forward progress.
            size_t trimmed = end;
            while (trimmed > start && clean[trimmed-1] == ' ') --trimmed;
            if (used) page[used++] = '\n';
            const size_t len = trimmed - start;
            memcpy(page + used, clean + start, len); used += len;
            page[used] = '\0';
            pos = end;
        }
    }
    if (!pages.count) { pages.count = 1; strcpy(pages.text[0], "Request denied."); }
    if (pages.count > 1) {
        for (size_t i = 0; i < pages.count; ++i) {
            const size_t used = strlen(pages.text[i]);
            snprintf(pages.text[i] + used, PAGE_BYTES - used, "\n(%u/%u)",
                     static_cast<unsigned>(i + 1), static_cast<unsigned>(pages.count));
        }
    }
    return pages;
}

// Nextion uses the ASCII escape \\r for a rendered line break. Do not send the
// literal newline through the general setText(), which intentionally strips it.
inline bool encode(const char *page, char *out, size_t capacity) {
    if (!out || !capacity) return false;
    out[0] = '\0';
    if (!page) return false;
    size_t used = 0;
    for (size_t i = 0; page[i]; ++i) {
        if (page[i] == '\n') {
            if (used + 2 >= capacity) return false;
            out[used++] = '\\'; out[used++] = 'r';
        } else {
            if (used + 1 >= capacity) return false;
            const unsigned char c = static_cast<unsigned char>(page[i]);
            out[used++] = c >= 32 && c <= 126 && c != '"' && c != '\\'
                        ? static_cast<char>(c) : ' ';
        }
    }
    out[used] = '\0'; return true;
}
} // namespace FailureDisplay
