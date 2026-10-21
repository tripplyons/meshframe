#include "model/captions.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace mf {

namespace {

struct Word {
    std::string text;
    double start, end;
};

bool wordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80; }

// Replaces `from` where it stands as a whole word, so "VLLM's" and "VLLM,"
// change but "VLLMs" does not.
std::string respell(std::string w, const std::map<std::string, std::string>& replace) {
    for (const auto& [from, to] : replace) {
        if (from.empty()) continue;
        for (size_t at = w.find(from); at != std::string::npos; at = w.find(from, at)) {
            bool before = at == 0 || (!wordChar(w[at - 1]) && w[at - 1] != '.');
            bool after = at + from.size() == w.size() || !wordChar(w[at + from.size()]);
            if (before && after) {
                w.replace(at, from.size(), to);
                at += to.size();
            } else {
                at += from.size();
            }
        }
    }
    return w;
}

size_t textLength(const std::vector<Word>& ws, size_t a, size_t b) {
    size_t n = b > a ? b - a - 1 : 0;  // spaces
    for (size_t i = a; i < b; ++i) n += ws[i].text.size();
    return n;
}

std::string join(const std::vector<Word>& ws, size_t a, size_t b) {
    std::string out;
    for (size_t i = a; i < b; ++i) out += (i > a ? " " : "") + ws[i].text;
    return out;
}

// The words as one line, or two split where the longer line is shortest;
// empty when they don't fit.
std::vector<std::string> fit(const std::vector<Word>& ws, size_t a, size_t b, size_t width) {
    if (textLength(ws, a, b) <= width) return {join(ws, a, b)};
    size_t best = 0, bestLongest = width + 1;
    for (size_t i = a + 1; i < b; ++i) {
        size_t longest = std::max(textLength(ws, a, i), textLength(ws, i, b));
        if (longest < bestLongest) best = i, bestLongest = longest;
    }
    if (bestLongest > width) return {};
    return {join(ws, a, best), join(ws, best, b)};
}

// Splits words [a, b) into n parts of about equal length, ending a part early
// at a comma once it is most of the way there.
std::vector<size_t> splitEvenly(const std::vector<Word>& ws, size_t a, size_t b, size_t n) {
    double target = double(textLength(ws, a, b)) / double(n);
    std::vector<size_t> ends;
    size_t partStart = a;
    for (size_t i = a; i < b; ++i) {
        if (i > partStart && ends.size() + 1 < n) {
            double size = double(textLength(ws, partStart, i));
            char last = ws[i - 1].text.back();
            if (size + double(ws[i].text.size()) > target * 1.15 || (size >= target * 0.7 && (last == ',' || last == ';' || last == ':'))) {
                ends.push_back(i);
                partStart = i;
            }
        }
    }
    ends.push_back(b);
    return ends;
}

std::string stamp(double t, bool vtt) {
    auto ms = int64_t(std::llround(std::max(0.0, t) * 1000));
    return fmt::format("{:02d}:{:02d}:{:02d}{}{:03d}", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, vtt ? '.' : ',', ms % 1000);
}

}  // namespace

std::vector<Caption> timelineCaptions(const Project& p, const CaptionOptions& o) {
    std::vector<Word> words;
    for (const Clip& c : p.clips) {
        const Track* t = p.findTrack(c.track);
        const MediaSource* m = c.kind == ClipKind::Media ? p.findMedia(c.media) : nullptr;
        if (!t || t->kind != TrackKind::Audio || t->muted || !m || !m->transcript || c.speed <= 0) continue;
        double srcEnd = c.in + c.duration * c.speed;
        for (const auto& w : m->transcript->words) {
            double mid = (w.start + w.end) / 2;
            if (mid < c.in || mid >= srcEnd) continue;
            auto at = [&](double s) { return c.start + (std::clamp(s, c.in, srcEnd) - c.in) / c.speed; };
            words.push_back({respell(w.text, o.replace), at(w.start), at(w.end)});
        }
    }
    std::ranges::stable_sort(words, {}, &Word::start);
    std::erase_if(words, [](const Word& w) { return w.text.empty(); });

    // Sentences end at . ? ! or at a pause; a pause inside a clause must be longer.
    std::vector<std::pair<size_t, size_t>> sentences;
    for (size_t i = 0, start = 0; i < words.size(); ++i) {
        bool last = i + 1 == words.size();
        char end = words[i].text.back();
        double gap = last ? 0 : words[i + 1].start - words[i].end;
        bool clause = end == ',' || end == ';' || end == ':';
        if (last || end == '.' || end == '?' || end == '!' || gap > (clause ? 0.5 : 1.0)) {
            sentences.push_back({start, i + 1});
            start = i + 1;
        }
    }

    auto width = size_t(std::max(8, o.lineLength));
    std::vector<Caption> out;
    for (auto [a, b] : sentences) {
        for (size_t n = 1;; ++n) {
            std::vector<size_t> ends = splitEvenly(words, a, b, n);
            std::vector<Caption> parts;
            size_t from = a;
            for (size_t e : ends) {
                auto lines = fit(words, from, e, width);
                if (lines.empty() && e - from > 1) break;  // a single word too long for a line still gets a caption
                parts.push_back({words[from].start, words[e - 1].end, lines.empty() ? std::vector{join(words, from, e)} : lines});
                from = e;
            }
            if (parts.size() == ends.size()) {
                out.insert(out.end(), parts.begin(), parts.end());
                break;
            }
        }
    }
    for (size_t i = 0; i + 1 < out.size(); ++i) out[i].end = std::min(out[i].end + 0.3, out[i + 1].start);
    if (!out.empty()) out.back().end += 0.3;
    return out;
}

std::string formatCaptions(const std::vector<Caption>& captions, bool vtt) {
    std::string out = vtt ? "WEBVTT\n\n" : "";
    for (size_t i = 0; i < captions.size(); ++i) {
        const Caption& c = captions[i];
        if (!vtt) out += fmt::format("{}\n", i + 1);
        out += fmt::format("{} --> {}\n", stamp(c.start, vtt), stamp(c.end, vtt));
        for (const auto& line : c.lines) out += line + "\n";
        out += "\n";
    }
    return out;
}

}  // namespace mf
