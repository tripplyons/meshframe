#include "session/inspect.hpp"

#include "audio/mixer.hpp"
#include "core/timecode.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <set>

namespace mf {

using json = nlohmann::json;

bool ClipBounds::titleSafe(const ProjectSettings& s) const {
    double mx = s.width * 0.05, my = s.height * 0.05;
    return visible && left >= mx && top >= my && right <= s.width - mx && bottom <= s.height - my;
}

ClipBounds measureClip(Session& s, const Clip& c, double t, double scale) {
    const auto& st = s.evaluator().project().settings;
    int w = std::max(2, int(std::lround(st.width * scale))), h = std::max(2, int(std::lround(st.height * scale)));
    int padX = w / 2, padY = h / 2;
    auto img = s.renderClip(c, t, scale, padX, padY);

    // Pixels at least 3% opaque count as covered.
    int x0 = img->w, y0 = img->h, x1 = -1, y1 = -1;
    int64_t covered = 0, inside = 0;
    for (int y = 0; y < img->h; ++y) {
        const uint8_t* row = img->row(y);
        for (int x = 0; x < img->w; ++x)
            if (row[x * 4 + 3] >= 8) {
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                ++covered;
                inside += x >= padX && x < padX + w && y >= padY && y < padY + h;
            }
    }
    ClipBounds b;
    if (!covered) return b;
    double unit = double(w) / st.width;
    b.visible = true;
    b.left = (x0 - padX) / unit;
    b.top = (y0 - padY) / unit;
    b.right = (x1 + 1 - padX) / unit;
    b.bottom = (y1 + 1 - padY) / unit;
    b.inFrame = double(inside) / double(covered);
    b.truncated = x0 == 0 || y0 == 0 || x1 == img->w - 1 || y1 == img->h - 1;
    return b;
}

AudioAnalysis analyzeMix(Session& s, double start, double end, const AudioAnalysisOptions& options) {
    int sr = s.evaluator().project().settings.sampleRate;
    int64_t first = std::llround(start * sr), frames = std::max<int64_t>(0, std::llround(end * sr) - first);
    AudioAnalyzer analyzer(sr, double(first) / sr, frames, options);
    AudioMixer mixer(s.cache());
    std::vector<float> block(size_t(AudioMixer::kBlock) * 16 * 2);
    for (int64_t done = 0; done < frames;) {
        int n = int(std::min<int64_t>(frames - done, int64_t(block.size() / 2)));
        mixer.render(s.evaluator(), double(first + done) / sr, n, block.data(), true);
        analyzer.push(block.data(), n);
        done += n;
    }
    return analyzer.finish();
}

namespace {

// Lowercase letters, digits, and apostrophes, so "Demo," matches "demo".
std::string normalizeWord(std::string_view w) {
    std::string out;
    for (char ch : w) {
        auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '\'' || c >= 0x80) out += char(std::tolower(c));
    }
    return out;
}

}  // namespace

json findSpeech(const Project& p, const std::string& text, Id media) {
    std::vector<std::string> query;
    for (size_t i = 0; i < text.size();) {
        size_t j = text.find_first_of(" \t\n", i);
        if (j == std::string::npos) j = text.size();
        if (std::string w = normalizeWord(std::string_view(text).substr(i, j - i)); !w.empty()) query.push_back(std::move(w));
        i = j + 1;
    }
    json out = json::array();
    if (query.empty()) return out;
    for (const MediaSource& m : p.media) {
        if ((media && m.id != media) || !m.transcript) continue;
        const auto& words = m.transcript->words;
        std::vector<std::string> norm;
        for (const auto& w : words) norm.push_back(normalizeWord(w.text));
        for (size_t i = 0; i + query.size() <= words.size(); ++i) {
            if (!std::equal(query.begin(), query.end(), norm.begin() + std::ptrdiff_t(i))) continue;
            const auto& first = words[i];
            const auto& last = words[i + query.size() - 1];
            std::string spoken;
            for (size_t k = i; k < i + query.size(); ++k) spoken += (k > i ? " " : "") + words[k].text;
            // A linked pair is reported once, through its audio clip.
            auto pairedWithAudio = [&](const Clip& c) {
                const Track* t = p.findTrack(c.track);
                if (!c.link || (t && t->kind == TrackKind::Audio)) return false;
                return std::ranges::any_of(p.clips, [&](const Clip& o) {
                    const Track* ot = p.findTrack(o.track);
                    return &o != &c && o.link == c.link && o.media == c.media && ot && ot->kind == TrackKind::Audio;
                });
            };
            json timeline = json::array();
            for (const Clip& c : p.clips) {
                if (c.kind != ClipKind::Media || c.media != m.id || c.speed <= 0 || pairedWithAudio(c)) continue;
                double srcEnd = c.in + c.duration * c.speed;
                double a = std::max(first.start, c.in), b = std::min(last.end, srcEnd);
                if (b <= a) continue;
                json j = {{"clip", c.id}, {"name", c.name}, {"start", c.start + (a - c.in) / c.speed}, {"end", c.start + (b - c.in) / c.speed}};
                if (a > first.start || b < last.end) j["partial"] = true;
                timeline.push_back(std::move(j));
            }
            out.push_back({{"media", m.id}, {"mediaName", m.name}, {"text", spoken}, {"start", first.start}, {"end", last.end},
                           {"timeline", timeline}});
        }
    }
    return out;
}

