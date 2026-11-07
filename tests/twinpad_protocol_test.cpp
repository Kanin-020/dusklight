// Tests de host del adaptador Twinpad: wire (base64url, NDJSON, parseo
// estricto de input_report) y mapeo del pad. Ninguno necesita el juego, el
// renderer ni un dispositivo — esa es justo la razón por la que el contrato y
// el mapeo viven separados del código de Aurora.
//
// Contrato de referencia (repo Twinpad): SPEC-006 §4/§7, SPEC-004 §4.

#include "dusk/twinpad/Channels/TwinpadPadMap.h"
#include "dusk/twinpad/Core/TwinpadProtocol.h"
#include "dusk/twinpad/Video/TwinpadH264.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using dusk::twinpad::Base64UrlEncode;
using dusk::twinpad::BuildAnnounceLine;
using dusk::twinpad::BuildFrameLine;
using dusk::twinpad::DecodeBase64Url;
using dusk::twinpad::H264Encoder;
using dusk::twinpad::IsForceIdrLine;
using dusk::twinpad::InputReport;
using dusk::twinpad::LineBuffer;
using dusk::twinpad::mapInput;
using dusk::twinpad::PadState;
using dusk::twinpad::ParseInputReport;

constexpr char kValidReport[] =
    "{\"type\":\"input_report\",\"seq\":7,\"stick_lx\":-64,\"stick_ly\":100,"
    "\"stick_rx\":0,\"stick_ry\":0,\"dpad\":8,\"buttons\":258,"
    "\"trigger_l2\":0,\"trigger_r2\":255,\"touch_x\":0,\"touch_y\":0,"
    "\"touch_pressed\":false}";

std::string encode(const std::string& text) {
    return Base64UrlEncode(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

TEST(TwinpadBase64Url, MatchesKnownVectors) {
    // RFC 4648 §10 con el alfabeto URL y sin relleno.
    EXPECT_EQ(encode("f"), "Zg");
    EXPECT_EQ(encode("fo"), "Zm8");
    EXPECT_EQ(encode("foo"), "Zm9v");
    EXPECT_EQ(encode("foob"), "Zm9vYg");
    EXPECT_EQ(encode("fooba"), "Zm9vYmE");
    EXPECT_EQ(encode("foobar"), "Zm9vYmFy");
}

TEST(TwinpadBase64Url, SurvivesNonAsciiBytes) {
    const std::vector<uint8_t> raw{0x00, 0xFF, 0x10, 0x80, 0x7F};
    std::vector<uint8_t> round;
    ASSERT_TRUE(DecodeBase64Url(Base64UrlEncode(raw.data(), raw.size()), round));
    EXPECT_EQ(round, raw);
}

TEST(TwinpadBase64Url, RejectsMalformedInput) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(DecodeBase64Url("ab+c", out));   // '+' no es alfabeto URL
    EXPECT_FALSE(DecodeBase64Url("ab=", out));    // padding no admitido
    EXPECT_FALSE(DecodeBase64Url("abcde", out));  // tamaño 4n+1
}

TEST(TwinpadAnnounce, IsTheExactProtocolLine) {
    EXPECT_EQ(BuildAnnounceLine(),
        "{\"type\":\"game_announce\",\"game_id\":\"dusklight\",\"game_name\":\"Dusklight\","
        "\"capabilities\":[\"second_screen\",\"input_consumption\"]}");
}

TEST(TwinpadFrame, CarriesH264AndMetadata) {
    // SPEC-003 §4: el servidor solo relaya H.264, y el cliente necesita el
    // timestamp de captura y el flag de keyframe.
    EXPECT_EQ(BuildFrameLine(3, 640, 360, 123456, true, "AAAA"),
        "{\"type\":\"game_frame\",\"gfn\":3,\"w\":640,\"h\":360,\"fmt\":\"h264\","
        "\"ts_capture_us\":123456,\"idr\":true,\"data\":\"AAAA\"}");
    EXPECT_EQ(BuildFrameLine(0, 16, 16, 0, false, "B"),
        "{\"type\":\"game_frame\",\"gfn\":0,\"w\":16,\"h\":16,\"fmt\":\"h264\","
        "\"ts_capture_us\":0,\"idr\":false,\"data\":\"B\"}");
}

TEST(TwinpadFrame, RecognisesOnlyTheCanonicalForceIdrLine) {
    EXPECT_TRUE(IsForceIdrLine("{\"type\":\"video_force_idr\"}"));
    EXPECT_FALSE(IsForceIdrLine("{\"type\":\"video_force_idr\",\"extra\":1}"));
    EXPECT_FALSE(IsForceIdrLine("{\"type\":\"game_frame\"}"));
    EXPECT_FALSE(IsForceIdrLine(""));
}

TEST(TwinpadH264, FirstFrameIsAnnexBWithSpsPpsAndIdr) {
    constexpr uint32_t width = 64;
    constexpr uint32_t height = 64;
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4, 0);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* pixel = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            pixel[0] = static_cast<uint8_t>(x * 4);
            pixel[1] = static_cast<uint8_t>(y * 4);
            pixel[2] = 0x40;
            pixel[3] = 0xFF;
        }
    }

    H264Encoder encoder;
    std::vector<uint8_t> annexB;
    uint64_t timestampUs = 0;
    bool idr = false;
    ASSERT_TRUE(encoder.encode(rgba.data(), width, height, true, annexB, timestampUs, idr));
    EXPECT_TRUE(idr);
    EXPECT_EQ(encoder.width(), width);
    EXPECT_EQ(encoder.height(), height);

    // Annex B: el flujo empieza con un start code de 4 bytes.
    ASSERT_GE(annexB.size(), 5u);
    EXPECT_EQ(annexB[0], 0u);
    EXPECT_EQ(annexB[1], 0u);
    EXPECT_EQ(annexB[2], 0u);
    EXPECT_EQ(annexB[3], 1u);

    // Un IDR utilizable lleva SPS (7) y PPS (8) delante del slice (5).
    std::vector<int> types;
    for (size_t index = 0; index + 4 < annexB.size(); ++index) {
        if (annexB[index] == 0 && annexB[index + 1] == 0 && annexB[index + 2] == 0 &&
            annexB[index + 3] == 1) {
            types.push_back(annexB[index + 4] & 0x1F);
        }
    }
    ASSERT_FALSE(types.empty());
    EXPECT_NE(std::find(types.begin(), types.end(), 7), types.end());
    EXPECT_NE(std::find(types.begin(), types.end(), 8), types.end());
    EXPECT_NE(std::find(types.begin(), types.end(), 5), types.end());
}

