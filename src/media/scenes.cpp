#include "media/scenes.hpp"

#include "media/decoder.hpp"

#include <cmath>
#include <cstdlib>

namespace mf {

bool detectCuts(const std::string& path, double duration, double threshold, std::vector<SceneCut>& out, std::string* err) {
    VideoDecoder dec;
    if (!dec.open(path, 64, 64, err)) return false;
    ImagePtr prev;
    double step = dec.frameDuration();
    for (int64_t i = 0;; ++i) {
        double t = (double(i) + 0.5) * step;
        if (t >= duration) break;
        ImagePtr img = dec.frameAt(t);
        if (!img) break;
        if (prev && prev->w == img->w && prev->h == img->h) {
            uint64_t diff = 0;
            const auto* a = img->px.data();
            const auto* b = prev->px.data();
            for (size_t k = 0; k < img->bytes(); k += 4) {
                diff += unsigned(std::abs(int(a[k]) - int(b[k])));
                diff += unsigned(std::abs(int(a[k + 1]) - int(b[k + 1])));
                diff += unsigned(std::abs(int(a[k + 2]) - int(b[k + 2])));
            }
            double score = double(diff) / (255.0 * double(size_t(img->w) * img->h * 3));
            if (score > threshold) out.push_back({dec.position(), score});
        }
        prev = std::move(img);
    }
    return true;
}

}  // namespace mf
