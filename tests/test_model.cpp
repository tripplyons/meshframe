#include <gtest/gtest.h>

#include "model/edit.hpp"
#include "model/history.hpp"
#include "model/serialize.hpp"

#include <filesystem>

using namespace mf;

namespace {

Id trackNamed(const Project& p, const char* name) { return p.findTrackByName(name)->id; }

Project withAvMedia() {
    Project p = Project::makeDefault();
    MediaSource m;
    m.path = "/tmp/clip.mp4";
    m.hasVideo = m.hasAudio = true;
    m.duration = 10;
    m.width = 640;
    m.height = 360;
    m.fps = 30;
    edit::addMedia(p, m);
    return p;
}

Clip textClip(Id track, double start, double dur) {
    Clip c;
    c.kind = ClipKind::Text;
    c.track = track;
    c.start = start;
    c.duration = dur;
    return c;
}

}  // namespace

TEST(Model, AddMediaCreatesLinkedPair) {
    Project p = withAvMedia();
    auto ids = edit::addMediaClips(p, p.media[0].id, 2.0);
    ASSERT_EQ(ids.size(), 2u);
    const Clip* v = p.findClip(ids[0]);
    const Clip* a = p.findClip(ids[1]);
    EXPECT_EQ(p.findTrack(v->track)->kind, TrackKind::Video);
    EXPECT_EQ(p.findTrack(a->track)->kind, TrackKind::Audio);
    EXPECT_NE(v->link, 0u);
    EXPECT_EQ(v->link, a->link);
    edit::moveClip(p, ids[0], 5.0);
    EXPECT_DOUBLE_EQ(p.findClip(ids[1])->start, 5.0);
}

TEST(Model, RejectsInvalidEditsWithoutChanges) {
    Project p = withAvMedia();
    Project before = p;
    Clip c = textClip(trackNamed(p, "A1"), 0, 1);
    EXPECT_THROW(edit::addClip(p, c), EditError);
    c.track = trackNamed(p, "V1");
    c.duration = 0;
    EXPECT_THROW(edit::addClip(p, c), EditError);
    EXPECT_THROW(edit::setPropConstant(p, 9999, "x", 1.0), EditError);
    EXPECT_EQ(projectToJson(p), projectToJson(before));
}

TEST(Model, PropertiesCoerceAndValidate) {
    Project p = Project::makeDefault();
    Id clip = edit::addClip(p, textClip(trackNamed(p, "V1"), 0, 3));
    edit::setPropConstant(p, clip, "color", std::string("#ff0000"));
    EXPECT_EQ(typeOf(p.findClip(clip)->props["color"].constant), ValueType::Color);
    EXPECT_THROW(edit::setPropConstant(p, clip, "blend", std::string("overlay")), EditError);
    EXPECT_THROW(edit::setKeyframe(p, clip, "text", 0, 1), EditError);
    edit::setKeyframe(p, clip, "opacity", 0, 0);
    edit::setKeyframe(p, clip, "opacity", 1, 1);
    EXPECT_EQ(p.findClip(clip)->props["opacity"].mode, PropMode::Curve);
    EXPECT_DOUBLE_EQ(p.findClip(clip)->props["opacity"].curve.sample(0.5), 0.5);
    edit::removeKeyframe(p, clip, "opacity", 0);
    edit::removeKeyframe(p, clip, "opacity", 1);
    EXPECT_EQ(p.findClip(clip)->props["opacity"].mode, PropMode::Constant);
}

TEST(Model, SplitShiftsSourceAndCurves) {
    Project p = withAvMedia();
    auto ids = edit::addMediaClips(p, p.media[0].id, 0);
    edit::setKeyframe(p, ids[0], "opacity", 4, 0.25);
    auto right = edit::splitClip(p, ids[0], 3.0);
    ASSERT_EQ(right.size(), 2u);
    const Clip* r = p.findClip(right[0]);
    EXPECT_DOUBLE_EQ(r->start, 3.0);
    EXPECT_DOUBLE_EQ(r->in, 3.0);
    EXPECT_DOUBLE_EQ(r->duration, 7.0);
    EXPECT_DOUBLE_EQ(r->props.at("opacity").curve.keys[0].t, 1.0);
    EXPECT_DOUBLE_EQ(p.findClip(ids[0])->duration, 3.0);
    EXPECT_EQ(p.findClip(right[0])->link, p.findClip(right[1])->link);
    EXPECT_NE(p.findClip(right[0])->link, p.findClip(ids[0])->link);
}

