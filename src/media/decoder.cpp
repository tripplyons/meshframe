#include "media/decoder.hpp"

#include "core/work_counter.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

namespace mf {

namespace {

std::string avError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

bool isStillImage(const AVFormatContext* fmt, const AVStream* st) {
    std::string_view name = fmt->iformat->name;
    if (name.find("image2") != std::string_view::npos || name.ends_with("_pipe")) return true;
    switch (st->codecpar->codec_id) {
        case AV_CODEC_ID_PNG:
        case AV_CODEC_ID_MJPEG:
        case AV_CODEC_ID_BMP:
        case AV_CODEC_ID_TIFF:
        case AV_CODEC_ID_WEBP:
            return st->nb_frames <= 1;
        default: return false;
    }
}

struct FormatCloser {
    void operator()(AVFormatContext* f) const { avformat_close_input(&f); }
};
using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;

FormatPtr openFormat(const std::string& path, std::string* err) {
    AVFormatContext* fmt = nullptr;
    int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        if (err) *err = fmt::format("cannot open {}: {}", path, avError(rc));
        return nullptr;
    }
    FormatPtr out(fmt);
    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        if (err) *err = fmt::format("cannot read streams of {}: {}", path, avError(rc));
        return nullptr;
    }
    return out;
}

}  // namespace

bool probeMedia(const std::string& path, MediaSource& out, std::string* err) {
    av_log_set_level(AV_LOG_ERROR);
    FormatPtr fmt = openFormat(path, err);
    if (!fmt) return false;
    int v = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int a = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    // Embedded cover art is not a video stream for editing.
    if (v >= 0 && (fmt->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC)) v = -1;
    out.path = path;
    out.hasVideo = v >= 0;
    out.hasAudio = a >= 0;
    if (!out.hasVideo && !out.hasAudio) {
        if (err) *err = path + " has no video or audio stream";
        return false;
    }
    double duration = fmt->duration > 0 ? double(fmt->duration) / AV_TIME_BASE : 0;
    if (v >= 0) {
        AVStream* st = fmt->streams[v];
        out.width = st->codecpar->width;
        out.height = st->codecpar->height;
        AVRational r = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
        out.fps = r.num && r.den ? av_q2d(r) : 30;
        if (isStillImage(fmt.get(), st)) {
            out.kind = MediaKind::Image;
            out.hasAudio = false;
            duration = 0;
        } else {
            out.kind = MediaKind::Video;
            if (duration <= 0 && st->duration > 0) duration = st->duration * av_q2d(st->time_base);
        }
    } else {
        out.kind = MediaKind::Audio;
    }
    if (a >= 0 && out.hasAudio) {
        AVStream* st = fmt->streams[a];
        out.sampleRate = st->codecpar->sample_rate;
        out.channels = st->codecpar->ch_layout.nb_channels;
        if (duration <= 0 && st->duration > 0) duration = st->duration * av_q2d(st->time_base);
    }
    out.duration = duration;
    if (out.name.empty()) {
        auto slash = path.find_last_of('/');
        out.name = slash == std::string::npos ? path : path.substr(slash + 1);
    }
    return true;
}

std::shared_ptr<AudioBuffer> decodeAudio(const std::string& path, int sampleRate, std::string* err) {
    av_log_set_level(AV_LOG_ERROR);
    FormatPtr fmt = openFormat(path, err);
    if (!fmt) return nullptr;
    int si = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (si < 0) {
        if (err) *err = path + " has no audio stream";
        return nullptr;
    }
    AVStream* st = fmt->streams[si];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        if (err) *err = "no decoder for audio in " + path;
        return nullptr;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        if (err) *err = "cannot open audio decoder for " + path;
        return nullptr;
    }
    SwrContext* swr = nullptr;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout inLayout;
    if (ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&inLayout, ctx->ch_layout.nb_channels);
    else av_channel_layout_copy(&inLayout, &ctx->ch_layout);
    swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, sampleRate, &inLayout, ctx->sample_fmt, ctx->sample_rate, 0,
                        nullptr);
    // Mono should play at unity on both channels, not at the -3 dB default.
    if (swr && ctx->ch_layout.nb_channels == 1) {
        const double unity[2] = {1.0, 1.0};
        swr_set_matrix(swr, unity, 1);
    }
    av_channel_layout_uninit(&inLayout);
    if (!swr || swr_init(swr) < 0) {
        swr_free(&swr);
        avcodec_free_context(&ctx);
        if (err) *err = "cannot set up resampler for " + path;
        return nullptr;
    }
    auto buf = std::make_shared<AudioBuffer>();
    buf->sampleRate = sampleRate;
    if (st->duration > 0) buf->samples.reserve(size_t(st->duration * av_q2d(st->time_base) * sampleRate * 2 + 4096));
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto drain = [&](AVFrame* f) {
        int maxOut = swr_get_out_samples(swr, f ? f->nb_samples : 0);
        if (maxOut <= 0) return;
        size_t old = buf->samples.size();
        buf->samples.resize(old + size_t(maxOut) * 2);
        uint8_t* outPtr = reinterpret_cast<uint8_t*>(buf->samples.data() + old);
        int got = swr_convert(swr, &outPtr, maxOut, f ? const_cast<const uint8_t**>(f->extended_data) : nullptr,
                              f ? f->nb_samples : 0);
        buf->samples.resize(old + size_t(std::max(got, 0)) * 2);
    };
    auto receive = [&] {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            drain(frame);
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(fmt.get(), pkt) >= 0) {
        if (pkt->stream_index == si && avcodec_send_packet(ctx, pkt) >= 0) receive();
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, nullptr);
    receive();
    drain(nullptr);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&ctx);

    size_t frames = buf->frames();
    size_t blocks = (frames + AudioBuffer::kPeakBlock - 1) / AudioBuffer::kPeakBlock;
    buf->peaks.resize(blocks * 2);
    for (size_t b = 0; b < blocks; ++b) {
        float lo = 0, hi = 0;
        size_t end = std::min(frames, (b + 1) * AudioBuffer::kPeakBlock);
        for (size_t i = b * AudioBuffer::kPeakBlock; i < end; ++i) {
            float m = 0.5f * (buf->samples[i * 2] + buf->samples[i * 2 + 1]);
            lo = std::min(lo, m);
            hi = std::max(hi, m);
        }
        buf->peaks[b * 2] = lo;
        buf->peaks[b * 2 + 1] = hi;
    }
    buf->buildPeakPyramid();
    return buf;
}