TEST(TwinpadH264, FollowUpFrameIsNotAnIdrAndKeepsTimestampsMonotonic) {
    constexpr uint32_t width = 32;
    constexpr uint32_t height = 32;
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4, 0x80);

    H264Encoder encoder;
    std::vector<uint8_t> annexB;
    uint64_t firstTs = 0;
    bool firstIdr = false;
    ASSERT_TRUE(encoder.encode(rgba.data(), width, height, true, annexB, firstTs, firstIdr));

    uint64_t secondTs = 0;
    bool secondIdr = false;
    ASSERT_TRUE(encoder.encode(rgba.data(), width, height, false, annexB, secondTs, secondIdr));
    EXPECT_FALSE(secondIdr);
    EXPECT_GE(secondTs, firstTs);
}

TEST(TwinpadH264, RejectsDimensionsOutsideThePanelContract) {
    std::vector<uint8_t> tiny(8 * 8 * 4, 0);
    H264Encoder encoder;
    std::vector<uint8_t> annexB;
    uint64_t timestampUs = 0;
    bool idr = false;
    // < 16 (SPEC-003 §4) y > 3840 de ancho no son panel válido.
    EXPECT_FALSE(encoder.encode(tiny.data(), 8, 8, true, annexB, timestampUs, idr));
    EXPECT_TRUE(annexB.empty());
    EXPECT_FALSE(encoder.encode(tiny.data(), 3842, 64, true, annexB, timestampUs, idr));
    EXPECT_FALSE(encoder.encode(nullptr, 64, 64, true, annexB, timestampUs, idr));
}

TEST(TwinpadLineBuffer, SplitsFramesAndStripsCarriageReturn) {
    LineBuffer buffer;
    buffer.append("one\ntw", 6);
    std::string line;
    ASSERT_TRUE(buffer.next(line));
    EXPECT_EQ(line, "one");
    EXPECT_FALSE(buffer.next(line));

    buffer.append("o\r\nthree\n", 9);
    ASSERT_TRUE(buffer.next(line));
    EXPECT_EQ(line, "two");
    ASSERT_TRUE(buffer.next(line));
    EXPECT_EQ(line, "three");
    EXPECT_FALSE(buffer.next(line));

    buffer.append("partial", 7);
    buffer.clear();
    buffer.append("x\n", 2);
    ASSERT_TRUE(buffer.next(line));
    EXPECT_EQ(line, "x");
}

TEST(TwinpadInputReport, AcceptsCompleteReport) {
    InputReport report;
    ASSERT_TRUE(ParseInputReport(kValidReport, report));
    EXPECT_EQ(report.seq, 7u);
    EXPECT_EQ(report.stickLx, -64);
    EXPECT_EQ(report.stickLy, 100);
    EXPECT_EQ(report.dpad, 8);
    EXPECT_EQ(report.buttons, 258);
    EXPECT_EQ(report.triggerR2, 255);
    EXPECT_FALSE(report.touchPressed);
    EXPECT_FALSE(report.hasMotion);
}

