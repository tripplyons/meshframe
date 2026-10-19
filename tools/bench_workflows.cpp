#include "session/session.hpp"
#include "session/diff.hpp"
#include "media/scenes.hpp"
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
    Project after = p; after.clips.back().name = "changed";
    Evaluator ev;
    p.script = "mf.synth('tone', function(t) return math.sin(t * 440 * 2 * math.pi) end)";
    ev.setProject(p);
    Clip synth; synth.kind = ClipKind::Synth;
    synth.props["synth"].constant = std::string("tone");
    std::vector<float> audio(4096 * 2);
    uint64_t checksum = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        if (mode == "edit") s.apply("rename", [&](Project& out) { out.clips.back().name = std::to_string(i); });
        else if (mode == "lookup") { for (const auto& c : ev.project().clips) checksum += ev.clipVisibleAt(c, .25); }
        else if (mode == "evaluate") { ev.setProject(p); checksum += ev.graph().size(); }
        else if (mode == "diff") checksum += diffProjects(p, after).dump().size();
        else if (mode == "synth") { std::string err; if (!ev.renderSynth(synth, i * 4096.0 / 48000, 4096, audio.data(), &err)) { std::cerr << err; return 1; } checksum += uint64_t(std::abs(audio[123]) * 100000); }
        else if (mode == "scenes") { std::vector<SceneCut> cuts; std::string err; if (!detectCuts("bench/media/clip_b.mp4", 4, .1, cuts, &err)) return 1; checksum += cuts.size(); }
    }
    std::cout << mode << " " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count() << " ms " << checksum << "\n";
}
