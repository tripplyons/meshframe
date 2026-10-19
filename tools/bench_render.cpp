// Render benchmark. Renders every project in bench/projects over a fixed
// window of frames and reports the renderer's deterministic work counters, so
// two runs of the same code report the same numbers no matter how the thread
// pool split the work. Wall clock goes to stderr as a diagnostic only.
//
//   meshframe_bench [--projects DIR] [--frames N] [--expect FILE] [--write FILE]
//
// With --expect FILE, the rendered frames must hash to the digests in FILE, and
// the run fails when they do not. With --write FILE, it writes them instead.

#include "core/work_counter.hpp"
#include "engine/evaluator.hpp"
#include "engine/frame_renderer.hpp"
#include "media/media_cache.hpp"
#include "model/project.hpp"
#include "model/serialize.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace mf;

struct Options {
    int frames = 60;
    std::string projects = "bench/projects";
    std::string expect;
    std::string write;
    bool json = false;
    RenderBackend backend = RenderBackend::Auto;
    std::string backendName = "auto";
    bool compareCpu = false;
};

struct Quality {
    uint64_t channels = 0, changed = 0, error = 0;
    int maxError = 0;
    void compare(const Image& actual, const Image& reference) {
        channels += actual.px.size();
        for (size_t i = 0; i < actual.px.size(); ++i) {
            int delta = std::abs(int(actual.px[i]) - reference.px[i]);
            changed += delta != 0;
            error += delta;
            maxError = std::max(maxError, delta);
        }
    }
};

WorkCounts operator-(const WorkCounts& a, const WorkCounts& b) {
    WorkCounts d;
    d.blendPixels = a.blendPixels - b.blendPixels;
    d.sampleTaps = a.sampleTaps - b.sampleTaps;
    d.mapPixels = a.mapPixels - b.mapPixels;
    d.maskPixels = a.maskPixels - b.maskPixels;
    d.blurSteps = a.blurSteps - b.blurSteps;
    d.copyPixels = a.copyPixels - b.copyPixels;
    d.rasterPixels = a.rasterPixels - b.rasterPixels;
    d.edgeTests = a.edgeTests - b.edgeTests;
    d.glyphPixels = a.glyphPixels - b.glyphPixels;
    d.distancePixels = a.distancePixels - b.distancePixels;
    d.decodeFrames = a.decodeFrames - b.decodeFrames;
    d.scaledPixels = a.scaledPixels - b.scaledPixels;
    d.layerBuilds = a.layerBuilds - b.layerBuilds;
    d.evals = a.evals - b.evals;
    d.chunks = a.chunks - b.chunks;
    return d;
}

// FNV-1a over the frame bytes, with the frame index mixed in so that dropping,
// adding, or reordering frames changes the digest.
uint64_t hashFrame(const Image& img, int index) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint8_t b) { h = (h ^ b) * 1099511628211ull; };
    for (int y = 0; y < img.h; ++y) {
        const uint8_t* row = img.row(y);
        for (int x = 0; x < img.w * 4; ++x) mix(row[x]);
        mix(uint8_t(y));
    }
    for (int i = 0; i < 4; ++i) mix(uint8_t(index >> (8 * i)));
    return h;
}

std::string hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
    return buf;
}

