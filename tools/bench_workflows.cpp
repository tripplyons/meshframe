#include "session/session.hpp"
#include "session/diff.hpp"
#include "media/scenes.hpp"
#include "model/serialize.hpp"
#include "session/recovery.hpp"
#include <filesystem>
#include <chrono>
#include <iostream>
using namespace mf;
int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "edit";
    int count = argc > 2 ? std::stoi(argv[2]) : 100;
    Project p = Project::makeDefault();
    for (int i = 0; i < 2000; ++i) {
        Clip c; c.id = 100 + i; c.track = p.tracks.front().id; c.name = "clip" + std::to_string(i);
        c.start = i; c.duration = 1; p.clips.push_back(c);
    }
    if (mode == "lookup") {
        for (int i = 0; i < 1999; ++i) {
            Transition tr; tr.id = 10000 + i; tr.clipA = 100 + i; tr.clipB = 101 + i;
            p.transitions.push_back(tr);
        }
    }
    Session s;
    s.apply("setup", [&](Project& out) { out = p; });
    if (mode.starts_with("preview")) { std::string err; if (!s.open("bench/projects/text_paths.mfproj", &err)) { std::cerr << err; return 1; } }
    Project after = p; after.clips.back().name = "changed";
    Evaluator ev;
    p.script = "mf.synth('tone', function(t) return math.sin(t * 440 * 2 * math.pi) end)";
    ev.setProject(p);
    Clip synth; synth.kind = ClipKind::Synth;
    synth.props["synth"].constant = std::string("tone");
    std::vector<float> audio(4096 * 2);
    Recovery recovery("build-ar/workflow-round4/recovery-bench");
    VideoDecoder video;
    if (mode.starts_with("decode")) {
        std::string error;
        if (!video.open(mode.find("long") != std::string::npos ? "build-ar/workflow-round4/long-gop.mp4" : "bench/media/clip_b.mp4", 1920, 1080, &error)) return 1;
    }
    uint64_t checksum = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        if (mode == "edit") s.apply("rename", [&](Project& out) { out.clips.back().name = std::to_string(i); });
        else if (mode.starts_with("preview")) {
            double t = i / 30.0;
            s.renderFrame(t, 960, 540);
            auto frame = mode == "preview-reuse" ? s.analysisFrame(t, 320, 180) : s.renderFrame(t, 320, 180);
            checksum += frame->px[100];
        }
        else if (mode.starts_with("decode")) {
            double t = mode.find("long") != std::string::npos ? (i % 5) * 2.5 :
                mode.find("seek") == std::string::npos ? (i % 90) / 30.0 : ((i * 37) % 90) / 30.0;
            auto frame = video.frameAt(t); if (!frame) return 1;
            checksum += frame->px[100];
        }
        else if (mode == "recovery") { recovery.update(s, i * 11.0); }
        else if (mode == "lookup") { for (const auto& c : ev.project().clips) checksum += ev.clipVisibleAt(c, .25); }
        else if (mode == "evaluate") { ev.setProject(p); checksum += ev.graph().size(); }
        else if (mode == "diff") checksum += diffProjects(p, after).dump().size();
        else if (mode == "synth") { std::string err; if (!ev.renderSynth(synth, i * 4096.0 / 48000, 4096, audio.data(), &err)) { std::cerr << err; return 1; } checksum += uint64_t(std::abs(audio[123]) * 100000); }
        else if (mode == "scenes") { std::vector<SceneCut> cuts; std::string err; if (!detectCuts("bench/media/clip_b.mp4", 4, .1, cuts, &err)) return 1; checksum += cuts.size(); }
    }
    std::cout << mode << " " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count() << " ms " << checksum << "\n";
}