TEST(TwinpadInputReport, AcceptsMotionAndMarksIt) {
    std::string line = kValidReport;
    line.insert(line.size() - 1,
        ",\"motion\":{\"timestamp_us\":12,\"accel\":[0.0,9.8,-0.5],\"gyro\":[0,0,0]}");
    InputReport report;
    ASSERT_TRUE(ParseInputReport(line, report));
    EXPECT_TRUE(report.hasMotion);
}

TEST(TwinpadInputReport, RejectsUnknownField) {
    std::string line = kValidReport;
    line.insert(line.size() - 1, ",\"extra\":1");
    InputReport report;
    // deny_unknown_fields del servidor: aceptarla aquí la haría morir allá.
    EXPECT_FALSE(ParseInputReport(line, report));
}

TEST(TwinpadInputReport, RejectsMissingField) {
    InputReport report;
    EXPECT_FALSE(ParseInputReport(
        "{\"type\":\"input_report\",\"seq\":1,\"stick_lx\":0,\"stick_ly\":0,"
        "\"stick_rx\":0,\"stick_ry\":0,\"dpad\":8,\"buttons\":0,\"trigger_l2\":0,"
        "\"trigger_r2\":0,\"touch_x\":0,\"touch_y\":0}",
        report));
}

TEST(TwinpadInputReport, RejectsOutOfRangeValues) {
    InputReport report;
    std::string line = kValidReport;
    const auto replace = [&line](const std::string& from, const std::string& to) {
        const size_t at = line.find(from);
        ASSERT_NE(at, std::string::npos);
        line.replace(at, from.size(), to);
    };

    line = kValidReport;
    replace("\"dpad\":8", "\"dpad\":9");
    EXPECT_FALSE(ParseInputReport(line, report));

    line = kValidReport;
    replace("\"stick_lx\":-64", "\"stick_lx\":128");
    EXPECT_FALSE(ParseInputReport(line, report));

    line = kValidReport;
    replace("\"seq\":7", "\"seq\":1.5");
    EXPECT_FALSE(ParseInputReport(line, report));

    line = kValidReport;
    replace("\"touch_pressed\":false", "\"touch_pressed\":\"no\"");
    EXPECT_FALSE(ParseInputReport(line, report));
}

TEST(TwinpadInputReport, RejectsOtherMessageTypesAndTrailingBytes) {
    InputReport report;
    std::string line = kValidReport;
    line.replace(line.find("input_report"), 12, "game_frame");
    EXPECT_FALSE(ParseInputReport(line, report));
    EXPECT_FALSE(ParseInputReport(std::string(kValidReport) + "{}", report));
    EXPECT_FALSE(ParseInputReport("", report));
}

TEST(TwinpadPadMap, MapsButtonsAndTriggers) {
    InputReport report;
    ASSERT_TRUE(ParseInputReport(kValidReport, report));
    // buttons 258 = 0x102 = START (bit 8) + B (bit 1); R2 al máximo.
    PadState pad = mapInput(report);
    EXPECT_TRUE(pad.b);
    EXPECT_TRUE(pad.start);
    EXPECT_FALSE(pad.a);
    EXPECT_TRUE(pad.r);
    EXPECT_FLOAT_EQ(pad.triggerRight, 1.0f);
    EXPECT_TRUE(pad.active());

    InputReport extra;
    extra.buttons = dusk::twinpad::kButtonExtra1;
    pad = mapInput(extra);
    EXPECT_TRUE(pad.z);  // el teléfono no tiene Z: EXTRA_1 la representa
}

TEST(TwinpadPadMap, MapsDpadAndInvertsStickY) {
    InputReport report;
    ASSERT_TRUE(ParseInputReport(kValidReport, report));
    // stick_ly = 100 (Android Y+ = abajo) => stickY negativo en GameCube.
    PadState pad = mapInput(report);
    EXPECT_LT(pad.stickY, 0.0f);
    EXPECT_FALSE(pad.up);
    EXPECT_FALSE(pad.down);

    report.dpad = 0;
    pad = mapInput(report);
    EXPECT_TRUE(pad.up);
    EXPECT_FALSE(pad.right);

    report.dpad = 5;
    pad = mapInput(report);
    EXPECT_TRUE(pad.down);
    EXPECT_TRUE(pad.left);

    InputReport neutral;
    EXPECT_FALSE(mapInput(neutral).active());
}

}  // namespace
