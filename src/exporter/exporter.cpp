#include "exporter/exporter.hpp"

#include "audio/mixer.hpp"
#include "engine/frame_renderer.hpp"
#include "model/chapters.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <condition_variable>
#include <deque>
#include <optional>

namespace mf {

namespace {

std::string avError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

struct Range {
    double start, end;
};

Range rangeOf(const Project& p, const ExportOptions& o) {
    double end = o.end < 0 ? p.duration() : o.end;
    return {std::max(0.0, o.start), end};
}

bool writeWav(Evaluator& ev, AudioMixer& mixer, Range r, const std::string& path, ExportProgress* prog,
              std::string* err) {
    int sr = ev.project().settings.sampleRate;
    int64_t total = int64_t(std::llround((r.end - r.start) * sr));
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        *err = "cannot write " + path;
        return false;
    }
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t dataBytes = uint32_t(total * 4);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);  // PCM
    u16(2);
    u32(uint32_t(sr));
    u32(uint32_t(sr * 4));
    u16(4);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    constexpr int kChunk = 4096;
    std::vector<float> buf(kChunk * 2);
    std::vector<int16_t> pcm(kChunk * 2);
    for (int64_t done = 0; done < total; done += kChunk) {
        if (prog && prog->cancel) {
            std::fclose(f);
            *err = "export canceled";
            return false;
        }
        int n = int(std::min<int64_t>(kChunk, total - done));
        mixer.render(ev, r.start + double(done) / sr, n, buf.data(), true);
        for (int i = 0; i < n * 2; ++i) pcm[i] = int16_t(std::lround(std::clamp(buf[i], -1.0f, 1.0f) * 32767));
        std::fwrite(pcm.data(), 2, size_t(n) * 2, f);
        if (prog) prog->fraction = double(done + n) / std::max<int64_t>(1, total);
    }
    bool ok = std::fclose(f) == 0;
    if (!ok) *err = "error writing " + path;
    return ok;
}

class Mp4Writer {
public:
    ~Mp4Writer() {
        sws_freeContext(sws_);
        av_frame_free(&rgba_);
        av_frame_free(&vframe_);
        av_frame_free(&aframe_);
        av_packet_free(&pkt_);
        avcodec_free_context(&venc_);
        avcodec_free_context(&aenc_);
        if (oc_) {
            if (opened_) avio_closep(&oc_->pb);
            avformat_free_context(oc_);
        }
    }

