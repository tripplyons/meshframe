// Targeted rendering benchmarks. Hashing is outside the timed region.
#include "render/raster.hpp"
#include "render/font.hpp"
#include "render/compositor.hpp"
#include "render/waveform.hpp"
#include "media/decoder.hpp"
#include "media/media_cache.hpp"
#include "audio/analysis.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
using namespace mf;
int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "text";
    const int count = argc > 2 ? std::stoi(argv[2]) : 100;
    auto font = Font::sans();
    Image image(1920, 1080);
    std::string paragraph;
    for (int i=0;i<120;++i) paragraph += "Rendering animated text with outlines and effects. ";
    DrawList list;
    for(int i=0;i<12;++i) { DrawCmd c; c.type=DrawCmd::Type::Circle;c.a=150+i*140;c.b=540;c.c=300;c.color={.7f,.3f,.9f,.4f};list.cmds.push_back(c); }
    AudioAnalysis audio;audio.duration=600;audio.columns.resize(2048);
    for(size_t i=0;i<audio.columns.size();++i) audio.columns[i]={-.5f,.5f};
    VideoDecoder decoder;std::string error;
    MediaCache cache; MediaSource media;
    if(mode=="resize-cache" && !probeMedia("bench/media/clip_b.mp4",media,&error)) return 1;
    if(mode=="decode" && !decoder.open(argc>3?argv[3]:"bench/media/clip_b.mp4",1920,1080,&error)) {std::cerr<<error;return 1;}
    if(mode=="outline") drawText(image,*font,"Outlined animated title",30,120,160,Color::gray(1));
    const Image source = image;
    TextBlock block;
    auto start=std::chrono::steady_clock::now();
    for(int i=0;i<count;++i) {
        if(mode=="text") { fill(image,Color::gray(.2));for(int row=0;row<7;++row) drawText(image,*font,"Animated text and glyph rasterization",20,row*140,100,Color{.8f,.5f,.3f,.7f}); }
        else if(mode=="layout") { block=layoutTextBlock(*font,paragraph,TextLayout{24,1000,1,0}); if(block.lines.empty())return 1; }
        else if(mode=="outline") { image=outlineOf(source,12,Color::gray(1)); }
        else if(mode=="generator") {fill(image,Color::gray(.2));drawList(image,list,1);}
        else if(mode=="waveform") image=drawWaveform(audio,300,{});
        else if(mode=="resize-cache") { auto f=cache.frame(media,.5,1920+(i%8)*16,1080+(i%8)*9);if(!f)return 1;image=*f; }
        else if(mode=="decode") {auto f=decoder.frameAt((i%90+.5)/30.);if(!f)return 1;image=*f;}
        else { std::cerr<<"unknown benchmark";return 1; }
    }
    auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    uint64_t digest=1469598103934665603ull;for(uint8_t c:image.px)digest=(digest^c)*1099511628211ull;
    for(const auto& line:block.lines)for(unsigned char c:line)digest=(digest^c)*1099511628211ull;
    std::cout<<"{\"mode\":\""<<mode<<"\",\"iterations\":"<<count<<",\"seconds\":"<<seconds<<",\"digest\":"<<digest<<"}\n";
}