TEST(Model, TrimMovesSourceOffset) {
    Project p = withAvMedia();
    auto ids = edit::addMediaClips(p, p.media[0].id, 0);
    edit::trimClip(p, ids[0], 2.0, 5.0);
    EXPECT_DOUBLE_EQ(p.findClip(ids[0])->in, 2.0);
    EXPECT_DOUBLE_EQ(p.findClip(ids[1])->in, 2.0);
    EXPECT_THROW(edit::trimClip(p, ids[0], -1.0, 8.0), EditError);
}

TEST(Model, EffectsAndTransitions) {
    Project p = Project::makeDefault();
    Id v1 = trackNamed(p, "V1");
    Id a = edit::addClip(p, textClip(v1, 0, 2));
    Id b = edit::addClip(p, textClip(v1, 2, 2));
    Id fx = edit::addEffect(p, a, false, "blur");
    Id fx2 = edit::addEffect(p, a, false, "invert");
    EXPECT_THROW(edit::addEffect(p, a, true, "gain"), EditError);
    edit::moveEffect(p, fx2, -1);
    EXPECT_EQ(p.findClip(a)->videoEffects[0].id, fx2);
    edit::setPropConstant(p, fx, "radius", 10.0);
    EXPECT_DOUBLE_EQ(asNumber(p.findEffect(fx)->props["radius"].constant), 10.0);
    Id tr = edit::addTransition(p, a, b, "wipe", 0.5);
    EXPECT_THROW(edit::addTransition(p, b, a, "crossfade", 0.5), EditError);
    edit::setPropConstant(p, tr, "direction", std::string("up"));
    edit::removeClip(p, b);
    EXPECT_EQ(p.findTransition(tr), nullptr);
    Id master = edit::addEffect(p, 0, true, "compressor");
    EXPECT_EQ(p.masterAudio.size(), 1u);
    edit::removeEffect(p, master);
    EXPECT_TRUE(p.masterAudio.empty());
}

TEST(Model, JsonRoundTrip) {
    Project p = withAvMedia();
    auto ids = edit::addMediaClips(p, p.media[0].id, 1);
    Id t = edit::addClip(p, textClip(trackNamed(p, "V2"), 0, 4));
    edit::setPropExpr(p, t, "x", "sin(t) * 100");
    edit::setKeyframe(p, t, "opacity", 0, 0, Interp::Bezier);
    edit::setKeyframe(p, t, "opacity", 1, 1);
    edit::addEffect(p, ids[1], true, "lowpass");
    edit::setSignal(p, "beat", Property::expression("floor(t * 2)"));
    Id t2 = edit::addClip(p, textClip(trackNamed(p, "V2"), 4, 4));
    edit::addTransition(p, t, t2, "dip", 1);
    json j = projectToJson(p);
    Project q = projectFromJson(j);
    EXPECT_EQ(projectToJson(q), j);
    EXPECT_EQ(q.findClip(t)->props["x"].expr, "sin(t) * 100");
    EXPECT_EQ(q.nextId, p.nextId);

    auto dir = std::filesystem::temp_directory_path() / "mf_test_roundtrip";
    std::filesystem::create_directories(dir);
    auto file = (dir / "p.mfproj").string();
    saveProject(p, file);
    Project r = loadProject(file);
    EXPECT_EQ(projectToJson(r), j);
    std::filesystem::remove_all(dir);
}

TEST(Model, HistoryUndoRedo) {
    Project p = Project::makeDefault();
    History h;
    auto apply = [&](const char* label, auto fn) {
        Project before = p;
        fn();
        h.push(label, std::move(before));
    };
    Id clip = 0;
    apply("add", [&] { clip = edit::addClip(p, textClip(trackNamed(p, "V1"), 0, 2)); });
    apply("move", [&] { edit::moveClip(p, clip, 3); });
    apply("effect", [&] { edit::addEffect(p, clip, false, "blur"); });
    EXPECT_EQ(h.undo(p), "effect");
    EXPECT_TRUE(p.findClip(clip)->videoEffects.empty());
    EXPECT_EQ(h.undo(p), "move");
    EXPECT_DOUBLE_EQ(p.findClip(clip)->start, 0);
    EXPECT_EQ(h.redo(p), "move");
    EXPECT_DOUBLE_EQ(p.findClip(clip)->start, 3);
    EXPECT_EQ(h.undo(p), "move");
    EXPECT_EQ(h.undo(p), "add");
    EXPECT_EQ(p.findClip(clip), nullptr);
    EXPECT_FALSE(h.canUndo());
    apply("other", [&] { edit::addTrack(p, TrackKind::Video); });
    EXPECT_FALSE(h.canRedo());
}
