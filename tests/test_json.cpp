#include "abp/Json.h"
#include "TestFramework.h"

using abp::json::JsonValue;

ABP_TEST(json_roundtrip_primitives) {
    JsonValue obj = JsonValue::makeObject();
    obj.set("str", "hello \"world\"\n");
    obj.set("num", 42);
    obj.set("flag", true);
    obj.set("nothing", nullptr);

    std::string dumped = obj.dump(2);
    JsonValue parsed = JsonValue::parse(dumped);

    ABP_CHECK_EQ(parsed.get("str").asString(), "hello \"world\"\n");
    ABP_CHECK_EQ(parsed.get("num").asInt(), 42);
    ABP_CHECK(parsed.get("flag").asBool());
    ABP_CHECK(parsed.get("nothing").isNull());
}

ABP_TEST(json_array_and_nested_object) {
    JsonValue root = JsonValue::makeObject();
    JsonValue arr = JsonValue::makeArray();
    arr.push_back(JsonValue(1));
    arr.push_back(JsonValue(2));
    arr.push_back(JsonValue(3));
    root.set("numbers", arr);

    JsonValue nested = JsonValue::makeObject();
    nested.set("inner", "value");
    root.set("nested", nested);

    JsonValue parsed = JsonValue::parse(root.dump());
    ABP_CHECK_EQ(parsed.get("numbers").items().size(), 3u);
    ABP_CHECK_EQ(parsed.get("numbers").items()[1].asInt(), 2);
    ABP_CHECK_EQ(parsed.get("nested").get("inner").asString(), "value");
}

ABP_TEST(json_preserves_object_key_order) {
    JsonValue obj = JsonValue::makeObject();
    obj.set("z", 1);
    obj.set("a", 2);
    obj.set("m", 3);

    const auto& members = obj.members();
    ABP_CHECK_EQ(members.size(), 3u);
    ABP_CHECK_EQ(members[0].first, "z");
    ABP_CHECK_EQ(members[1].first, "a");
    ABP_CHECK_EQ(members[2].first, "m");
}

ABP_TEST(json_missing_key_returns_default) {
    JsonValue obj = JsonValue::makeObject();
    ABP_CHECK_EQ(obj.get("missing").asString("fallback"), "fallback");
    ABP_CHECK(!obj.has("missing"));
}

ABP_TEST(json_parses_empty_containers) {
    JsonValue parsed = JsonValue::parse("{\"a\": [], \"b\": {}}");
    ABP_CHECK(parsed.get("a").isArray());
    ABP_CHECK_EQ(parsed.get("a").items().size(), 0u);
    ABP_CHECK(parsed.get("b").isObject());
}

ABP_TEST(json_rejects_malformed_input) {
    bool threw = false;
    try {
        JsonValue::parse("{ not valid json");
    } catch (const abp::json::JsonParseError&) {
        threw = true;
    }
    ABP_CHECK(threw);
}

ABP_TEST(json_rejects_deeply_nested_input_instead_of_crashing) {
    // Each nested container costs a stack frame in the recursive-descent
    // parser, so an unbounded nest used to segfault rather than fail.
    std::string deep(100000, '[');
    bool threw = false;
    try {
        JsonValue::parse(deep);
    } catch (const abp::json::JsonParseError&) {
        threw = true;
    }
    ABP_CHECK(threw);
}

ABP_TEST(json_accepts_reasonable_nesting) {
    std::string doc;
    const int depth = 50;
    for (int i = 0; i < depth; ++i) doc += "[";
    doc += "1";
    for (int i = 0; i < depth; ++i) doc += "]";

    JsonValue parsed = JsonValue::parse(doc);
    ABP_CHECK(parsed.isArray());
}

ABP_TEST(json_decodes_surrogate_pairs_as_one_code_point) {
    // U+1F600 GRINNING FACE arrives as the pair \uD83D\uDE00 and must come out
    // as its 4-byte UTF-8 encoding, not as two 3-byte lone surrogates.
    JsonValue parsed = JsonValue::parse("{\"s\":\"\\ud83d\\ude00\"}");
    const std::string s = parsed.get("s").asString();
    ABP_CHECK_EQ(s.size(), 4u);
    ABP_CHECK_EQ(s, "\xf0\x9f\x98\x80");
}

ABP_TEST(json_replaces_unpaired_surrogates) {
    // A lone surrogate has no UTF-8 encoding; U+FFFD keeps the output valid.
    JsonValue parsed = JsonValue::parse("{\"s\":\"\\ud83d\"}");
    ABP_CHECK_EQ(parsed.get("s").asString(), "\xef\xbf\xbd");
}

ABP_TEST(json_decodes_basic_multilingual_plane_escapes) {
    ABP_CHECK_EQ(JsonValue::parse("{\"s\":\"\\u0041\"}").get("s").asString(), "A");
    ABP_CHECK_EQ(JsonValue::parse("{\"s\":\"\\u00e9\"}").get("s").asString(), "\xc3\xa9");
    ABP_CHECK_EQ(JsonValue::parse("{\"s\":\"\\u20ac\"}").get("s").asString(), "\xe2\x82\xac");
}

ABP_TEST(json_rejects_truncated_literals) {
    // matchLiteral used to index past the end of the buffer for these.
    for (const char* bad : {"tru", "fals", "nul", "t", "n"}) {
        bool threw = false;
        try {
            JsonValue::parse(bad);
        } catch (const abp::json::JsonParseError&) {
            threw = true;
        }
        ABP_CHECK(threw);
    }
}

ABP_TEST(json_round_trips_non_ascii_text) {
    JsonValue obj = JsonValue::makeObject();
    obj.set("model", "Pixel \xf0\x9f\x98\x80 Pro");
    JsonValue parsed = JsonValue::parse(obj.dump());
    ABP_CHECK_EQ(parsed.get("model").asString(), "Pixel \xf0\x9f\x98\x80 Pro");
}