    bool open(const std::string& path, int w, int h, double fps, int sampleRate, const ExportOptions& o,
              const std::vector<Chapter>& chapters, std::string* err) {
        int rc = avformat_alloc_output_context2(&oc_, nullptr, "mp4", path.c_str());
        if (rc < 0 || !oc_) return fail(err, "cannot create mp4 container", rc);
        pkt_ = av_packet_alloc();

        AVRational rate = av_d2q(fps, 100000);
        // VideoToolbox encodes on the media engine at about twice x264's speed
        // here, so try it first and fall back to software.
        const AVCodec* candidates[] = {avcodec_find_encoder_by_name("h264_videotoolbox"),
                                       avcodec_find_encoder_by_name("libx264"),
                                       avcodec_find_encoder(AV_CODEC_ID_H264), avcodec_find_encoder(AV_CODEC_ID_MPEG4)};
        const AVCodec* vc = nullptr;
        for (const AVCodec* c : candidates)
            if (c && openVideoEncoder(c, w, h, rate, o.crf)) {
                vc = c;
                break;
            }
        if (!vc) return fail(err, "cannot open an H.264 or MPEG-4 encoder", 0);
        vst_ = avformat_new_stream(oc_, nullptr);
        avcodec_parameters_from_context(vst_->codecpar, venc_);
        vst_->time_base = venc_->time_base;
        vst_->avg_frame_rate = rate;

        // AudioToolbox's AAC encoder is about three times faster than FFmpeg's,
        // whose encoding took as long as color conversion in exports.
        const AVCodec* ac = nullptr;
        for (const AVCodec* c : {avcodec_find_encoder_by_name("aac_at"), avcodec_find_encoder(AV_CODEC_ID_AAC)})
            if (c && openAudioEncoder(c, sampleRate, o.audioBitrate)) {
                ac = c;
                break;
            }
        if (!ac) return fail(err, "cannot open an AAC encoder", 0);
        ast_ = avformat_new_stream(oc_, nullptr);
        avcodec_parameters_from_context(ast_->codecpar, aenc_);
        ast_->time_base = aenc_->time_base;
        audioFrameSize_ = aenc_->frame_size > 0 ? aenc_->frame_size : 1024;

        // The mp4 muxer writes these as a QuickTime chapter track and a Nero
        // chpl box; avformat_free_context frees them.
        for (const Chapter& c : chapters) {
            auto* ch = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
            ch->id = oc_->nb_chapters + 1;
            ch->time_base = {1, 1000};
            ch->start = std::llround(c.start * 1000);
            ch->end = std::llround(c.end * 1000);
            av_dict_set(&ch->metadata, "title", c.title.c_str(), 0);
            int count = int(oc_->nb_chapters);
            av_dynarray_add(&oc_->chapters, &count, ch);
            oc_->nb_chapters = unsigned(count);
        }
        if ((rc = avio_open(&oc_->pb, path.c_str(), AVIO_FLAG_WRITE)) < 0) return fail(err, "cannot write " + path, rc);
        opened_ = true;
        if ((rc = avformat_write_header(oc_, nullptr)) < 0) return fail(err, "cannot write mp4 header", rc);

        vframe_ = av_frame_alloc();
        vframe_->format = venc_->pix_fmt;
        vframe_->width = w;
        vframe_->height = h;
        av_frame_get_buffer(vframe_, 0);
        aframe_ = av_frame_alloc();
        aframe_->format = aenc_->sample_fmt;
        aframe_->sample_rate = sampleRate;
        av_channel_layout_copy(&aframe_->ch_layout, &aenc_->ch_layout);
        aframe_->nb_samples = audioFrameSize_;
        av_frame_get_buffer(aframe_, 0);
        vframe_->colorspace = AVCOL_SPC_BT709;
        vframe_->color_range = AVCOL_RANGE_MPEG;
        vframe_->color_primaries = AVCOL_PRI_BT709;
        vframe_->color_trc = AVCOL_TRC_BT709;
        // A dynamic context takes formats and colors from the frames and
        // splits the conversion across threads.
        sws_ = sws_alloc_context();
        rgba_ = av_frame_alloc();
        if (!sws_ || !rgba_) return fail(err, "cannot allocate the color converter", 0);
        sws_->flags = SWS_BICUBIC;
        sws_->threads = 0;
        // Full-range RGB in, limited-range BT.709 out.
        rgba_->format = AV_PIX_FMT_RGBA;
        rgba_->width = w;
        rgba_->height = h;
        rgba_->colorspace = AVCOL_SPC_RGB;
        rgba_->color_range = AVCOL_RANGE_JPEG;
        rgba_->color_primaries = AVCOL_PRI_BT709;
        rgba_->color_trc = AVCOL_TRC_BT709;
        return true;
    }

    int audioFrameSize() const { return audioFrameSize_; }

    bool video(const Image& img, int64_t index, std::string* err) {
        av_frame_make_writable(vframe_);
        rgba_->data[0] = const_cast<uint8_t*>(img.px.data());
        rgba_->linesize[0] = img.w * 4;
        int rc = sws_scale_frame(sws_, vframe_, rgba_);
        if (rc < 0) return fail(err, "cannot convert frame colors", rc);
        vframe_->pts = index;
        return send(venc_, vst_, vframe_, err);
    }

