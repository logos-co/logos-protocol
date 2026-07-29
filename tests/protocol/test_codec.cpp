// The canonical LIDL <-> JSON codec (cpp/logos_codec.h).
//
// This is the single implementation of an encoding that previously existed six
// times with divergent semantics, so the tests pin the CONTRACT, not just the
// happy path: which shapes decode, which throw, and that bytes stay tagged at
// any nesting depth (the case that used to compile and then either throw at
// call time or emit untagged number arrays).

#include <gtest/gtest.h>

#include "logos_codec.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using nlohmann::json;

namespace {

const std::vector<uint8_t> kSpan = {0x00, 0x7f, 0x80, 0xff};  // spans the UTF-8 boundary
const char* kSpanB64 = "AH-A_w";

} // namespace

// ── leaves ────────────────────────────────────────────────────────────────

TEST(Codec, ScalarsRoundTrip)
{
    EXPECT_EQ(logos::fromJson<std::string>(logos::toJson(std::string("hi"))), "hi");
    EXPECT_EQ(logos::fromJson<int64_t>(logos::toJson<int64_t>(-7)), -7);
    EXPECT_EQ(logos::fromJson<uint64_t>(logos::toJson<uint64_t>(9u)), 9u);
    EXPECT_DOUBLE_EQ(logos::fromJson<double>(logos::toJson(1.5)), 1.5);
    EXPECT_TRUE(logos::fromJson<bool>(logos::toJson(true)));
}

// The codec itself is width-agnostic — it is a library, usable from any C++ that
// has an integer. The 64-bit-only rule is a MODULE CONTRACT enforced by the
// cdylib gate (logos-cpp-sdk), which rejects a uint32_t parameter and tells the
// author to write uint64_t. Keeping those concerns apart means internal callers
// are not forced to widen, while a published module interface cannot disagree
// with its declared C++ type about range.
TEST(Codec, CodecItselfIsWidthAgnostic)
{
    EXPECT_EQ(logos::fromJson<int>(logos::toJson(42)), 42);
    EXPECT_EQ(logos::fromJson<uint32_t>(logos::toJson<uint32_t>(42u)), 42u);
    EXPECT_EQ(logos::fromJson<short>(logos::toJson<short>(-3)), -3);
    EXPECT_EQ(logos::fromJson<size_t>(logos::toJson<size_t>(7u)), 7u);
    EXPECT_EQ(logos::fromJson<uint8_t>(logos::toJson<uint8_t>(255u)), 255u);
    EXPECT_FLOAT_EQ(logos::fromJson<float>(logos::toJson(0.5f)), 0.5f);
}

// uint64 above 2^63 must survive: nlohmann keeps unsigned and signed apart, so
// the value round-trips rather than wrapping through int64.
TEST(Codec, LargeUnsignedSurvives)
{
    const uint64_t big = 18446744073709551615ull;
    EXPECT_EQ(logos::fromJson<uint64_t>(logos::toJson(big)), big);
}

// A whole-valued double may arrive as an integer (JSON has one number type);
// float64 accepts it rather than failing a strict is_number_float check.
TEST(Codec, IntegralJsonNumberDecodesAsFloat64)
{
    EXPECT_DOUBLE_EQ(logos::fromJson<double>(json(2)), 2.0);
}

// ── bytes ─────────────────────────────────────────────────────────────────

TEST(Codec, BytesUseTheTaggedForm)
{
    const json j = logos::toJson(kSpan);
    ASSERT_TRUE(logos::isTaggedBytes(j));
    EXPECT_EQ(j["_bytes"], kSpanB64);
    EXPECT_EQ(logos::fromJson<std::vector<uint8_t>>(j), kSpan);
}

TEST(Codec, EmptyBytesRoundTrip)
{
    const std::vector<uint8_t> empty;
    const json j = logos::toJson(empty);
    EXPECT_EQ(j["_bytes"], "");
    EXPECT_TRUE(logos::fromJson<std::vector<uint8_t>>(j).empty());
}

// The size()==1 check: a map that merely CONTAINS a "_bytes" entry is a map, not
// bytes. The lp helper omitted this check while the plain wire enforced it, so
// the same value decoded two ways depending on the layer.
TEST(Codec, MultiKeyObjectIsNotBytes)
{
    const json j = json{{"_bytes", "AA"}, {"x", 1}};
    EXPECT_FALSE(logos::isTaggedBytes(j));
    EXPECT_THROW(logos::bytesFromJson(j), logos::CodecError);
}

// Padding tolerance: a padded encoder on the other side used to yield correct
// bytes in one copy, empty in another and None in Rust.
TEST(Codec, PaddedBase64Decodes)
{
    EXPECT_EQ(logos::bytesFromJson(json{{"_bytes", "AH-A_w=="}}), kSpan);
}