void AudioBuffer::buildPeakPyramid() {
    peakPyramid_.clear();
    const std::vector<float>* source = &peaks;
    while (source->size() / 2 > 16) {
        std::vector<float> level(((source->size() / 2 + 15) / 16) * 2);
        for (size_t group = 0; group < level.size() / 2; ++group) {
            float lo = 0, hi = 0;
            for (size_t i = group * 16; i < std::min(source->size() / 2, (group + 1) * 16); ++i) {
                lo = std::min(lo, (*source)[i * 2]);
                hi = std::max(hi, (*source)[i * 2 + 1]);
            }
            level[group * 2] = lo;
            level[group * 2 + 1] = hi;
        }
        peakPyramid_.push_back(std::move(level));
        source = &peakPyramid_.back();
    }
}

std::pair<float, float> AudioBuffer::peakRange(size_t first, size_t last) const {
    last = std::min(last, peaks.size() / 2);
    float lo = 0, hi = 0;
    while (first < last) {
        size_t level = 0, stride = 1;
        while (level < peakPyramid_.size() && stride <= (last - first) / 16 && first % (stride * 16) == 0) {
            stride *= 16;
            ++level;
        }
        const auto& values = level ? peakPyramid_[level - 1] : peaks;
        lo = std::min(lo, values[first / stride * 2]);
        hi = std::max(hi, values[first / stride * 2 + 1]);
        first += stride;
    }
    return {lo, hi};
}

VideoDecoder::~VideoDecoder() {
    sws_freeContext(sws_);
    av_frame_free(&cur_);
    av_frame_free(&next_);
    av_packet_free(&pkt_);
    avcodec_free_context(&codec_);
    if (fmt_) avformat_close_input(&fmt_);
}

bool VideoDecoder::open(const std::string& path, int maxW, int maxH, std::string* err) {
    av_log_set_level(AV_LOG_ERROR);
    FormatPtr fmt = openFormat(path, err);
    if (!fmt) return false;
    stream_ = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_ < 0) {
        if (err) *err = path + " has no video stream";
        return false;
    }
    AVStream* st = fmt->streams[stream_];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        if (err) *err = "no decoder for video in " + path;
        return false;
    }
    codec_ = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(codec_, st->codecpar);
    codec_->thread_count = 0;  // let FFmpeg pick
    codec_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (avcodec_open2(codec_, dec, nullptr) < 0) {
        if (err) *err = "cannot open video decoder for " + path;
        avcodec_free_context(&codec_);
        return false;
    }
    timeBase_ = av_q2d(st->time_base);
    startTime_ = st->start_time != AV_NOPTS_VALUE ? st->start_time * timeBase_ : 0;
    AVRational r = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    frameDur_ = r.num && r.den ? 1.0 / av_q2d(r) : 1.0 / 30;
    std::tie(outW_, outH_) = outputSize(codec_->width, codec_->height, maxW, maxH);
    pkt_ = av_packet_alloc();
    cur_ = av_frame_alloc();
    next_ = av_frame_alloc();
    fmt_ = fmt.release();
    return true;
}

std::pair<int, int> VideoDecoder::outputSize(int sourceW, int sourceH, int maxW, int maxH) {
    double scale = std::min({1.0, double(maxW) / sourceW, double(maxH) / sourceH});
    return {std::max(2, int(std::lround(sourceW * scale)) & ~1),
            std::max(2, int(std::lround(sourceH * scale)) & ~1)};
}

