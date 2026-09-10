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