// The documented lenient set, for provider-side argument decoding only.
TEST(Codec, LenientBytesAcceptsWhatOtherLayersProduce)
{
    EXPECT_EQ(logos::bytesFromJsonLenient(json("ab")), (std::vector<uint8_t>{'a', 'b'}));
    EXPECT_EQ(logos::bytesFromJsonLenient(json(12)), (std::vector<uint8_t>{'1', '2'}));
    EXPECT_EQ(logos::bytesFromJsonLenient(json::array({0, 255})),
              (std::vector<uint8_t>{0x00, 0xff}));
    // ...and the canonical form still wins over the array reading.
    EXPECT_EQ(logos::bytesFromJsonLenient(logos::toJson(kSpan)), kSpan);
}

// ── composition ───────────────────────────────────────────────────────────

TEST(Codec, TypedScalarArraysRoundTripIncludingEmpty)
{
    const std::vector<int64_t> ints = {1, -2, 3};
    EXPECT_EQ(logos::fromJson<std::vector<int64_t>>(logos::toJson(ints)), ints);

    const std::vector<std::string> strs;
    EXPECT_TRUE(logos::fromJson<std::vector<std::string>>(logos::toJson(strs)).empty());
    EXPECT_TRUE(logos::toJson(strs).is_array());
}

TEST(Codec, ListOfBytesTagsEachElement)
{
    const std::vector<std::vector<uint8_t>> list = {kSpan, {}, {0xde, 0xad}};
    const json j = logos::toJson(list);
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 3u);
    EXPECT_TRUE(logos::isTaggedBytes(j[0]));
    EXPECT_TRUE(logos::isTaggedBytes(j[1]));   // the empty element stays an element
    EXPECT_EQ(logos::fromJson<std::vector<std::vector<uint8_t>>>(j), list);
}

// The case that used to compile and then emit untagged nested number arrays.
TEST(Codec, ListOfListOfBytesTagsAtDepth)
{
    const std::vector<std::vector<std::vector<uint8_t>>> nested = {{kSpan}, {}, {{}, {0x01}}};
    const json j = logos::toJson(nested);
    ASSERT_TRUE(j.is_array());
    ASSERT_TRUE(j[0].is_array());
    EXPECT_TRUE(logos::isTaggedBytes(j[0][0]));
    EXPECT_EQ(logos::fromJson<std::vector<std::vector<std::vector<uint8_t>>>>(j), nested);
}

TEST(Codec, MapOfBytesAndMapOfListsCompose)
{
    const std::map<std::string, std::vector<uint8_t>> m = {{"a", kSpan}, {"b", {}}};
    const json j = logos::toJson(m);
    ASSERT_TRUE(j.is_object());
    EXPECT_TRUE(logos::isTaggedBytes(j["a"]));
    EXPECT_EQ((logos::fromJson<std::map<std::string, std::vector<uint8_t>>>(j)), m);

    const std::map<std::string, std::vector<std::vector<uint8_t>>> deep = {{"k", {kSpan, {}}}};
    EXPECT_EQ((logos::fromJson<std::map<std::string, std::vector<std::vector<uint8_t>>>>(
                  logos::toJson(deep))),
              deep);

    const std::unordered_map<std::string, int64_t> um = {{"n", 5}};
    EXPECT_EQ((logos::fromJson<std::unordered_map<std::string, int64_t>>(logos::toJson(um))), um);
}

// `any` stops the recursion — the value passes through byte-identically, so a
// LogosMap keeps whatever the peer sent (tagged bytes included).
TEST(Codec, AnyPassesThroughVerbatim)
{
    const json payload = json{{"nested", json{{"_bytes", kSpanB64}}}, {"n", 1}};
    EXPECT_EQ(logos::fromJson<json>(logos::toJson(payload)), payload);

    const std::vector<json> anyList = {json(1), json("s"), payload};
    EXPECT_EQ(logos::fromJson<std::vector<json>>(logos::toJson(anyList)), anyList);
}

// ── failure modes ─────────────────────────────────────────────────────────

// A shape mismatch throws with the path, rather than silently substituting a
// default. Callers turn this into a structured error; the old behaviour differed
// per layer (throw in C++, silently-empty in Rust).
TEST(Codec, MismatchThrowsWithPath)
{
    EXPECT_THROW(logos::fromJson<int64_t>(json("nope")), logos::CodecError);
    EXPECT_THROW(logos::fromJson<std::vector<int64_t>>(json("nope")), logos::CodecError);

    try {
        logos::fromJson<std::vector<std::vector<int64_t>>>(json::array({json::array({1, "x"})}));
        FAIL() << "expected CodecError";
    } catch (const logos::CodecError& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("[0][1]"), std::string::npos) << what;
    }

    try {
        logos::fromJson<std::map<std::string, int64_t>>(json{{"k", "x"}});
        FAIL() << "expected CodecError";
    } catch (const logos::CodecError& e) {
        EXPECT_NE(std::string(e.what()).find(".k"), std::string::npos) << e.what();
    }
}