bool VideoDecoder::decodeNext(AVFrame* into) {
    for (;;) {
        int rc = avcodec_receive_frame(codec_, into);
        if (rc == 0) {
            work::addDecode(1);
            return true;
        }
        if (rc == AVERROR_EOF) {
            eof_ = true;
            return false;
        }
        if (rc != AVERROR(EAGAIN)) return false;
        rc = av_read_frame(fmt_, pkt_);
        if (rc < 0) {
            avcodec_send_packet(codec_, nullptr);
            continue;
        }
        if (pkt_->stream_index == stream_) avcodec_send_packet(codec_, pkt_);
        av_packet_unref(pkt_);
    }
}

void VideoDecoder::seek(double t) {
    int64_t ts = int64_t((t + startTime_) / timeBase_);
    av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codec_);
    av_frame_unref(cur_);
    av_frame_unref(next_);
    haveCur_ = haveNext_ = eof_ = false;
    curPts_ = -1e9;
    curImage_.reset();
}

ImagePtr VideoDecoder::frameAt(double t) {
    if (!fmt_) return nullptr;
    constexpr double eps = 1e-4;
    auto frameEnd = [&] {
        if (cur_->duration > 0 && cur_->best_effort_timestamp != AV_NOPTS_VALUE)
            return (cur_->best_effort_timestamp + cur_->duration) * timeBase_ - startTime_;
        return curPts_ + frameDur_;
    };
    bool inCur = haveCur_ && t + eps >= curPts_ && t < frameEnd() - eps;
    if (inCur) {
        if (!curImage_) curImage_ = convert(cur_);
        return curImage_;
    }
    // Decode forward when the target is close ahead; otherwise seek.
    bool forwardSeek = t > curPts_ + 2.0;
    if (haveCur_ && forwardSeek) {
        // A known intervening keyframe lets us skip directly to the target's
        // GOP. Without one, keep decoding forward rather than replaying the
        // same GOP from its beginning (important for long-GOP recordings).
        AVStream* stream = fmt_->streams[stream_];
        int index = av_index_search_timestamp(stream,
            int64_t((t + startTime_) / timeBase_), AVSEEK_FLAG_BACKWARD);
        if (index >= 0) {
            const AVIndexEntry* entry = avformat_index_get_entry(stream, index);
            if (entry) forwardSeek = entry->timestamp * timeBase_ - startTime_ > curPts_ + frameDur_;
        }
    }
    if (!haveCur_ || t < curPts_ || forwardSeek) seek(t);
    bool advanced = false;
    for (;;) {
        if (!haveNext_) {
            if (eof_ || !decodeNext(next_)) break;
            haveNext_ = true;
        }
        int64_t pts = next_->best_effort_timestamp;
        double ft = pts == AV_NOPTS_VALUE ? curPts_ + frameDur_ : pts * timeBase_ - startTime_;
        if (haveCur_ && ft > t + eps) break;
        av_frame_unref(cur_);
        av_frame_move_ref(cur_, next_);
        haveNext_ = false;
        haveCur_ = true;
        curPts_ = ft;
        advanced = true;
    }
    if (!haveCur_) return nullptr;
    if (eof_ && t >= frameEnd()) return nullptr;
    if (advanced || !curImage_) curImage_ = convert(cur_);
    return curImage_;
}

ImagePtr VideoDecoder::convert(AVFrame* f) {
    sws_ = sws_getCachedContext(sws_, f->width, f->height, AVPixelFormat(f->format), outW_, outH_, AV_PIX_FMT_RGBA,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return nullptr;
    // Honor the frame's matrix and range; untagged HD is almost always BT.709.
    int cs = SWS_CS_DEFAULT;
    switch (f->colorspace) {
        case AVCOL_SPC_BT709: cs = SWS_CS_ITU709; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: cs = SWS_CS_BT2020; break;
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_BT470BG: cs = SWS_CS_ITU601; break;
        default: cs = f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
    }
    const AVPixFmtDescriptor* fd = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    bool rgb = fd && (fd->flags & AV_PIX_FMT_FLAG_RGB);
    if (!rgb) {
        int srcFull = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        sws_setColorspaceDetails(sws_, sws_getCoefficients(cs), srcFull, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                                 1 << 16, 1 << 16);
    }
    work::addScaled(uint64_t(outW_) * uint64_t(outH_));
    auto img = std::make_shared<Image>(Image::forOverwrite(outW_, outH_));
    uint8_t* dst[4] = {img->px.data(), nullptr, nullptr, nullptr};
    int stride[4] = {outW_ * 4, 0, 0, 0};
    if (sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, stride) != outH_) return nullptr;
    // Sources with alpha arrive straight; the compositor wants premultiplied.
    if (fd && (fd->flags & AV_PIX_FMT_FLAG_ALPHA)) {
        for (size_t i = 0; i < img->px.size(); i += 4) {
            unsigned a = img->px[i + 3];
            for (int c = 0; c < 3; ++c) img->px[i + c] = uint8_t((img->px[i + c] * a + 127) / 255);
        }
    }
    return img;
}

}  // namespace mf