namespace {

struct Issue {
    std::string severity, kind;
    double start = 0, end = 0;
    Id clip = 0;
    std::string message;
};

std::optional<double> constantNumber(const Clip& c, const char* name, double fallback) {
    auto it = c.props.find(name);
    if (it == c.props.end()) return fallback;
    if (it->second.mode != PropMode::Constant) return std::nullopt;
    if (const double* d = std::get_if<double>(&it->second.constant)) return *d;
    return std::nullopt;
}

// Whether a clip covers the whole frame with opaque pixels for its whole length.
bool opaqueFullFrame(const Project& p, const Clip& c) {
    if (std::ranges::any_of(c.videoEffects, &Effect::enabled)) return false;
    if (std::ranges::any_of(p.transitions, [&](const Transition& t) { return t.clipA == c.id || t.clipB == c.id; })) return false;
    auto num = [&](const char* name, double fallback) { return constantNumber(c, name, fallback); };
    auto opacity = num("opacity", 1), scale = num("scale", 1), x = num("x", 0), y = num("y", 0), rotation = num("rotation", 0);
    if (opacity != 1.0 || !scale || *scale < 1 || x != 0.0 || y != 0.0 || !rotation || std::fmod(*rotation, 360) != 0) return false;
    if (auto it = c.props.find("blend"); it != c.props.end() && !(it->second.mode == PropMode::Constant && it->second.constant == Value(std::string("normal"))))
        return false;
    if (c.kind == ClipKind::Solid) {
        auto it = c.props.find("color");
        if (it == c.props.end()) return true;
        const Color* col = std::get_if<Color>(&it->second.constant);
        return it->second.mode == PropMode::Constant && col && col->a >= 1;
    }
    if (c.kind == ClipKind::Media) {
        const MediaSource* m = p.findMedia(c.media);
        return m && m->hasVideo && m->width > 0 && m->height > 0 &&
               std::abs(double(m->width) / m->height - double(p.settings.width) / p.settings.height) < 0.005;
    }
    return false;
}

bool onVisibleVideoTrack(const Project& p, const Clip& c) {
    const Track* t = p.findTrack(c.track);
    return t && t->kind == TrackKind::Video && !t->hidden;
}

}  // namespace

