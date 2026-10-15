#include "media/transcribe.hpp"

#include "media/decoder.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace mf {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\n"), b = s.find_last_not_of(" \t\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool writeWav16(const fs::path& path, const std::vector<int16_t>& samples, int rate) {
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    uint32_t bytes = uint32_t(samples.size() * 2);
    out.write("RIFF", 4), u32(36 + bytes), out.write("WAVEfmt ", 8), u32(16), u16(1), u16(1), u32(uint32_t(rate)),
        u32(uint32_t(rate * 2)), u16(2), u16(16), out.write("data", 4), u32(bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), std::streamsize(bytes));
    return bool(out);
}

}  // namespace

std::string findWhisperCli() {
    std::vector<fs::path> dirs;
    if (const char* path = std::getenv("PATH"))
        for (std::string_view rest = path; !rest.empty();) {
            size_t colon = rest.find(':');
            dirs.emplace_back(rest.substr(0, colon));
            rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
        }
    // An app started from Finder has a short PATH.
    dirs.emplace_back("/opt/homebrew/bin");
    dirs.emplace_back("/usr/local/bin");
    for (const auto& d : dirs)
        if (fs::path p = d / "whisper-cli"; access(p.c_str(), X_OK) == 0) return p.string();
    return {};
}

std::string defaultWhisperModel() {
    if (const char* m = std::getenv("MESHFRAME_WHISPER_MODEL"); m && *m) return m;
    const char* home = std::getenv("HOME");
    return (fs::path(home ? home : "") / ".cache/meshframe/whisper/ggml-base.en.bin").string();
}

Transcript parseWhisperJson(const json& j, std::string model) {
    Transcript t;
    t.model = std::move(model);
    for (const auto& seg : j.value("transcription", json::array())) {
        std::string text = trim(seg.value("text", ""));
        if (text.empty() || (text.front() == '[' && text.back() == ']')) continue;
        const json& off = seg.at("offsets");
        t.segments.push_back({text, off.value("from", 0) / 1000.0, off.value("to", 0) / 1000.0});
        for (const auto& tok : seg.value("tokens", json::array())) {
            std::string piece = tok.value("text", "");
            if (piece.empty() || piece.starts_with("[_")) continue;
            double from = tok.at("offsets").value("from", 0) / 1000.0, to = tok.at("offsets").value("to", 0) / 1000.0;
            bool startsWord = piece.front() == ' ' || t.words.empty() || t.words.back().end <= t.segments.back().start;
            if (startsWord) {
                std::string word = trim(piece);
                if (!word.empty()) t.words.push_back({word, from, to});
            } else {
                t.words.back().text += piece;
                t.words.back().end = to;
            }
        }
    }
    return t;
}

std::optional<Transcript> transcribeFile(const std::string& path, const std::string& model, const std::string& language,
                                         std::string* err) {
    auto fail = [&](std::string msg) -> std::optional<Transcript> {
        if (err) *err = std::move(msg);
        return std::nullopt;
    };
    std::string cli = findWhisperCli();
    if (cli.empty()) return fail("whisper-cli is not installed; install it with: brew install whisper-cpp");
    if (!fs::exists(model))
        return fail(fmt::format("no whisper model at {}; download one from https://huggingface.co/ggerganov/whisper.cpp "
                                "(for example ggml-base.en.bin) or pass model=PATH",
                                model));

    constexpr int kRate = 16000;
    std::string decodeErr;
    auto audio = decodeAudio(path, kRate, &decodeErr);
    if (!audio) return fail("cannot decode audio: " + decodeErr);
    std::vector<int16_t> mono(audio->frames());
    for (size_t i = 0; i < mono.size(); ++i) {
        float v = (audio->samples[2 * i] + audio->samples[2 * i + 1]) / 2;
        mono[i] = int16_t(std::clamp(v, -1.0f, 1.0f) * 32767);
    }

    static std::atomic<int> counter = 0;
    fs::path dir = fs::temp_directory_path() / fmt::format("meshframe-whisper-{}-{}", getpid(), counter++);
    fs::create_directories(dir);
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    } cleanup{dir};
    fs::path wav = dir / "audio.wav", base = dir / "out", log = dir / "stderr.txt";
    if (!writeWav16(wav, mono, kRate)) return fail("cannot write " + wav.string());

    std::vector<std::string> args = {cli, "-m", model, "-f", wav.string(), "-l", language.empty() ? "auto" : language,
                                     "-ojf", "-of", base.string(), "-np"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, cli.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return fail(fmt::format("cannot run {}: {}", cli, std::strerror(rc)));
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::ifstream in(log);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        return fail(fmt::format("whisper-cli failed: {}", trim(text.size() > 2000 ? text.substr(text.size() - 2000) : text)));
    }
    std::ifstream in(base.string() + ".json");
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded()) return fail("whisper-cli wrote no readable JSON");
    return parseWhisperJson(j, model);
}

}  // namespace mf
