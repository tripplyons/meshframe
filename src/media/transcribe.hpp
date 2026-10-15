#pragma once

#include "model/project.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace mf {

// Speech to text through the whisper-cli program from whisper.cpp.

// whisper-cli on PATH or in the usual Homebrew places; empty when missing.
std::string findWhisperCli();
// $MESHFRAME_WHISPER_MODEL, or ~/.cache/meshframe/whisper/ggml-base.en.bin.
std::string defaultWhisperModel();

// Converts whisper-cli --output-json-full output. Words join the tokens
// between leading spaces; bracketed non-speech segments are dropped.
Transcript parseWhisperJson(const nlohmann::json& j, std::string model);

// Decodes a file's audio, runs whisper-cli on it, and parses the result.
// Blocks for as long as transcription takes; run it off the UI thread.
std::optional<Transcript> transcribeFile(const std::string& path, const std::string& model, const std::string& language,
                                         std::string* err);

}  // namespace mf
