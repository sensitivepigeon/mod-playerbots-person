// The AI provider of bot personalities, on its own: Boost and OpenSSL, no core.
//   cl /std:c++20 /EHsc /I src\Mgr\Personality /I <boost> /I <openssl>\include tests\personality_provider.cpp
//      src\Mgr\Personality\PersonalityProvider.cpp <openssl>\lib\libssl.lib <openssl>\lib\libcrypto.lib
// With "live" as argument it also sends one request with a key that is not valid: the API must answer
// 401 authentication_error, which proves the TLS connection, the certificates and the headers. It costs
// nothing.
#include "PersonalityProvider.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    PersonalityApiSettings settings;
    std::string const body = BuildPersonalityRequestBody(settings, "SYSTEM", "USER", 3);
    assert(body.find("\"model\":\"claude-opus-5-5\"") != std::string::npos);
    assert(body.find("\"fallbacks\":\"default\"") != std::string::npos);
    assert(body.find("\"cache_control\":{\"type\":\"ephemeral\"}") != std::string::npos);
    assert(body.find("\"effort\":\"low\"") != std::string::npos);
    assert(body.find("\"type\":\"json_schema\"") != std::string::npos);
    assert(body.find("temperature") == std::string::npos);  // rejected on API version 2026-10-21

    settings.model = "claude-haiku-5-5";  // no server-side fallback for Haiku: the parameter would be a 400
    assert(BuildPersonalityRequestBody(settings, "S", "U", 3).find("fallbacks") == std::string::npos);

    // A normal answer: thinking first, then the lines.
    PersonalityApiResult ok = ParsePersonalityResponse(200, R"({
        "model":"claude-opus-5-5","stop_reason":"end_turn",
        "content":[{"type":"thinking","thinking":""},
                   {"type":"text","text":"{\"lines\":[{\"speaker\":\"Gidaeus\",\"text\":\"chill out, Pigeon\"}]}"}],
        "usage":{"input_tokens":412,"cache_read_input_tokens":1530,"cache_creation_input_tokens":0,"output_tokens":96}})");
    assert(ok.ok && !ok.refused);
    assert(ok.lines.size() == 1 && ok.lines[0].speaker == "Gidaeus" && ok.lines[0].text == "chill out, Pigeon");
    assert(ok.inputTokens == 412 && ok.cacheReadTokens == 1530 && ok.outputTokens == 96);

    // Nothing to say is a valid answer.
    PersonalityApiResult quiet = ParsePersonalityResponse(
        200, R"({"stop_reason":"end_turn","content":[{"type":"text","text":"{\"lines\":[]}"}]})");
    assert(quiet.ok && quiet.lines.empty());

    PersonalityApiResult refused = ParsePersonalityResponse(200, R"({"stop_reason":"refusal","content":[]})");
    assert(!refused.ok && refused.refused);

    PersonalityApiResult limited = ParsePersonalityResponse(
        429, R"({"type":"error","error":{"type":"rate_limit_error","message":"slow down"}})");
    assert(!limited.ok && limited.error == "HTTP 429 rate_limit_error: slow down");

    assert(!ParsePersonalityResponse(200, "not json").ok);
    assert(!RequestPersonalityLines(PersonalityApiSettings{}, "S", "U", 3).ok);  // no key: no request at all

    if (argc > 1 && std::strcmp(argv[1], "live") == 0)
    {
        PersonalityApiSettings live;
        live.apiKey = "sk-ant-not-a-real-key";
        live.timeoutMs = 20000;
        PersonalityApiResult answer = RequestPersonalityLines(live, "S", "U", 3);
        std::printf("live: status %d, %u ms, error: %s\n", answer.status, answer.latencyMs, answer.error.c_str());
        assert(answer.status == 401);
        assert(answer.error.find("authentication_error") != std::string::npos);
    }
    std::printf("PROVIDER-TESTS-PASSED\n");
}
