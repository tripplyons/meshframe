#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

// Media fixtures generated once per test run with the ffmpeg CLI. Tests that
// need them skip when ffmpeg is not installed.
namespace fixtures {

inline std::filesystem::path dir() {
    static std::filesystem::path d = [] {
        auto p = std::filesystem::temp_directory_path() / "meshframe-test-fixtures";
        std::filesystem::create_directories(p);
        return p;
    }();
    return d;
}

inline bool haveFfmpeg() {
    static bool ok = std::system("ffmpeg -version >/dev/null 2>&1") == 0;
    return ok;
}

inline std::string make(const std::string& name, const std::string& args) {
    auto path = dir() / name;
    if (!std::filesystem::exists(path)) {
        std::string cmd = "ffmpeg -nostdin -loglevel error -y " + args + " '" + path.string() + "'";
        if (std::system(cmd.c_str()) != 0) return {};
    }
    return path.string();
}

// 3 s, 10 fps, 64x48. Frame N has luma 20 + 7 N, so a frame's index is readable from its pixels.
inline std::string countingVideo() {
    return make("counting.mp4",
                "-f lavfi -i color=black:s=64x48:r=10:d=3 -vf \"geq=lum='20+7*N':cb=128:cr=128\" "
                "-c:v libx264 -qp 0 -g 10 -pix_fmt yuv420p");
}

// 2 s, 440 Hz sine at 44.1 kHz, mono.
inline std::string sineAudio() {
    return make("sine.wav", "-f lavfi -i sine=frequency=440:sample_rate=44100:duration=2");
}

// 1 s of video with a stereo tone track.
inline std::string videoWithAudio() {
    return make("av.mp4",
                "-f lavfi -i testsrc2=s=160x90:r=25:d=1 -f lavfi -i sine=frequency=220:duration=1 "
                "-c:v libx264 -pix_fmt yuv420p -c:a aac -shortest");
}

// 32x16 solid red PNG.
inline std::string redImage() {
    return make("red.png", "-f lavfi -i color=red:s=32x16:d=1 -frames:v 1");
}

// 3 s, 10 fps: 1 s each of red, blue, and white, so cuts fall at 1 s and 2 s.
inline std::string cutsVideo() {
    return make("cuts.mp4",
                "-f lavfi -i color=red:s=64x48:r=10:d=1 -f lavfi -i color=blue:s=64x48:r=10:d=1 "
                "-f lavfi -i color=white:s=64x48:r=10:d=1 -filter_complex \"[0][1][2]concat=n=3:v=1:a=0\" "
                "-c:v libx264 -pix_fmt yuv420p");
}

// 1 s of tone, then 1 s of silence.
inline std::string toneThenSilence() {
    return make("tone_silence.wav", "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=1 -af apad=pad_dur=1");
}

// Spoken English from macOS `say`; empty when `say` is missing.
inline std::string speech() {
    auto path = dir() / "speech.aiff";
    if (!std::filesystem::exists(path)) {
        std::string cmd = "say -o '" + path.string() + "' 'Welcome to the demo. Today we cut the boring part. Then we add a title.' 2>/dev/null";
        if (std::system(cmd.c_str()) != 0) return {};
    }
    return path.string();
}

}  // namespace fixtures
