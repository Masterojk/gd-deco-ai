// Deco AI - talking to the Claude API.
#pragma once

#include <Geode/Geode.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/web.hpp>

#include <string>

namespace deco {

extern char const* const kDecoSystem;
extern char const* const kLayoutSystem;
extern char const* const kIdeasSystem;

bool hasApiKey();

// Sends one message to Claude. `cb` runs on the main thread with Claude's text
// or an error message. Destroying `holder` cancels the request.
void askClaude(geode::async::TaskHolder<geode::utils::web::WebResponse>& holder, std::string const& system, std::string const& user,
               int maxTokens, geode::Function<void(geode::Result<std::string>)> cb);

} // namespace deco