    bool audio(const float* stereo, int frames, std::string* err) {
        av_frame_make_writable(aframe_);
        aframe_->nb_samples = frames;
        if (aenc_->sample_fmt == AV_SAMPLE_FMT_S16) {
            auto* out = reinterpret_cast<int16_t*>(aframe_->data[0]);
            for (int i = 0; i < frames * 2; ++i)
                out[i] = int16_t(std::lround(std::clamp(stereo[i], -1.0f, 1.0f) * 32767));
        } else {
            auto* l = reinterpret_cast<float*>(aframe_->data[0]);
            auto* r = reinterpret_cast<float*>(aframe_->data[1]);
            for (int i = 0; i < frames; ++i) {
                l[i] = stereo[i * 2];
                r[i] = stereo[i * 2 + 1];
            }
        }
        aframe_->pts = audioPts_;
        audioPts_ += frames;
        return send(aenc_, ast_, aframe_, err);
    }

    bool finish(std::string* err) {
        if (!send(venc_, vst_, nullptr, err) || !send(aenc_, ast_, nullptr, err)) return false;
        int rc = av_write_trailer(oc_);
        if (rc < 0) return fail(err, "cannot finish mp4", rc);
        return true;
    }

private:
    // Leaves venc_ open on success and freed on failure.
    bool openVideoEncoder(const AVCodec* vc, int w, int h, AVRational rate, int crf) {
        venc_ = avcodec_alloc_context3(vc);
        venc_->width = w;
        venc_->height = h;
        venc_->time_base = av_inv_q(rate);
        venc_->framerate = rate;
        venc_->pix_fmt = AV_PIX_FMT_YUV420P;
        venc_->gop_size = std::max(1, int(av_q2d(rate) * 2));
        venc_->color_range = AVCOL_RANGE_MPEG;
        venc_->colorspace = AVCOL_SPC_BT709;
        venc_->color_primaries = AVCOL_PRI_BT709;
        venc_->color_trc = AVCOL_TRC_BT709;
        std::string_view name = vc->name;
        if (name == "libx264") {
            venc_->max_b_frames = 2;
            av_opt_set(venc_->priv_data, "preset", "medium", 0);
            av_opt_set_int(venc_->priv_data, "crf", crf, 0);
        } else if (name == "h264_videotoolbox") {
            // Its B-frames come out with DTS after PTS, which the mp4 muxer
            // rejects, and they made files larger, not smaller, in tests.
            // Constant quality q = 116 - 2 * crf matches x264's PSNR at CRF 18.
            venc_->max_b_frames = 0;
            venc_->flags |= AV_CODEC_FLAG_QSCALE;
            venc_->global_quality = std::clamp(116 - 2 * crf, 1, 100) * FF_QP2LAMBDA;
        } else {
            venc_->max_b_frames = 2;
            venc_->bit_rate = int64_t(w) * h * av_q2d(rate) * 0.15;
        }
        if (oc_->oformat->flags & AVFMT_GLOBALHEADER) venc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(venc_, vc, nullptr) >= 0) return true;
        avcodec_free_context(&venc_);
        return false;
    }

    // Leaves aenc_ open on success and freed on failure. Planar float when the
    // encoder takes it, else interleaved 16-bit.
    bool openAudioEncoder(const AVCodec* ac, int sampleRate, int bitrate) {
        const void* formats = nullptr;
        int count = 0;
        aenc_ = avcodec_alloc_context3(ac);
        avcodec_get_supported_config(aenc_, ac, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &formats, &count);
        auto* fmts = static_cast<const AVSampleFormat*>(formats);
        auto supports = [&](AVSampleFormat f) { return !fmts || std::find(fmts, fmts + count, f) != fmts + count; };
        aenc_->sample_fmt = supports(AV_SAMPLE_FMT_FLTP) ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S16;
        aenc_->sample_rate = sampleRate;
        aenc_->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        aenc_->bit_rate = bitrate;
        aenc_->time_base = AVRational{1, sampleRate};
        if (oc_->oformat->flags & AVFMT_GLOBALHEADER) aenc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (supports(aenc_->sample_fmt) && avcodec_open2(aenc_, ac, nullptr) >= 0) return true;
        avcodec_free_context(&aenc_);
        return false;
    }

    bool fail(std::string* err, const std::string& what, int rc) {
        *err = rc ? fmt::format("{}: {}", what, avError(rc)) : what;
        return false;
    }

    bool send(AVCodecContext* enc, AVStream* st, AVFrame* frame, std::string* err) {
        int rc = avcodec_send_frame(enc, frame);
        if (rc < 0 && rc != AVERROR_EOF) return fail(err, "encode failed", rc);
        for (;;) {
            rc = avcodec_receive_packet(enc, pkt_);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) return fail(err, "encode failed", rc);
            av_packet_rescale_ts(pkt_, enc->time_base, st->time_base);
            pkt_->stream_index = st->index;
            rc = av_interleaved_write_frame(oc_, pkt_);
            if (rc < 0) return fail(err, "cannot write packet", rc);
        }
    }

    AVFormatContext* oc_ = nullptr;
    AVCodecContext *venc_ = nullptr, *aenc_ = nullptr;
    AVStream *vst_ = nullptr, *ast_ = nullptr;
    AVFrame *vframe_ = nullptr, *aframe_ = nullptr, *rgba_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* sws_ = nullptr;
    bool opened_ = false;
    int audioFrameSize_ = 1024;
    int64_t audioPts_ = 0;
};

