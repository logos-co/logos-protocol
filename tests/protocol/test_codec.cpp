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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using nlohmann::json;

namespace {

const std::vector<uint8_t> kSpan = {0x00, 0x7f, 0x80, 0xff};  // spans the UTF-8 boundary
const char* kSpanB64 = "AH-A_w";

// Exactly what a generated record decoder hands a Codec for field `name`: the
// value when the key is present, a null json when it is not — the emitter writes
// `j.contains(f) ? j.at(f) : nlohmann::json()`. Reproduced here so the optional
// tests exercise the ABSENT case as it actually arrives, instead of asserting
// against a null the test made up.
json field(const json& obj, const char* name)
{
    return obj.contains(name) ? obj.at(name) : json();
}

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

// ── optionals (?T) ────────────────────────────────────────────────────────
//
// `?T` is TWO-state: a value of T, or empty. The wire rule is asymmetric on
// purpose — absent and explicit null are the SAME state on the way in, and empty
// has exactly ONE spelling on the way out — so these tests pin both halves plus
// the fact that a round trip CANONICALISES rather than preserving the input.

// The liberal half. A record field that was never sent reaches the codec as a
// null json (see `field` above), so absent and null cannot be told apart here
// even in principle — and must not be.
TEST(CodecOptional, AbsentAndExplicitNullBothDecodeToEmpty)
{
    const json sent = json{{"present", 1}, {"explicitNull", nullptr}};

    EXPECT_EQ(logos::fromJson<std::optional<int64_t>>(field(sent, "missing")), std::nullopt);
    EXPECT_EQ(logos::fromJson<std::optional<int64_t>>(field(sent, "explicitNull")), std::nullopt);
    EXPECT_EQ(logos::fromJson<std::optional<int64_t>>(field(sent, "present")),
              std::optional<int64_t>(1));

    // A bare null, whatever produced it, is the same answer.
    EXPECT_EQ(logos::fromJson<std::optional<std::string>>(json()), std::nullopt);
    EXPECT_EQ(logos::fromJson<std::optional<std::string>>(json(nullptr)), std::nullopt);
}

// The canonical half, and the layering boundary. This codec emits the POSITIONAL
// spelling (null) because it is handed a value, never a slot; omitting the key
// for a nullopt record field is the record emitter's job in logos-cpp-sdk. If
// this test ever starts asserting "nothing", key-omission has leaked one layer
// too deep — and a `to()` that can return nothing cannot fill an array position.
TEST(CodecOptional, EmptyEncodesAsNullAndRoundTripIsCanonicalising)
{
    EXPECT_TRUE(logos::toJson(std::optional<int64_t>{}).is_null());
    EXPECT_TRUE(logos::toJson(std::optional<std::string>{}).is_null());

    EXPECT_EQ(logos::fromJson<std::optional<int64_t>>(logos::toJson(std::optional<int64_t>{})),
              std::nullopt);

    // Absent in, null out: the two input spellings converge on the one output
    // spelling, which is what "canonicalising, not identity" means.
    const json absent = field(json::object(), "x");
    EXPECT_TRUE(logos::toJson(logos::fromJson<std::optional<int64_t>>(absent)).is_null());
}

TEST(CodecOptional, PresentValueRoundTripsUnchanged)
{
    const auto n = std::optional<int64_t>(-7);
    EXPECT_EQ(logos::toJson(n), json(-7));
    EXPECT_EQ(logos::fromJson<std::optional<int64_t>>(logos::toJson(n)), n);

    const auto s = std::optional<std::string>("hi");
    EXPECT_EQ(logos::toJson(s), json("hi"));
    EXPECT_EQ(logos::fromJson<std::optional<std::string>>(logos::toJson(s)), s);

    const auto b = std::optional<bool>(false);
    EXPECT_EQ(logos::toJson(b), json(false));   // false is a VALUE, not empty
    EXPECT_EQ(logos::fromJson<std::optional<bool>>(logos::toJson(b)), b);

    const auto d = std::optional<double>(1.5);
    EXPECT_EQ(logos::fromJson<std::optional<double>>(logos::toJson(d)), d);
}

// Optional widens the domain by exactly ONE inhabitant. It is not a licence to
// accept anything: a present-but-wrong-typed value throws exactly as it would in
// a required slot, with the same path. Silently mapping a bad value to empty
// would be the "silent default" this codec exists to refuse, made invisible.
TEST(CodecOptional, PresentButWrongTypedStillThrows)
{
    EXPECT_THROW(logos::fromJson<std::optional<int64_t>>(json("nope")), logos::CodecError);
    EXPECT_THROW(logos::fromJson<std::optional<std::string>>(json(5)), logos::CodecError);
    EXPECT_THROW(logos::fromJson<std::optional<bool>>(json(1)), logos::CodecError);
    EXPECT_THROW(logos::fromJson<std::optional<std::vector<int64_t>>>(json("nope")),
                 logos::CodecError);
    // Range checking survives the wrapper too.
    EXPECT_THROW(logos::fromJson<std::optional<int64_t>>(json(1.5)), logos::CodecError);

    // ...and the path still names the offending element, not the optional.
    try {
        logos::fromJson<std::vector<std::optional<int64_t>>>(
            json::array({1, nullptr, "x"}));
        FAIL() << "expected CodecError";
    } catch (const logos::CodecError& e) {
        EXPECT_NE(std::string(e.what()).find("[2]"), std::string::npos) << e.what();
    }
}

// A required slot is untouched by any of this: null there still means "wrong
// type", which is the only reason absent-means-empty is safe to allow in an
// optional one.
TEST(CodecOptional, RequiredSlotStillRejectsNull)
{
    EXPECT_THROW(logos::fromJson<int64_t>(json()), logos::CodecError);
    EXPECT_THROW(logos::fromJson<std::string>(json(nullptr)), logos::CodecError);
    EXPECT_THROW(logos::fromJson<std::vector<uint8_t>>(json()), logos::CodecError);
}

// ?bstr — bytes keep their tagged form inside an optional, and an EMPTY byte
// string is a present value, not empty-the-state. Conflating the two is the
// obvious way to lose a deliberately-empty payload.
TEST(CodecOptional, OptionalOfBytes)
{
    const auto some = std::optional<std::vector<uint8_t>>(kSpan);
    const json j = logos::toJson(some);
    ASSERT_TRUE(logos::isTaggedBytes(j));
    EXPECT_EQ(j["_bytes"], kSpanB64);
    EXPECT_EQ(logos::fromJson<std::optional<std::vector<uint8_t>>>(j), some);

    EXPECT_TRUE(logos::toJson(std::optional<std::vector<uint8_t>>{}).is_null());
    EXPECT_EQ(logos::fromJson<std::optional<std::vector<uint8_t>>>(json()), std::nullopt);

    // Present-but-empty stays present.
    const auto emptyBytes = std::optional<std::vector<uint8_t>>(std::vector<uint8_t>{});
    const json ej = logos::toJson(emptyBytes);
    EXPECT_FALSE(ej.is_null());
    EXPECT_EQ(ej["_bytes"], "");
    const auto back = logos::fromJson<std::optional<std::vector<uint8_t>>>(ej);
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(back->empty());

    // bstr's documented leniency does not become "accept anything": an object
    // that is not the tagged form is still an error inside an optional.
    EXPECT_THROW(logos::fromJson<std::optional<std::vector<uint8_t>>>(json{{"a", 1}}),
                 logos::CodecError);
}

// ?[T] and ?{tstr: T}: an empty container is a present value. `[]` and null are
// different states, and only null is empty.
TEST(CodecOptional, OptionalOfContainerSeparatesEmptyFromMissing)
{
    using OptList = std::optional<std::vector<int64_t>>;
    const OptList present(std::vector<int64_t>{});
    EXPECT_TRUE(logos::toJson(present).is_array());
    EXPECT_TRUE(logos::fromJson<OptList>(json::array()).has_value());
    EXPECT_EQ(logos::fromJson<OptList>(json()), std::nullopt);

    const OptList vals(std::vector<int64_t>{1, -2});
    EXPECT_EQ(logos::fromJson<OptList>(logos::toJson(vals)), vals);

    using OptMap = std::optional<std::map<std::string, std::vector<uint8_t>>>;
    const OptMap m(std::map<std::string, std::vector<uint8_t>>{{"a", kSpan}});
    const json mj = logos::toJson(m);
    ASSERT_TRUE(mj.is_object());
    EXPECT_TRUE(logos::isTaggedBytes(mj["a"]));
    EXPECT_EQ(logos::fromJson<OptMap>(mj), m);
    EXPECT_EQ(logos::fromJson<OptMap>(json()), std::nullopt);
}

// [?T] and {tstr: ?T}: an empty element still occupies its position, and an
// empty map value still occupies its key. This is the concrete reason `to()`
// spells empty as null instead of omitting anything — dropping an array element
// changes arity, and dropping a map entry changes the key set, which is data.
TEST(CodecOptional, OptionalInsideContainersKeepsPositionAndKey)
{
    const std::vector<std::optional<int64_t>> list = {1, std::nullopt, 3};
    const json j = logos::toJson(list);
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 3u);
    EXPECT_TRUE(j[1].is_null());
    EXPECT_EQ(logos::fromJson<std::vector<std::optional<int64_t>>>(j), list);