json checkProject(Session& s, const CheckOptions& o) {
    Evaluator& ev = s.evaluator();
    const Project& p = ev.project();
    const auto& st = p.settings;
    double frame = 1 / st.fps;
    double dur = p.duration();
    auto snap = [&](double t) { return std::round(t * st.fps) / st.fps; };
    auto tc = [&](double t) { return formatTimecode(t, st.fps); };
    auto clipName = [&](Id id) {
        const Clip* c = p.findClip(id);
        return c ? (c->name.empty() ? fmt::format("#{}", id) : c->name) : fmt::format("#{}", id);
    };
    std::vector<Issue> issues;

    // ---- script ----
    if (const auto& err = ev.script().error)
        issues.push_back({"error", "script", 0, 0, 0, fmt::format("script line {}: {}", err->line, err->message)});

    // ---- structure ----
    std::vector<std::pair<double, double>> covered;
    for (const Clip& c : p.clips)
        if (onVisibleVideoTrack(p, c)) covered.push_back({c.start, c.end()});
    std::ranges::sort(covered);
    double reached = 0;
    auto gap = [&](double a, double b) {
        if (b - a >= frame / 2) issues.push_back({"warning", "gap", a, b, 0, fmt::format("no video from {} to {}", tc(a), tc(b))});
    };
    for (auto [a, b] : covered) {
        if (a > reached) gap(reached, a);
        reached = std::max(reached, b);
    }
    if (reached < dur) gap(reached, dur);

    std::set<Id> usedMedia, reportedMissing;
    for (const Clip& c : p.clips) {
        if (c.kind != ClipKind::Media) continue;
        usedMedia.insert(c.media);
        const MediaSource* m = p.findMedia(c.media);
        if (!m) {
            issues.push_back({"error", "missing-media", c.start, c.end(), c.id, fmt::format("clip '{}' refers to media that is gone", clipName(c.id))});
            continue;
        }
        std::string cacheError = s.cache().error(m->path);
        if (!std::filesystem::exists(m->path) || !cacheError.empty()) {
            if (reportedMissing.insert(m->id).second)
                issues.push_back({"error", "missing-media", c.start, c.end(), c.id,
                                  fmt::format("{} cannot be read: {}", m->name, cacheError.empty() ? m->path + " does not exist" : cacheError)});
            continue;
        }
        if (m->kind == MediaKind::Image || m->duration <= 0) continue;
        double srcEnd = c.in + c.duration * c.speed;
        if (srcEnd > m->duration + frame) {
            double from = c.start + (m->duration - c.in) / c.speed;
            issues.push_back({"error", "past-media", std::max(c.start, from), c.end(), c.id,
                              fmt::format("clip '{}' runs {:.2f} s past the end of {}", clipName(c.id), (srcEnd - m->duration) / c.speed, m->name)});
        }
    }
    for (const MediaSource& m : p.media)
        if (!usedMedia.contains(m.id)) issues.push_back({"warning", "unused-media", 0, 0, 0, fmt::format("{} is not on the timeline", m.name)});

    for (const Clip& c : p.clips) {
        if (!onVisibleVideoTrack(p, c)) continue;
        int level = p.trackIndex(c.track);
        for (const Clip& top : p.clips) {
            if (&top == &c || !onVisibleVideoTrack(p, top) || p.trackIndex(top.track) <= level) continue;
            if (top.start <= c.start + 1e-9 && top.end() >= c.end() - 1e-9 && opaqueFullFrame(p, top)) {
                issues.push_back({"warning", "hidden", c.start, c.end(), c.id,
                                  fmt::format("clip '{}' is covered by '{}' for its whole length", clipName(c.id), clipName(top.id))});
                break;
            }
        }
    }

    // ---- sampled frames ----
    std::vector<double> times;
    if (o.step > 0)
        for (double t = 0; t < dur - 1e-9; t += o.step) times.push_back(snap(t));
    for (const Clip& c : p.clips)
        if (onVisibleVideoTrack(p, c)) {
            times.push_back(snap(c.start));
            times.push_back(snap(c.end() - frame));
        }
    std::erase_if(times, [&](double t) { return t < 0 || t >= dur; });
    std::ranges::sort(times);
    times.erase(std::unique(times.begin(), times.end()), times.end());

    // Findings per key at sample indices; consecutive samples merge into one issue.
    struct Finding {
        Issue issue;
        std::vector<size_t> samples;
    };
    std::map<std::string, Finding> found;
    auto hit = [&](const std::string& key, size_t i, Issue issue) {
        auto& f = found[key];
        if (f.samples.empty()) f.issue = std::move(issue);
        f.samples.push_back(i);
    };
    int smallW = 160, smallH = std::max(2, int(std::lround(160.0 * st.height / st.width)));
    for (size_t i = 0; i < times.size(); ++i) {
        double t = times[i];
        auto img = s.renderFrame(t, smallW, smallH);
        int brightest = 0;
        for (size_t k = 0; k < img->px.size(); k += 4)
            brightest = std::max(brightest, int(0.2126 * img->px[k] + 0.7152 * img->px[k + 1] + 0.0722 * img->px[k + 2]));
        if (brightest < 8) hit("black", i, {"warning", "black", 0, 0, 0, "the frame is black"});
        for (const auto& e : ev.errors()) {
            Id clip = 0;
            if (e.key.starts_with("clip:")) clip = Id(std::strtoull(e.key.c_str() + 5, nullptr, 10));
            hit("error:" + e.key + e.message, i, {"error", "expression", 0, 0, clip, fmt::format("{}: {}", e.key, e.message)});
        }

        std::vector<std::pair<const Clip*, ClipBounds>> texts;
        for (const Clip& c : p.clips) {
            if (c.kind != ClipKind::Text || !onVisibleVideoTrack(p, c) || !ev.clipVisibleAt(c, t)) continue;
            ClipBounds b = measureClip(s, c, t, 0.25);
            if (!b.visible) continue;
            std::string name = clipName(c.id);
            if (b.inFrame < 0.999)
                hit(fmt::format("off-frame:{}", c.id), i,
                    {"warning", "off-frame", 0, 0, c.id, fmt::format("text '{}' is {:.0f}% outside the frame", name, (1 - b.inFrame) * 100)});
            else if (!b.titleSafe(st))
                hit(fmt::format("title-safe:{}", c.id), i, {"warning", "title-safe", 0, 0, c.id, fmt::format("text '{}' is outside the title-safe area", name)});
            texts.push_back({&c, b});
        }
        for (size_t a = 0; a < texts.size(); ++a)
            for (size_t b = a + 1; b < texts.size(); ++b) {
                const ClipBounds &x = texts[a].second, &y = texts[b].second;
                if (std::min(x.right, y.right) <= std::max(x.left, y.left) || std::min(x.bottom, y.bottom) <= std::max(x.top, y.top)) continue;
                Id ia = texts[a].first->id, ib = texts[b].first->id;
                hit(fmt::format("text-overlap:{}:{}", ia, ib), i,
                    {"warning", "text-overlap", 0, 0, ia, fmt::format("text '{}' overlaps text '{}'", clipName(ia), clipName(ib))});
            }
    }
    for (auto& [key, f] : found) {
        for (size_t k = 0; k < f.samples.size();) {
            size_t first = k;
            while (k + 1 < f.samples.size() && f.samples[k + 1] == f.samples[k] + 1) ++k;
            Issue issue = f.issue;
            issue.start = times[f.samples[first]];
            issue.end = times[f.samples[k]] + frame;
            issues.push_back(std::move(issue));
            ++k;
        }
    }

    // ---- sound ----
    bool hasAudio = std::ranges::any_of(p.clips, [&](const Clip& c) {
        const Track* t = p.findTrack(c.track);
        return t && t->kind == TrackKind::Audio && !t->muted;
    });
    if (hasAudio && dur > 0) {
        AudioAnalysisOptions ao;
        ao.step = std::max(dur, 1.0);
        AudioAnalysis a = analyzeMix(s, 0, dur, ao);
        for (const auto& r : a.clipping)
            issues.push_back({"error", "clipping", r.start, r.end, 0, fmt::format("{} samples clip at full scale", r.samples)});
        if (a.integrated && std::abs(*a.integrated - o.lufs) > o.lufsTolerance)
            issues.push_back({"warning", "loudness", 0, dur, 0,
                              fmt::format("integrated loudness is {:.1f} LUFS; the target is {:.0f} ±{:.0f}", *a.integrated, o.lufs, o.lufsTolerance)});
        if (dbOf(a.truePeak) > o.truePeak)
            issues.push_back({"warning", "true-peak", 0, dur, 0, fmt::format("true peak is {:.1f} dBTP; keep it at or below {:.0f}", dbOf(a.truePeak), o.truePeak)});
    }

    std::ranges::stable_sort(issues, [](const Issue& a, const Issue& b) {
        return a.start != b.start ? a.start < b.start : (a.severity == "error") > (b.severity == "error");
    });
    json out = json::array();
    int errors = 0, warnings = 0;
    for (const Issue& i : issues) {
        json j = {{"severity", i.severity}, {"kind", i.kind}, {"start", std::round(i.start * 1e6) / 1e6},
                  {"end", std::round(i.end * 1e6) / 1e6}, {"message", i.message}};
        if (i.clip) j["clip"] = i.clip, j["name"] = clipName(i.clip);
        (i.severity == "error" ? errors : warnings)++;
        out.push_back(std::move(j));
    }
    return {{"issues", out}, {"samples", times.size()}, {"errors", errors}, {"warnings", warnings}};
}

}  // namespace mf
