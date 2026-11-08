// The JSON reader and writer (src/core/json.cpp): text that is read back as written, \u escapes (a surrogate pair is one
// character; a lone half is not turned into garbage bytes), objects with many keys read in time that grows with their
// number (not its square), repeated keys, numbers outside the integer's range, nesting too deep. One line per check.
//   json_test
#include "../../src/core/json.h"
#include <chrono>
#include <cstdio>
#include <string>

using namespace reno;

static int fails = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

static std::string parsedString(const std::string& text)
{
    Json j;
    std::string e;
    if (!Json::parse(text, j, &e)) return "<error: " + e + ">";
    return j.asString();
}

int main()
{
    {
        Json j;
        std::string e;
        const bool ok = Json::parse("{\"a\":[1,2.5,true,null,\"x\"],\"b\":{\"c\":-3},\"a\":7}", j, &e);
        check(ok, "object parses");
        check(ok && j.size() == 2, "a repeated key is one member");
        check(ok && j.get("a").asInt() == 7, "the later value of a repeated key wins");
        check(ok && j.members()[0].first == "a" && j.members()[1].first == "b", "members keep the order the keys came in");
        Json k;
        check(Json::parse(j.dump(), k, &e) && k.dump() == j.dump(), "dump reads back the same");
    }
    check(parsedString("\"\\ud83d\\ude00\"") == "\xF0\x9F\x98\x80", "a surrogate pair is one character");
    check(parsedString("\"\\ud800\\u0041\"") == "\xEF\xBF\xBD" "A", "a high surrogate before an ordinary escape: U+FFFD, then that character");
    check(parsedString("\"\\ud800x\"") == "\xEF\xBF\xBD" "x", "a high surrogate alone: U+FFFD");
    check(parsedString("\"\\udc00\"") == "\xEF\xBF\xBD", "a low surrogate alone: U+FFFD");
    check(parsedString("\"\\u00e9\\u20ac\"") == "\xC3\xA9\xE2\x82\xAC", "two- and three-byte characters");
    {
        // 30000 keys: with a look through the earlier keys for each new one this took about a second
        std::string s = "{";
        for (int i = 0; i < 30000; ++i) {
            if (i) s += ",";
            s += "\"key" + std::to_string(i % 29000) + "\":" + std::to_string(i);
        }
        s += "}";
        const auto t0 = std::chrono::steady_clock::now();
        Json j;
        std::string e;
        const bool ok = Json::parse(s, j, &e);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("     30000 keys parsed in %.1f ms\n", ms);
        check(ok && j.size() == 29000, "big object: 1000 repeated keys merged");
        check(ok && j.get("key5").asInt() == 29005, "big object: a repeated key has its last value");
        check(ok && j.get("key28999").asInt() == 28999, "big object: last key");
        check(ms < 400, "big object parses quickly");
    }
    {
        Json j;
        std::string e;
        check(Json::parse("[1e300,-1e300,1e30]", j, &e), "huge numbers parse");
        check(j.at(0).asInt() == 2147483647 && j.at(1).asInt() == (-2147483647 - 1), "asInt cuts to the range");
        check(j.at(0).asInt64() == INT64_MAX && j.at(1).asInt64() == INT64_MIN, "asInt64 cuts to the range");
        check(j.at(0).asUInt64() == UINT64_MAX && j.at(1).asUInt64() == 0, "asUInt64 cuts to the range");
    }
    {
        std::string deep(300, '['), e;
        Json j;
        check(!Json::parse(deep, j, &e), "nesting too deep is refused");
        check(!Json::parse("{\"a\":1} x", j, &e), "text after the end is refused");
        check(!Json::parse("{\"a\":", j, &e), "an unfinished object is refused");
        check(Json::parse("{}", j, &e) && j.isObject() && j.size() == 0, "empty object");
        check(Json::parse("\xEF\xBB\xBF[1]", j, &e) && j.size() == 1, "a byte order mark is skipped");
    }
    std::printf("%s (%d failed)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