std::map<std::string, std::string> readDigests(const std::string& path) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto space = line.find(' ');
        if (space == std::string::npos) continue;
        out[line.substr(0, space)] = line.substr(space + 1);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--frames") opt.frames = std::stoi(next());
        else if (a == "--projects") opt.projects = next();
        else if (a == "--expect") opt.expect = next();
        else if (a == "--write") opt.write = next();
        else if (a == "--json") opt.json = true;
        else if (a == "--compare-cpu") opt.compareCpu = true;
        else if (a == "--backend") {
            opt.backendName = next();
            if (opt.backendName == "auto") opt.backend = RenderBackend::Auto;
            else if (opt.backendName == "cpu") opt.backend = RenderBackend::Cpu;
            else if (opt.backendName == "metal") opt.backend = RenderBackend::Metal;
            else { std::cerr << "backend must be auto, cpu, or metal\n"; return 2; }
        }
        else {
            std::cerr << "unknown argument " << a << "\n";
            return 2;
        }
    }
    if (opt.backend == RenderBackend::Metal && !gpuAvailable()) {
        std::cerr << "Metal unavailable: " << gpuError() << '\n';
        return 1;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(opt.projects)) {
        if (entry.path().extension() == ".mfproj") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cerr << "no projects in " << opt.projects << "\n";
        return 1;
    }

    resetWorkCounts();
    std::map<std::string, uint64_t> digests;
    std::vector<uint64_t> samples;
    double seconds = 0;
    double renderSeconds = 0;
    double gpuSeconds = 0;
    int gpuFrames = 0;
    Quality quality;
    std::map<std::string, double> projectSeconds;
    for (const auto& file : files) {
        Project project = loadProject(file.string());
        MediaCache cache;
        FrameRenderer renderer{cache, opt.backend};
        MediaCache referenceCache;
        FrameRenderer referenceRenderer{referenceCache, RenderBackend::Cpu};
        Evaluator referenceEv;
        if (opt.compareCpu) referenceEv.setProject(project);
        Evaluator ev;
        ev.setProject(project);
        const double fps = project.settings.fps > 0 ? project.settings.fps : 30;
        const int w = int(project.settings.width), h = int(project.settings.height);
        WorkCounts before = workCounts();
        uint64_t digest = 1469598103934665603ull;
        double renderBefore = renderSeconds;
        auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < opt.frames; ++i) {
            double t = (double(i) + 0.5) / fps;
            auto frameStarted = std::chrono::steady_clock::now();
            auto frame = renderer.render(ev, t, w, h);
            renderSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - frameStarted).count();
            gpuFrames += renderer.usedGpuLastFrame();
            gpuSeconds += renderer.gpuSecondsLastFrame();
            digest = (digest ^ hashFrame(*frame, i)) * 1099511628211ull;
            if (opt.compareCpu) quality.compare(*frame, *referenceRenderer.render(referenceEv, t, w, h));
        }
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        digests[file.stem().string()] = digest;
        samples.push_back((workCounts() - before).total());
        projectSeconds[file.stem().string()] = renderSeconds - renderBefore;
        std::cerr << file.stem().string() << ": " << opt.frames << " frames at " << w << "x" << h
                  << ", render " << renderSeconds - renderBefore << "s\n";
    }
    WorkCounts counts = workCounts();
    if (opt.backend == RenderBackend::Metal && gpuFrames != opt.frames * int(files.size())) {
        std::cerr << "Metal rendering fell back to the CPU\n";
        return 1;
    }
    if (opt.compareCpu) {
        std::cerr << "CPU comparison: maximum channel error " << quality.maxError
                  << ", mean absolute error " << (quality.channels ? double(quality.error) / quality.channels : 0)
                  << ", changed channels " << quality.changed << '/' << quality.channels << '\n';
        if (quality.maxError > 3) { std::cerr << "image difference exceeds three channel levels\n"; return 1; }
    }

    if (!opt.write.empty()) {
        std::ofstream out(opt.write);
        for (const auto& [name, digest] : digests) out << name << ' ' << hex(digest) << '\n';
    }
    if (!opt.expect.empty()) {
        auto want = readDigests(opt.expect);
        for (const auto& [name, digest] : digests) {
            auto it = want.find(name);
            if (it == want.end() || it->second != hex(digest)) {
                std::cerr << name << ": rendered " << hex(digest) << ", expected "
                          << (it == want.end() ? "<missing>" : it->second) << "\n";
                return 1;
            }
        }
        if (want.size() != digests.size()) {
            std::cerr << "expected " << want.size() << " digests, rendered " << digests.size() << "\n";
            return 1;
        }
        std::cerr << "frame digests match " << opt.expect << "\n";
    }

    std::cerr << "seconds " << seconds << " (diagnostic only)\nrender_seconds " << renderSeconds
              << "\ngpu_seconds " << gpuSeconds << "\ngpu_frames " << gpuFrames << '\n' << counts.toString();
    if (!opt.json) return 0;
    std::string json = "{\"aggregate\":" + std::to_string(counts.total()) + ",\"samples\":[";
    for (size_t i = 0; i < samples.size(); ++i) {
        if (i) json += ',';
        json += std::to_string(samples[i]);
    }
    json += "],\"digests\":{";
    bool first = true;
    for (const auto& [name, digest] : digests) {
        if (!first) json += ',';
        first = false;
        json += '"' + name + "\":\"" + hex(digest) + '"';
    }
    json += "},\"work\":" + counts.toJson() + ",\"backend\":\"" + opt.backendName
         + "\",\"render_seconds\":" + std::to_string(renderSeconds) + ",\"gpu_frames\":" + std::to_string(gpuFrames)
         + ",\"gpu_seconds\":" + std::to_string(gpuSeconds)
         + ",\"quality\":{\"compared_cpu\":" + (opt.compareCpu ? "true" : "false")
         + ",\"max_channel_error\":" + std::to_string(quality.maxError)
         + ",\"mean_absolute_error\":" + std::to_string(quality.channels ? double(quality.error) / quality.channels : 0)
         + ",\"changed_channels\":" + std::to_string(quality.changed) + "},\"project_seconds\":{";
    first = true;
    for (const auto& [name, time] : projectSeconds) {
        if (!first) json += ',';
        first = false;
        json += '"' + name + "\":" + std::to_string(time);
    }
    json += "}}";
    std::cout << json << std::endl;
    return 0;
}