// The plain wire validates frames with the strict decode: a corrupt base64 body
// must be rejected, not silently decoded to fewer bytes. Consumer-facing decodes
// stay tolerant (PaddedBase64Decodes above), so both behaviours come from one
// implementation instead of four disagreeing copies.
TEST(Codec, CheckedDecodeRejectsCorruptInput)
{
    std::vector<uint8_t> out;
    EXPECT_TRUE(logos::b64UrlDecodeChecked("AH-A_w", out));
    EXPECT_EQ(out, kSpan);

    EXPECT_TRUE(logos::b64UrlDecodeChecked("AH-A_w==", out));  // padding tolerated
    EXPECT_EQ(out, kSpan);

    EXPECT_FALSE(logos::b64UrlDecodeChecked("AH-A_w!!", out)); // stray character
    EXPECT_TRUE(out.empty());

    EXPECT_FALSE(logos::b64UrlDecodeChecked("AH-A_wQQQ??", out));
    EXPECT_FALSE(logos::b64UrlDecodeChecked("A", out));        // impossible length
}

// ---------------------------------------------------------------------------
// Adopted from logos-cpp-sdk tests/sdk/test_logos_json_bytes.cpp, which tested
// b64UrlEncode / bytesToJson back when logos_json.h carried its own copies of
// them. Those copies are gone (logos-cpp-sdk#117), so the coverage belongs with
// the canonical definitions rather than in a repo that would have to reach
// across for them.
//
// What these pin is not academic: a wrong alphabet, a stray '=', or a botched
// tail group silently corrupts every binary payload in the system, and the
// code-generation tests assert on generated source TEXT and cannot see it.
// ---------------------------------------------------------------------------

TEST(CodecBytes, RoundTripsEveryByteValue)
{
    std::vector<uint8_t> all(256);
    for (int i = 0; i < 256; ++i) all[i] = static_cast<uint8_t>(i);

    EXPECT_EQ(logos::b64UrlDecode(logos::b64UrlEncode(all)), all);
}

TEST(CodecBytes, UsesTheUrlSafeAlphabetAndOmitsPadding)
{
    // 0xFB 0xFF encodes to the two characters that differ between the standard
    // and URL-safe alphabets ('+/' vs '-_'), and a 2-byte input is where a
    // padding-emitting encoder would append '='.
    const std::string enc = logos::b64UrlEncode({0xFB, 0xFF});

    EXPECT_EQ(enc.find('+'), std::string::npos);
    EXPECT_EQ(enc.find('/'), std::string::npos);
    EXPECT_EQ(enc.find('='), std::string::npos);
}

TEST(CodecBytes, TailGroupsOfEveryLengthSurvive)
{
    for (size_t n = 0; n <= 5; ++n) {
        std::vector<uint8_t> v(n);
        for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(0xA0 + i);
        EXPECT_EQ(logos::b64UrlDecode(logos::b64UrlEncode(v)), v) << "length " << n;
    }
}

TEST(CodecBytes, EmbeddedNulSurvives)
{
    // The reason bytes are tagged at all: a plain JSON string cannot carry this.
    const std::vector<uint8_t> v{'a', 0, 'b', 0, 'c'};

    EXPECT_EQ(logos::b64UrlDecode(logos::b64UrlEncode(v)), v);
}

TEST(CodecBytes, BytesToJsonEmitsTheCanonicalTag)
{
    const nlohmann::json j = logos::bytesToJson({'h', 'i'});

    ASSERT_TRUE(j.is_object());
    EXPECT_EQ(j.size(), 1u);
    ASSERT_TRUE(j.contains("_bytes"));
    EXPECT_TRUE(j["_bytes"].is_string());
    EXPECT_TRUE(logos::isTaggedBytes(j));
}

// The decoder is deliberately tolerant of padding a peer may have emitted.
// The cdylib backend used to carry a SECOND decoder that bailed on any
// non-alphabet character, so padded input silently produced an empty vector —
// the two disagreed, and this test is what the surviving one must satisfy.
TEST(CodecBytes, DecodeAcceptsPaddedInput)
{
    EXPECT_EQ(logos::b64UrlDecode("Zm9vYg=="), (std::vector<uint8_t>{'f', 'o', 'o', 'b'}));
}