bool writeMp4(Evaluator& ev, MediaCache& cache, AudioMixer& mixer, Range r, const ExportOptions& o,
              ExportProgress* prog, std::string* err) {
    const Project& p = ev.project();
    int w = o.width > 0 ? o.width : p.settings.width;
    int h = o.height > 0 ? o.height : p.settings.height;
    w &= ~1;
    h &= ~1;
    double fps = p.settings.fps;
    int sr = p.settings.sampleRate;
    Mp4Writer out;
    if (!out.open(o.path, w, h, fps, sr, o, chaptersIn(p, r.start, r.end), err)) return false;
    FrameRenderer renderer(cache);
    int64_t frames = std::max<int64_t>(1, int64_t(std::llround((r.end - r.start) * fps)));
    int64_t audioTotal = int64_t(std::llround(double(frames) / fps * sr));
    int64_t audioDone = 0;
    int afs = out.audioFrameSize();
    std::vector<float> abuf(size_t(afs) * 2);
    struct Frame {
        ImagePtr image;
        int64_t index;
        std::vector<float> audio;
    };
    // Keep evaluation and media access on the caller's thread. Only the writer
    // moves to a worker, overlapping conversion/encoding with the next render.
    std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<Frame> queue;
    bool productionDone = false;
    std::string encodeError;
    std::jthread writer([&](std::stop_token stop) {
        auto fail = [&](std::string error) {
            std::lock_guard lock(mutex);
            encodeError = std::move(error);
            ready.notify_all();
        };
        for (;;) {
            Frame frame;
            {
                std::unique_lock lock(mutex);
                if (!ready.wait(lock, stop, [&] { return !queue.empty() || productionDone; })) return;
                if (queue.empty()) break;
                frame = std::move(queue.front());
                queue.pop_front();
                ready.notify_all();
            }
            std::string error;
            if (!out.video(*frame.image, frame.index, &error)) { fail(std::move(error)); return; }
            for (size_t offset = 0; offset < frame.audio.size();) {
                int length = int(std::min(size_t(afs), (frame.audio.size() - offset) / 2));
                if (!out.audio(frame.audio.data() + offset, length, &error)) { fail(std::move(error)); return; }
                offset += size_t(length) * 2;
            }
            if (prog) prog->fraction = double(frame.index + 1) / frames;
        }
        std::string error;
        if (!out.finish(&error)) fail(std::move(error));
    });
    // Begin each frame before finishing the one before it, so the CPU
    // prepares the next frame while the GPU draws the current one.
    std::optional<FrameRenderer::PendingFrame> pending = renderer.begin(ev, r.start, w, h);
    for (int64_t n = 0; n < frames; ++n) {
        if (prog && prog->cancel) {
            *err = "export canceled";
            return false;
        }
        std::optional<FrameRenderer::PendingFrame> next;
        if (n + 1 < frames) next = renderer.begin(ev, r.start + double(n + 1) / fps, w, h);
        Frame frame{renderer.finish(ev, std::move(*pending)), n, {}};
        pending = std::move(next);
        // Keep audio up to the end of this video frame.
        int64_t audioTarget = std::min(audioTotal, int64_t(std::llround(double(n + 1) / fps * sr)));
        while (audioDone < audioTarget) {
            int len = int(std::min<int64_t>(afs, audioTotal - audioDone));
            std::fill(abuf.begin(), abuf.end(), 0.0f);
            mixer.render(ev, r.start + double(audioDone) / sr, len, abuf.data(), true);
            frame.audio.insert(frame.audio.end(), abuf.begin(), abuf.begin() + size_t(len) * 2);
            audioDone += len;
        }
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return queue.empty() || !encodeError.empty(); });
        if (!encodeError.empty()) { *err = encodeError; return false; }
        queue.push_back(std::move(frame));
        ready.notify_all();
    }
    {
        std::lock_guard lock(mutex);
        productionDone = true;
        ready.notify_all();
    }
    writer.join();
    if (!encodeError.empty()) { *err = encodeError; return false; }
    return true;
}

}  // namespace