    const std::map<std::string, std::optional<std::string>> m = {{"a", "x"}, {"b", std::nullopt}};
    const json mj = logos::toJson(m);
    ASSERT_TRUE(mj.is_object());
    EXPECT_EQ(mj.size(), 2u);
    EXPECT_TRUE(mj["b"].is_null());
    EXPECT_EQ((logos::fromJson<std::map<std::string, std::optional<std::string>>>(mj)), m);

    // ?[?T] — the wrapper and the element are independent states.
    using OptListOfOpt = std::optional<std::vector<std::optional<int64_t>>>;
    const OptListOfOpt nested(std::vector<std::optional<int64_t>>{std::nullopt, 2});
    EXPECT_EQ(logos::fromJson<OptListOfOpt>(logos::toJson(nested)), nested);
    EXPECT_EQ(logos::fromJson<OptListOfOpt>(json()), std::nullopt);
}

// ??T collapses, and that is the two-state rule holding at depth rather than an
// implementation accident: there is one empty spelling, so "present but inner
// empty" has nowhere to be written and comes back as outer-empty.
TEST(CodecOptional, NestedOptionalCollapses)
{
    using OptOpt = std::optional<std::optional<int64_t>>;

    const OptOpt outerEmpty;
    const OptOpt innerEmpty(std::optional<int64_t>{});
    EXPECT_TRUE(logos::toJson(outerEmpty).is_null());
    EXPECT_TRUE(logos::toJson(innerEmpty).is_null());   // same byte, by design

    EXPECT_EQ(logos::fromJson<OptOpt>(json()), outerEmpty);
    EXPECT_NE(logos::fromJson<OptOpt>(json()), innerEmpty);  // collapsed, not three-state

    const OptOpt value(std::optional<int64_t>(4));
    EXPECT_EQ(logos::fromJson<OptOpt>(logos::toJson(value)), value);
}

