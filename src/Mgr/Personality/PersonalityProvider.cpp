/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PersonalityProvider.h"

// Boost.JSON header-only: its implementation is compiled here, in this one file of the module. First of all
// Boost headers, with MSVC's auto-link off: it would otherwise ask for a libboost_json and a libboost_container
// that the core never builds, and that nothing here needs.
#ifndef BOOST_JSON_NO_LIB
#define BOOST_JSON_NO_LIB
#endif
#ifndef BOOST_CONTAINER_NO_LIB
#define BOOST_CONTAINER_NO_LIB
#endif
#include <boost/json/src.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <chrono>
#include <exception>

#ifdef _WIN32
// OpenSSL does not read the Windows certificate store by itself: its trusted roots are handed over below.
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

namespace
{
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;

constexpr char const* ApiVersion = "2026-10-21";
constexpr char const* FallbackBeta = "server-side-fallback-2026-07-01";

ssl::context& ClientContext()
{
    // Built once and shared: OpenSSL lets several threads open connections from one context.
    static ssl::context* context = []()
    {
        auto* made = new ssl::context(ssl::context::tls_client);
        made->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                          ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
        made->set_verify_mode(ssl::verify_peer);
#ifdef _WIN32
        if (HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT"))
        {
            X509_STORE* trusted = SSL_CTX_get_cert_store(made->native_handle());
            for (PCCERT_CONTEXT cert = CertEnumCertificatesInStore(store, nullptr); cert;
                 cert = CertEnumCertificatesInStore(store, cert))
            {
                unsigned char const* data = cert->pbCertEncoded;
                if (X509* x509 = d2i_X509(nullptr, &data, long(cert->cbCertEncoded)))
                {
                    X509_STORE_add_cert(trusted, x509);
                    X509_free(x509);
                }
            }
            CertCloseStore(store, 0);
        }
#else
        made->set_default_verify_paths();
#endif
        return made;
    }();
    return *context;
}

struct HttpReply
{
    int status = 0;
    std::string body;
    std::string error;
};

asio::awaitable<void> Post(HttpReply* reply, std::string host, std::string target,
                           std::vector<std::pair<std::string, std::string>> headers, std::string body,
                           std::chrono::steady_clock::time_point deadline)
{
    auto executor = co_await asio::this_coro::executor;
    asio::ip::tcp::resolver resolver(executor);
    beast::ssl_stream<beast::tcp_stream> stream(executor, ClientContext());

    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        throw beast::system_error(beast::error_code(int(::ERR_get_error()), asio::error::get_ssl_category()));
    stream.set_verify_callback(ssl::host_name_verification(host));

    auto const endpoints = co_await resolver.async_resolve(host, "443", asio::use_awaitable);
    beast::get_lowest_layer(stream).expires_at(deadline);
    co_await beast::get_lowest_layer(stream).async_connect(endpoints, asio::use_awaitable);
    co_await stream.async_handshake(ssl::stream_base::client, asio::use_awaitable);

    http::request<http::string_body> request{http::verb::post, target, 11};
    request.set(http::field::host, host);
    request.set(http::field::user_agent, "mod-playerbots-personality");
    for (auto const& [name, value] : headers)
        request.set(name, value);
    request.body() = std::move(body);
    request.prepare_payload();
    co_await http::async_write(stream, request, asio::use_awaitable);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, asio::use_awaitable);
    reply->status = int(response.result_int());
    reply->body = std::move(response.body());

    // The answer is in: no TLS goodbye, which can wait on a slow peer for nothing.
    beast::error_code ignored;
    beast::get_lowest_layer(stream).socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
}

HttpReply HttpsPost(std::string const& host, std::string const& target,
                    std::vector<std::pair<std::string, std::string>> headers, std::string body, uint32_t timeoutMs)
{
    HttpReply reply;
    asio::io_context context;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    asio::co_spawn(context, Post(&reply, host, target, std::move(headers), std::move(body), deadline),
                   [&reply](std::exception_ptr failure)
                   {
                       if (!failure)
                           return;
                       try
                       {
                           std::rethrow_exception(failure);
                       }
                       catch (std::exception const& e)
                       {
                           reply.error = e.what();
                       }
                   });
    context.run();
    return reply;
}

json::object StringType()
{
    json::object type;
    type["type"] = "string";
    return type;
}

json::object LinesSchema()
{
    json::object lineProperties;
    lineProperties["speaker"] = StringType();
    lineProperties["text"] = StringType();

    json::object line;
    line["type"] = "object";
    line["additionalProperties"] = false;
    line["required"] = json::array{"speaker", "text"};
    line["properties"] = std::move(lineProperties);

    json::object lines;
    lines["type"] = "array";
    lines["items"] = std::move(line);

    json::object properties;
    properties["lines"] = std::move(lines);

    json::object schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::array{"lines"};
    schema["properties"] = std::move(properties);
    return schema;
}

std::string StringField(json::object const& object, char const* key)
{
    if (json::value const* value = object.if_contains(key))
        if (value->is_string())
            return std::string(value->as_string());
    return {};
}

uint32_t CountField(json::object const& object, char const* key)
{
    if (json::value const* value = object.if_contains(key))
        if (value->is_int64() && value->as_int64() >= 0)
            return uint32_t(value->as_int64());
    return 0;
}
}  // namespace

