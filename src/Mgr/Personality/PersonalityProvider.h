/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PERSONALITYPROVIDER_H
#define PLAYERBOTS_PERSONALITYPROVIDER_H

// The AI provider of bot personalities: one HTTPS request to the Anthropic Messages API, made on a worker
// thread, that comes back with the lines of a party exchange. Boost (Asio, Beast, JSON) and OpenSSL only,
// no game object, so that tests/personality_provider.cpp builds it on its own.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct PersonalityApiSettings
{
    std::string host = "api.anthropic.com";
    std::string apiKey;                   // never logged
    std::string model = "claude-opus-5-5";
    std::string effort = "low";           // empty: the model's default
    uint32_t maxTokens = 4096;            // thinking counts against it too
    uint32_t timeoutMs = 30000;
    bool fallbacks = true;                // "fallbacks": "default", where the model takes it
};

struct PersonalityLine
{
    std::string speaker;
    std::string text;
};

struct PersonalityApiResult
{
    bool ok = false;       // the model answered; lines may still be empty (nothing to say)
    bool refused = false;  // stop_reason "refusal": no lines, no retry
    int status = 0;        // HTTP status, 0 when the request never got an answer
    std::string error;     // what went wrong, for the log and ".playerbots personality stats"
    std::string stopReason;
    std::string model;     // the model that answered (another one after a fallback)
    std::vector<PersonalityLine> lines;
    uint32_t inputTokens = 0;
    uint32_t cacheReadTokens = 0;
    uint32_t cacheWriteTokens = 0;
    uint32_t outputTokens = 0;
    uint32_t latencyMs = 0;
};

// Server-side refusal fallbacks ("fallbacks": "default") exist for these models only; Claude Haiku 5.5 rejects
// the parameter.
bool PersonalityModelTakesFallbacks(std::string const& model);

std::string BuildPersonalityRequestBody(PersonalityApiSettings const& settings, std::string const& system,
                                        std::string const& user, uint32_t maxLines);
PersonalityApiResult ParsePersonalityResponse(int status, std::string const& body);

// Blocks the calling thread for up to settings.timeoutMs (plus the name lookup).
PersonalityApiResult RequestPersonalityLines(PersonalityApiSettings const& settings, std::string const& system,
                                             std::string const& user, uint32_t maxLines);

#endif