// PINS A TRAP, not a feature. JsonArg — the proxy that lets generated dispatch
// decode into the callee's exact parameter type without naming it — CANNOT
// deliver an optional, and the failure is silent-looking: for a
// std::optional<X> target, std::optional's own converting constructor
// optional(U&&) binds an rvalue reference to the proxy and out-ranks the
// proxy's const-qualified conversion function, so what gets decoded is X, and a
// null (an EMPTY optional) is rejected as a wrong-typed X.
//
// Verified against the alternatives before writing this down: making the
// conversion operator rvalue-qualified, or adding a conversion operator
// specifically for std::optional, either ties with the constructor (ambiguity
// error) or loses to it exactly as before. There is no signature that wins.
//
// So an optional parameter must be decoded by NAMING the type —
// fromJson<std::optional<X>>(j, path), which is what the cdylib backend already
// emits for every parameter. This test exists so that wiring JsonArg into
// optional dispatch fails here first, with the reason attached.
TEST(CodecOptional, JsonArgCannotDeliverOptionalsNameTheTypeInstead)
{
    const json nullValue;
    // Decoded as int64_t, not as std::optional<int64_t>: empty becomes an error.
    EXPECT_THROW(([&] {
                     const std::optional<int64_t> x = logos::JsonArg(nullValue, "arg0");
                     (void)x;
                 }()),
                 logos::CodecError);

    // Naming the type is the supported spelling, and it does the right thing.
    EXPECT_EQ(logos::fromJson<std::optional<int64_t>>(nullValue, "arg0"), std::nullopt);

    // A PRESENT value happens to survive the proxy (it decodes as X and is then
    // wrapped by that same constructor) — which is exactly why the empty case
    // has to be pinned: the trap only shows itself on the state that matters.
    const json intValue(42);
    const std::optional<int64_t> some = logos::JsonArg(intValue, "arg0");
    EXPECT_EQ(some, std::optional<int64_t>(42));
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