bool PersonalityModelTakesFallbacks(std::string const& model)
{
    return model == "claude-opus-5-5" || model == "claude-opus-5" || model == "claude-fable-5-1" ||
           model == "claude-sonnet-5-5";
}

std::string BuildPersonalityRequestBody(PersonalityApiSettings const& settings, std::string const& system,
                                        std::string const& user, uint32_t /*maxLines*/)
{
    // The number of lines is asked in the prompt and enforced when the lines come back: the schema keeps to
    // what structured outputs always accept.
    json::object format;
    format["type"] = "json_schema";
    format["schema"] = LinesSchema();

    json::object output;
    if (!settings.effort.empty())
        output["effort"] = settings.effort;
    output["format"] = std::move(format);

    // The system prompt only changes with the party: it is cached, and every exchange of a run reads it back.
    json::object cache;
    cache["type"] = "ephemeral";
    json::object systemBlock;
    systemBlock["type"] = "text";
    systemBlock["text"] = system;
    systemBlock["cache_control"] = std::move(cache);

    json::object message;
    message["role"] = "user";
    message["content"] = user;

    json::object body;
    body["model"] = settings.model;
    body["max_tokens"] = settings.maxTokens;
    body["output_config"] = std::move(output);
    body["system"] = json::array{std::move(systemBlock)};
    body["messages"] = json::array{std::move(message)};
    if (settings.fallbacks && PersonalityModelTakesFallbacks(settings.model))
        body["fallbacks"] = "default";
    return json::serialize(body);
}

PersonalityApiResult ParsePersonalityResponse(int status, std::string const& body)
{
    PersonalityApiResult result;
    result.status = status;

    boost::system::error_code parseError;
    json::value parsed = json::parse(body, parseError);
    if (parseError || !parsed.is_object())
    {
        result.error = "HTTP " + std::to_string(status) + ", the answer is not JSON";
        return result;
    }
    json::object const& answer = parsed.as_object();

    if (status != 200)
    {
        result.error = "HTTP " + std::to_string(status);
        if (json::value const* error = answer.if_contains("error"); error && error->is_object())
            result.error += " " + StringField(error->as_object(), "type") + ": " + StringField(error->as_object(), "message");
        return result;
    }

    result.stopReason = StringField(answer, "stop_reason");
    result.model = StringField(answer, "model");
    if (json::value const* usage = answer.if_contains("usage"); usage && usage->is_object())
    {
        json::object const& counts = usage->as_object();
        result.inputTokens = CountField(counts, "input_tokens");
        result.cacheReadTokens = CountField(counts, "cache_read_input_tokens");
        result.cacheWriteTokens = CountField(counts, "cache_creation_input_tokens");
        result.outputTokens = CountField(counts, "output_tokens");
    }

    if (result.stopReason == "refusal")
    {
        result.refused = true;
        result.error = "declined (refusal)";
        return result;
    }
    if (result.stopReason == "max_tokens")
    {
        result.error = "cut at max_tokens: raise AiPlayerbot.Personality.MaxTokens";
        return result;
    }

    // Thinking blocks come first; the answer is the first text block.
    json::value const* content = answer.if_contains("content");
    if (!content || !content->is_array())
    {
        result.error = "no content in the answer";
        return result;
    }
    for (json::value const& block : content->as_array())
    {
        if (!block.is_object() || StringField(block.as_object(), "type") != "text")
            continue;

        json::value lines = json::parse(StringField(block.as_object(), "text"), parseError);
        if (parseError || !lines.is_object())
        {
            result.error = "the lines are not JSON";
            return result;
        }
        if (json::value const* list = lines.as_object().if_contains("lines"); list && list->is_array())
            for (json::value const& line : list->as_array())
                if (line.is_object())
                    result.lines.push_back({StringField(line.as_object(), "speaker"), StringField(line.as_object(), "text")});
        result.ok = true;
        return result;
    }
    result.error = "no text block in the answer";
    return result;
}

PersonalityApiResult RequestPersonalityLines(PersonalityApiSettings const& settings, std::string const& system,
                                             std::string const& user, uint32_t maxLines)
{
    if (settings.apiKey.empty())
    {
        PersonalityApiResult missing;
        missing.error = "no API key (AiPlayerbot.Personality.ApiKeyEnv / ApiKey)";
        return missing;
    }

    std::vector<std::pair<std::string, std::string>> headers = {
        {"x-api-key", settings.apiKey},
        {"anthropic-version", ApiVersion},
        {"content-type", "application/json"},
    };
    if (settings.fallbacks && PersonalityModelTakesFallbacks(settings.model))
        headers.emplace_back("anthropic-beta", FallbackBeta);

    auto const start = std::chrono::steady_clock::now();
    HttpReply reply = HttpsPost(settings.host, "/v1/messages", std::move(headers),
                                BuildPersonalityRequestBody(settings, system, user, maxLines), settings.timeoutMs);
    uint32_t const latency = uint32_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());

    PersonalityApiResult result;
    if (!reply.error.empty())
    {
        result.error = "request failed: " + reply.error;
        result.latencyMs = latency;
        return result;
    }
    result = ParsePersonalityResponse(reply.status, reply.body);
    result.latencyMs = latency;
    return result;
}