bool exportProject(const Project& p, const ExportOptions& opts, ExportProgress* progress, std::string* err) {
    std::string localErr;
    if (!err) err = &localErr;
    Range r = rangeOf(p, opts);
    if (r.end <= r.start) {
        *err = "nothing to export: the range is empty";
        return false;
    }
    std::string ext = std::filesystem::path(opts.path).extension().string();
    std::ranges::transform(ext, ext.begin(), ::tolower);
    if (ext != ".mp4" && ext != ".wav") {
        *err = "export path must end in .mp4 or .wav";
        return false;
    }
    av_log_set_level(AV_LOG_ERROR);
    MediaCache cache(size_t(256) << 20);
    AudioMixer mixer(cache);
    Evaluator ev;
    ev.setProject(p);
    if (progress) progress->fraction = 0;
    bool ok = ext == ".wav" ? writeWav(ev, mixer, r, opts.path, progress, err)
                            : writeMp4(ev, cache, mixer, r, opts, progress, err);
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(opts.path, ec);
    }
    return ok;
}

ExportJob::~ExportJob() {
    cancel();
    wait();
}

bool ExportJob::start(const Project& p, ExportOptions opts, std::string* err) {
    if (running_) {
        if (err) *err = "an export is already running";
        return false;
    }
    wait();
    opts_ = opts;
    progress_.fraction = 0;
    progress_.cancel = false;
    finished_ = false;
    running_ = true;
    ++started_;
    {
        std::lock_guard lock(mu_);
        error_.clear();
    }
    thread_ = std::thread([this, p, opts = std::move(opts)] {
        std::string e;
        bool ok = exportProject(p, opts, &progress_, &e);
        {
            std::lock_guard lock(mu_);
            error_ = ok ? "" : e;
        }
        finished_ = true;
        running_ = false;
    });
    return true;
}

void ExportJob::cancel() { progress_.cancel = true; }

void ExportJob::wait() {
    if (thread_.joinable()) thread_.join();
}

std::string ExportJob::error() const {
    std::lock_guard lock(mu_);
    return error_;
}

}  // namespace mf
