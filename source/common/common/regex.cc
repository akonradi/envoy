#include "common/common/regex.h"

#include "envoy/common/exception.h"

#include "common/common/assert.h"
#include "common/common/fmt.h"
#include "common/protobuf/utility.h"

#include "re2/re2.h"

namespace Envoy {
namespace Regex {
namespace {

class CompiledStdMatcher : public CompiledMatcher {
public:
  CompiledStdMatcher(std::regex&& regex) : regex_(std::move(regex)) {}

  // CompiledMatcher
  bool match(absl::string_view value) const override {
    return std::regex_match(value.begin(), value.end(), regex_);
  }

private:
  const std::regex regex_;
};

class CompiledGoogleReMatcher : public CompiledMatcher {
public:
  CompiledGoogleReMatcher(std::unique_ptr<re2::RE2> regex) : regex_(std::move(regex)) {}

  // CompiledMatcher
  bool match(absl::string_view value) const override {
    return re2::RE2::FullMatch(re2::StringPiece(value.data(), value.size()), *regex_);
  }

private:
  const std::unique_ptr<const re2::RE2> regex_;
};

} // namespace

CompiledMatcherPtr Utility::parseRegex(const envoy::type::matcher::RegexMatcher& matcher) {
  // Google Re is the only currently supported engine.
  ASSERT(matcher.has_google_re2());

  auto re2 = parseGoogleReRegex(matcher.regex());
  const uint32_t max_program_size =
      PROTOBUF_GET_WRAPPED_OR_DEFAULT(matcher.google_re2(), max_program_size, 100);
  if (static_cast<uint32_t>(re2->ProgramSize()) > max_program_size) {
    throw EnvoyException(fmt::format("regex '{}' RE2 program size of {} > max program size of "
                                     "{}. Increase configured max program size if necessary.",
                                     matcher.regex(), re2->ProgramSize(), max_program_size));
  }
  return std::make_unique<CompiledGoogleReMatcher>(std::move(re2));
}

CompiledMatcherPtr Utility::parseStdRegexAsCompiledMatcher(const std::string& regex,
                                                           std::regex::flag_type flags) {
  return std::make_unique<CompiledStdMatcher>(parseStdRegex(regex, flags));
}

std::unique_ptr<re2::RE2> Utility::parseGoogleReRegex(const std::string& regex) {
  auto re2 = std::make_unique<re2::RE2>(regex, re2::RE2::Quiet);
  if (!re2->ok()) {
    throw EnvoyException(fmt::format("Invalid regex '{}': {}", regex, re2->error()));
  }

  return re2;
}

std::regex Utility::parseStdRegex(const std::string& regex, std::regex::flag_type flags) {
  // TODO(zuercher): In the future, PGV (https://github.com/envoyproxy/protoc-gen-validate)
  // annotations may allow us to remove this in favor of direct validation of regular
  // expressions.
  try {
    return std::regex(regex, flags);
  } catch (const std::regex_error& e) {
    throw EnvoyException(fmt::format("Invalid regex '{}': {}", regex, e.what()));
  }
}

} // namespace Regex
} // namespace Envoy
