#include "audio/mixer.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>

namespace mf {

void AudioMixer::reset() {
    for (auto& [id, s] : fx_)
        if (s.fx) s.fx->reset();
}

void AudioMixer::runChain(Evaluator& ev, const std::vector<Effect>& chain, float* buf, int frames) {
    int sr = ev.project().settings.sampleRate;
    for (const auto& e : chain) {
        if (!e.enabled) continue;
        FxState& s = fx_[e.id];
        if (!s.fx || s.type != e.type) {
            s.type = e.type;
            s.fx = makeAudioEffect(e.type, sr);
        }
        if (s.fx) s.fx->process(buf, frames, ev.effectParams(e, true));
    }
}

bool AudioMixer::clipSource(Evaluator& ev, const Clip& c, double t0, int i0, int i1, float* buf, bool wait) {
    const Project& p = ev.project();
    double sr = p.settings.sampleRate;
    if (c.kind == ClipKind::Synth) {
        std::string err;
        std::string key = fmt::format("clip:{}", c.id);
        if (!ev.renderSynth(c, t0 + i0 / sr, i1 - i0, buf + i0 * 2, &err)) {
            ev.reportRuntimeError(key, err);
            return false;
        }
        ev.clearRuntimeError(key);
        return true;
    }
    if (c.kind != ClipKind::Media) return false;
    const MediaSource* m = p.findMedia(c.media);
    if (!m || !m->hasAudio) return false;
    auto audio = cache_.audio(*m, p.settings.sampleRate, wait);
    if (!audio || audio->frames() == 0) return false;
    const float* src = audio->samples.data();
    int64_t n = int64_t(audio->frames());
    double pos = (c.in + (t0 + i0 / sr - c.start) * c.speed) * audio->sampleRate;
    double step = c.speed * audio->sampleRate / sr;
    for (int i = i0; i < i1; ++i, pos += step) {
        int64_t k = int64_t(std::floor(pos));
        if (k < 0 || k >= n) continue;
        float f = float(pos - k);
        int64_t k1 = std::min(k + 1, n - 1);
        buf[i * 2] = src[k * 2] + (src[k1 * 2] - src[k * 2]) * f;
        buf[i * 2 + 1] = src[k * 2 + 1] + (src[k1 * 2 + 1] - src[k * 2 + 1]) * f;
    }
    return true;
}

void AudioMixer::renderBlock(Evaluator& ev, double t0, int frames, float* out, bool wait) {
    std::fill_n(out, frames * 2, 0.0f);
    ev.setTime(t0);
    const Project& p = ev.project();
    double sr = p.settings.sampleRate;
    double t1 = t0 + frames / sr;
    clipBuf_.resize(size_t(frames) * 2);
    trackBuf_.resize(size_t(frames) * 2);

    for (const auto& track : p.tracks) {
        if (track.kind != TrackKind::Audio || track.muted) continue;
        bool any = false;
        std::fill(trackBuf_.begin(), trackBuf_.end(), 0.0f);
        for (const auto& c : p.clips) {
            if (c.track != track.id) continue;
            // The clip sounds within its range, extended through any transition window.
            double from = c.start, to = c.end();
            double fade = 1;
            for (const auto& tr : p.transitions) {
                if (tr.clipA != c.id && tr.clipB != c.id) continue;
                auto [ws, we] = Evaluator::window(p, tr);
                from = std::min(from, ws);
                to = std::max(to, we);
                if (t0 >= ws && t0 < we) {
                    double m = std::clamp(ev.transitionMix(tr), 0.0, 1.0);
                    fade *= tr.clipA == c.id ? 1 - m : m;
                }
            }
            if (to <= t0 || from >= t1) continue;
            int i0 = std::clamp(int(std::ceil((from - t0) * sr - 1e-6)), 0, frames);
            int i1 = std::clamp(int(std::ceil((to - t0) * sr - 1e-6)), 0, frames);
            if (i0 >= i1) continue;
            std::fill(clipBuf_.begin(), clipBuf_.end(), 0.0f);
            if (!clipSource(ev, c, t0, i0, i1, clipBuf_.data(), wait)) continue;
            runChain(ev, c.audioEffects, clipBuf_.data(), frames);
            float gl, gr;
            panGains(std::clamp(ev.num(c, "pan"), -1.0, 1.0), gl, gr);
            float g = float(dbToGain(ev.num(c, "gain")) * fade);
            for (int i = 0; i < frames; ++i) {
                trackBuf_[i * 2] += clipBuf_[i * 2] * g * gl;
                trackBuf_[i * 2 + 1] += clipBuf_[i * 2 + 1] * g * gr;
            }
            any = true;
        }
        if (any)
            for (int i = 0; i < frames * 2; ++i) out[i] += trackBuf_[i];
    }
    runChain(ev, p.masterAudio, out, frames);
}

void AudioMixer::render(Evaluator& ev, double t0, int frames, float* out, bool wait) {
    double sr = ev.project().settings.sampleRate;
    double peak[2] = {}, sum[2] = {};
    for (int done = 0; done < frames; done += kBlock) {
        int n = std::min(kBlock, frames - done);
        float* blk = out + done * 2;
        renderBlock(ev, t0 + done / sr, n, blk, wait);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                float v = blk[i * 2 + c];
                if (!std::isfinite(v)) v = blk[i * 2 + c] = 0;
                peak[c] = std::max(peak[c], double(std::abs(v)));
                sum[c] += double(v) * v;
            }
    }
    for (int c = 0; c < 2; ++c) {
        levels_.peak[c] = float(peak[c]);
        levels_.rms[c] = frames ? float(std::sqrt(sum[c] / frames)) : 0.0f;
    }
}

}  // namespace mf
