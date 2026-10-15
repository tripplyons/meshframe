#include "media/scenes.hpp"

#include "media/decoder.hpp"

#include <cmath>
#include <cstdlib>

namespace mf {

bool detectCuts(const std::string& path, double duration, double threshold, std::vector<SceneCut>& out, std::string* err) {
    VideoDecoder dec;
    if (!dec.open(path, 64, 64, err)) return false;
    std::vector<int> prev, cur;
    double step = dec.frameDuration();
    for (int64_t i = 0;; ++i) {
        double t = (double(i) + 0.5) * step;
        if (t >= duration) break;
        ImagePtr img = dec.frameAt(t);
        if (!img) break;
        cur.resize(size_t(img->w) * img->h * 3);
        for (size_t k = 0; k < cur.size(); ++k) cur[k] = img->px[k / 3 * 4 + k % 3];
        if (prev.size() == cur.size()) {
            double diff = 0;
            for (size_t k = 0; k < cur.size(); ++k) diff += std::abs(cur[k] - prev[k]);
            double score = diff / (255.0 * double(cur.size()));
            if (score > threshold) out.push_back({dec.position(), score});
        }
        prev.swap(cur);
    }
    return true;
}

}  // namespace mf
