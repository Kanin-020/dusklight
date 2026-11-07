#include "dusk/twinpad/Core/TwinpadProtocol.h"

#include <algorithm>
#include <cstdlib>

namespace dusk::twinpad {
namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int decodeSymbol(char symbol) {
    if (symbol >= 'A' && symbol <= 'Z') {
        return symbol - 'A';
    }
    if (symbol >= 'a' && symbol <= 'z') {
        return symbol - 'a' + 26;
    }
    if (symbol >= '0' && symbol <= '9') {
        return symbol - '0' + 52;
    }
    if (symbol == '-') {
        return 62;
    }
    if (symbol == '_') {
        return 63;
    }
    return -1;
}

// Lector JSON mínimo y estricto: los mensajes del wire son objetos planos de
// números y booleanos (más `motion`, que se salta entero). No se usa un parser
// genérico a propósito — así el conjunto de campos aceptados es exactamente el
// de SPEC-004 §4 y no se cuela nada.
class Scanner {
public:
    explicit Scanner(std::string_view text) : mText(text) {}

    void skipSpace() {
        while (mPos < mText.size()) {
            const char c = mText[mPos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++mPos;
        }
    }

    bool atEnd() {
        skipSpace();
        return mPos >= mText.size();
    }

    bool accept(char expected) {
        skipSpace();
        if (mPos >= mText.size() || mText[mPos] != expected) {
            return false;
        }
        ++mPos;
        return true;
    }

    bool parseString(std::string& out) {
        if (!accept('"')) {
            return false;
        }
        out.clear();
        while (mPos < mText.size()) {
            const char c = mText[mPos++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (mPos >= mText.size()) {
                return false;
            }
            const char escaped = mText[mPos++];
            switch (escaped) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (mPos + 4 > mText.size()) {
                    return false;
                }
                unsigned value = 0;
                for (int index = 0; index < 4; ++index) {
                    const char digit = mText[mPos++];
                    value <<= 4;
                    if (digit >= '0' && digit <= '9') {
                        value |= static_cast<unsigned>(digit - '0');
                    } else if (digit >= 'a' && digit <= 'f') {
                        value |= static_cast<unsigned>(digit - 'a' + 10);
                    } else if (digit >= 'A' && digit <= 'F') {
                        value |= static_cast<unsigned>(digit - 'A' + 10);
                    } else {
                        return false;
                    }
                }
                // Las claves y valores del wire son ASCII; un escape mayor se
                // acepta en forma pero no se reconstruye (no hay campo que lo use).
                if (value < 0x80) {
                    out.push_back(static_cast<char>(value));
                }
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool parseInteger(long long& out) {
        skipSpace();
        const size_t start = mPos;
        if (mPos < mText.size() && (mText[mPos] == '-' || mText[mPos] == '+')) {
            ++mPos;
        }
        const size_t digits = mPos;
        while (mPos < mText.size() && mText[mPos] >= '0' && mText[mPos] <= '9') {
            ++mPos;
        }
        if (mPos == digits || mPos - start > 20) {
            return false;
        }
        // Un entero del wire nunca continúa con fracción ni exponente; dejarlo
        // pasar convertiría "seq": 1.5 en un reporte válido por redondeo.
        if (mPos < mText.size()) {
            const char c = mText[mPos];
            if (c == '.' || c == 'e' || c == 'E') {
                return false;
            }
        }
        const std::string token(mText.substr(start, mPos - start));
        out = std::strtoll(token.c_str(), nullptr, 10);
        return true;
    }

    bool parseBool(bool& out) {
        skipSpace();
        if (mText.compare(mPos, 4, "true") == 0) {
            mPos += 4;
            out = true;
            return true;
        }
        if (mText.compare(mPos, 5, "false") == 0) {
            mPos += 5;
            out = false;
            return true;
        }
        return false;
    }

    bool skipValue(int depth) {
        if (depth > 8) {
            return false;
        }
        skipSpace();
        if (mPos >= mText.size()) {
            return false;
        }
        const char c = mText[mPos];
        if (c == '{') {
            ++mPos;
            if (accept('}')) {
                return true;
            }
            for (;;) {
                std::string key;
                if (!parseString(key) || !accept(':')) {
                    return false;
                }
                if (!skipValue(depth + 1)) {
                    return false;
                }
                if (accept('}')) {
                    return true;
                }
                if (!accept(',')) {
                    return false;
                }
            }
        }
        if (c == '[') {
            ++mPos;
            if (accept(']')) {
                return true;
            }
            for (;;) {
                if (!skipValue(depth + 1)) {
                    return false;
                }
                if (accept(']')) {
                    return true;
                }
                if (!accept(',')) {
                    return false;
                }
            }
        }
        if (c == '"') {
            std::string ignored;
            return parseString(ignored);
        }
        if (c == 't' || c == 'f') {
            bool ignored = false;
            return parseBool(ignored);
        }
        if (mText.compare(mPos, 4, "null") == 0) {
            mPos += 4;
            return true;
        }
        long long integer = 0;
        if (parseInteger(integer)) {
            return true;
        }
        // Números no enteros (solo aparecen dentro de `motion`): token libre.
        const size_t start = mPos;
        while (mPos < mText.size()) {
            const char n = mText[mPos];
            if ((n >= '0' && n <= '9') || n == '-' || n == '+' || n == '.' || n == 'e' ||
                n == 'E') {
                ++mPos;
            } else {
                break;
            }
        }
        return mPos > start;
    }

private:
    std::string_view mText;
    size_t mPos = 0;
};

bool readInteger(Scanner& scanner, long long low, long long high, long long& out) {
    long long value = 0;
    if (!scanner.parseInteger(value)) {
        return false;
    }
    if (value < low || value > high) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

std::string BuildAnnounceLine() {
    return "{\"type\":\"game_announce\",\"game_id\":\"" + std::string(kGameId) +
        "\",\"game_name\":\"" + std::string(kGameName) +
        "\",\"capabilities\":[\"second_screen\",\"input_consumption\"]}";
}

std::string BuildFrameLine(uint64_t gfn, uint16_t width, uint16_t height,
    uint64_t timestampUs, bool idr, std::string_view annexBBase64Url) {
    std::string line;
    line.reserve(annexBBase64Url.size() + 128);
    line += "{\"type\":\"game_frame\",\"gfn\":";
    line += std::to_string(gfn);
    line += ",\"w\":";
    line += std::to_string(width);
    line += ",\"h\":";
    line += std::to_string(height);
    line += ",\"fmt\":\"h264\",\"ts_capture_us\":";
    line += std::to_string(timestampUs);
    line += idr ? ",\"idr\":true,\"data\":\"" : ",\"idr\":false,\"data\":\"";
    line += annexBBase64Url;
    line += "\"}";
    return line;
}

bool IsForceIdrLine(std::string_view line) {
    // El servidor emite esta línea en su forma canónica y sin campos extra.
    return line == "{\"type\":\"video_force_idr\"}";
}

std::string Base64UrlEncode(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t index = 0;
    while (index + 3 <= size) {
        const uint32_t chunk = (static_cast<uint32_t>(data[index]) << 16) |
            (static_cast<uint32_t>(data[index + 1]) << 8) |
            static_cast<uint32_t>(data[index + 2]);
        out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(chunk >> 6) & 0x3F]);
        out.push_back(kBase64Alphabet[chunk & 0x3F]);
        index += 3;
    }
    const size_t remaining = size - index;
    if (remaining == 1) {
        const uint32_t chunk = static_cast<uint32_t>(data[index]) << 16;
        out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
    } else if (remaining == 2) {
        const uint32_t chunk = (static_cast<uint32_t>(data[index]) << 16) |
            (static_cast<uint32_t>(data[index + 1]) << 8);
        out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(chunk >> 6) & 0x3F]);
    }
    return out;
}

bool DecodeBase64Url(std::string_view text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.size() % 4 == 1) {
        return false;
    }
    uint32_t accumulator = 0;
    int bits = 0;
    for (const char symbol : text) {
        const int value = decodeSymbol(symbol);
        if (value < 0) {
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return true;
}

void LineBuffer::append(const char* data, size_t size) {
    m_buffer.append(data, size);
}

bool LineBuffer::next(std::string& line) {
    const size_t newline = m_buffer.find('\n');
    if (newline == std::string::npos) {
        return false;
    }
    line.assign(m_buffer, 0, newline);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    m_buffer.erase(0, newline + 1);
    return true;
}

void LineBuffer::clear() {
    m_buffer.clear();
}

bool ParseInputReport(std::string_view line, InputReport& out) {
    Scanner scanner(line);
    if (!scanner.accept('{')) {
        return false;
    }

    InputReport report;
    bool haveType = false;
    bool haveSeq = false;
    bool haveStickLx = false;
    bool haveStickLy = false;
    bool haveStickRx = false;
    bool haveStickRy = false;
    bool haveDpad = false;
    bool haveButtons = false;
    bool haveTriggerL2 = false;
    bool haveTriggerR2 = false;
    bool haveTouchX = false;
    bool haveTouchY = false;
    bool haveTouchPressed = false;

    bool first = true;
    for (;;) {
        if (scanner.accept('}')) {
            break;
        }
        if (!first && !scanner.accept(',')) {
            return false;
        }
        first = false;

        std::string key;
        if (!scanner.parseString(key) || !scanner.accept(':')) {
            return false;
        }

        long long integer = 0;
        if (key == "type") {
            std::string type;
            if (!scanner.parseString(type) || type != "input_report") {
                return false;
            }
            haveType = true;
        } else if (key == "seq") {
            if (!readInteger(scanner, 0, 4294967295LL, integer)) {
                return false;
            }
            report.seq = static_cast<uint32_t>(integer);
            haveSeq = true;
        } else if (key == "stick_lx") {
            if (!readInteger(scanner, -127, 127, integer)) {
                return false;
            }
            report.stickLx = static_cast<int8_t>(integer);
            haveStickLx = true;
        } else if (key == "stick_ly") {
            if (!readInteger(scanner, -127, 127, integer)) {
                return false;
            }
            report.stickLy = static_cast<int8_t>(integer);
            haveStickLy = true;
        } else if (key == "stick_rx") {
            if (!readInteger(scanner, -127, 127, integer)) {
                return false;
            }
            report.stickRx = static_cast<int8_t>(integer);
            haveStickRx = true;
        } else if (key == "stick_ry") {
            if (!readInteger(scanner, -127, 127, integer)) {
                return false;
            }
            report.stickRy = static_cast<int8_t>(integer);
            haveStickRy = true;
        } else if (key == "dpad") {
            if (!readInteger(scanner, 0, kDpadNeutral, integer)) {
                return false;
            }
            report.dpad = static_cast<uint8_t>(integer);
            haveDpad = true;
        } else if (key == "buttons") {
            if (!readInteger(scanner, 0, 65535, integer)) {
                return false;
            }
            report.buttons = static_cast<uint16_t>(integer);
            haveButtons = true;
        } else if (key == "trigger_l2") {
            if (!readInteger(scanner, 0, 255, integer)) {
                return false;
            }
            report.triggerL2 = static_cast<uint8_t>(integer);
            haveTriggerL2 = true;
        } else if (key == "trigger_r2") {
            if (!readInteger(scanner, 0, 255, integer)) {
                return false;
            }
            report.triggerR2 = static_cast<uint8_t>(integer);
            haveTriggerR2 = true;
        } else if (key == "touch_x") {
            if (!readInteger(scanner, 0, 65535, integer)) {
                return false;
            }
            report.touchX = static_cast<uint16_t>(integer);
            haveTouchX = true;
        } else if (key == "touch_y") {
            if (!readInteger(scanner, 0, 65535, integer)) {
                return false;
            }
            report.touchY = static_cast<uint16_t>(integer);
            haveTouchY = true;
        } else if (key == "touch_pressed") {
            bool pressed = false;
            if (!scanner.parseBool(pressed)) {
                return false;
            }
            report.touchPressed = pressed;
            haveTouchPressed = true;
        } else if (key == "motion") {
            // Se acepta su forma pero no se retiene: el giro no tiene consumidor.
            if (!scanner.skipValue(0)) {
                return false;
            }
            report.hasMotion = true;
        } else {
            // deny_unknown_fields: el servidor cerraría el canal por esto.
            return false;
        }
    }

    if (!scanner.atEnd()) {
        return false;
    }
    if (!haveType || !haveSeq || !haveStickLx || !haveStickLy || !haveStickRx || !haveStickRy ||
        !haveDpad || !haveButtons || !haveTriggerL2 || !haveTriggerR2 || !haveTouchX ||
        !haveTouchY || !haveTouchPressed) {
        return false;
    }

    out = report;
    return true;
}

bool ParseVideoConfig(std::string_view line, VideoConfig& out) {
    Scanner scanner(line);
    VideoConfig config;
    bool haveType = false;
    bool haveWidth = false;
    bool haveHeight = false;
    bool haveAnyValue = false;

    if (!scanner.accept('{') || scanner.accept('}')) {
        return false;
    }
    for (;;) {
        std::string key;
        if (!scanner.parseString(key) || !scanner.accept(':')) {
            return false;
        }
        if (key == "type") {
            std::string type;
            if (!scanner.parseString(type) || type != "video_config" || haveType) {
                return false;
            }
            haveType = true;
        } else if (key == "bitrate_kbps") {
            long long integer = 0;
            if (!scanner.parseInteger(integer) || integer < 0) {
                return false;
            }
            // Mismo clamp que el Servidor y Cemu (SPEC-003 §4): 1.000–30.000.
            config.bitrateKbps = static_cast<int>(std::clamp<long long>(integer, 1000, 30000));
            haveAnyValue = true;
        } else if (key == "w") {
            long long integer = 0;
            if (!scanner.parseInteger(integer) || integer < 0 || integer > 3840) {
                return false;
            }
            config.width = static_cast<uint32_t>(integer);
            haveWidth = true;
            haveAnyValue = true;
        } else if (key == "h") {
            long long integer = 0;
            if (!scanner.parseInteger(integer) || integer < 0 || integer > 2160) {
                return false;
            }
            config.height = static_cast<uint32_t>(integer);
            haveHeight = true;
            haveAnyValue = true;
        } else if (!scanner.skipValue(0)) {
            // Campo ajeno (p. ej. `fps` de una app anterior a CR-029).
            return false;
        }
        if (scanner.accept('}')) {
            break;
        }
        if (!scanner.accept(',')) {
            return false;
        }
    }

    if (!scanner.atEnd() || !haveType || !haveAnyValue) {
        return false;
    }
    // `w` y `h` viajan juntos; `0/0` es el centinela de nativo (CR-074).
    if (haveWidth != haveHeight) {
        return false;
    }
    if (haveWidth) {
        const bool nativeWidth = config.width == 0;
        const bool nativeHeight = config.height == 0;
        if (nativeWidth != nativeHeight) {
            return false;
        }
        if (!nativeWidth) {
            // Dimensiones pares: I420 no tiene muestreo para impares.
            config.width = std::clamp<uint32_t>(config.width, 16, 3840) & ~1u;
            config.height = std::clamp<uint32_t>(config.height, 16, 2160) & ~1u;
        }
    }

    out = config;
    return true;
}

}  // namespace dusk::twinpad
