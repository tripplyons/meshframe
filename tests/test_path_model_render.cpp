#include "engine/frame_renderer.hpp"
#include "model/edit.hpp"
#include "model/serialize.hpp"

#include <gtest/gtest.h>

using namespace mf;

namespace {
Project pathProject() {
    Project p = Project::makeDefault();
    p.script.clear();
    p.settings.width = 100; p.settings.height = 100;
    return p;
}
Id pathClip(Project& p, double start = 0) {
    Clip c;
    c.kind = ClipKind::Path;
    c.track = p.findTrackByName("V1")->id;
    c.start = start; c.duration = 2;
    c.props["width"] = Property::of(20.0);
    c.props["height"] = Property::of(20.0);
    c.props["fill"] = Property::of(Color{1, 0, 0, 1});
    c.props["strokeWidth"] = Property::of(0.0);
    return edit::addClip(p, c);
}
struct PathRender {
    MediaCache cache;
    FrameRenderer renderer{cache};
    Evaluator ev;
    std::shared_ptr<Image> frame(const Project& p, double t = 0) {
        ev.setProject(p);
        return renderer.render(ev, t, 100, 100);
    }
};
}

TEST(PathModel, GeometryValidationPreservesProjectOnFailure) {
    Project p = pathProject();
    Id id = pathClip(p);
    EXPECT_THROW(edit::setPropConstant(p, id, "path", std::string("M 0 invalid")), EditError);
    EXPECT_EQ(asString(p.findClip(id)->props.at("path").constant), "");
    EXPECT_THROW(edit::setPropConstant(p, id, "motionPath", std::string("Q")), EditError);
    EXPECT_THROW(edit::setPropConstant(p, id, "width", -1.0), EditError);
    Clip bad = *p.findClip(id);
    bad.props["targetPath"] = Property::of(std::string("M 1"));
    EXPECT_THROW(edit::addClip(p, bad), EditError);
    EXPECT_EQ(p.clips.size(), 1u);
}

TEST(PathModel, PersistenceEffectsAndMotionAnimationInteroperate) {
    Project p = pathProject();
    Id id = pathClip(p);
    edit::setPropConstant(p, id, "motionPath", std::string("M -20 0 L 20 0"));
    edit::setKeyframe(p, id, "motionProgress", 0, 0);
    edit::setKeyframe(p, id, "motionProgress", 2, 1);
    Id effect = edit::addEffect(p, id, false, "invert");
    p = projectFromJson(projectToJson(p));
    ASSERT_EQ(p.findClip(id)->kind, ClipKind::Path);
    ASSERT_EQ(p.findClip(id)->videoEffects.front().id, effect);
    PathRender r;
    auto a = r.frame(p, 0), b = r.frame(p, 1);
    EXPECT_EQ(a->at(30,50)[1], 255); // Inverted red is cyan.
    EXPECT_EQ(a->at(50,50)[1], 0);
    EXPECT_EQ(b->at(50,50)[1], 255);
    EXPECT_TRUE(r.ev.errors().empty());
}

TEST(PathRender, OffOriginGeometryRotatesAroundObjectOriginAndMasksLocally) {
    Project p = pathProject();
    Id id = pathClip(p);
    edit::setPropConstant(p, id, "path", std::string("M 10 -5 L 30 -5 L 30 5 L 10 5 Z"));
    edit::setPropConstant(p, id, "rotation", 90.0);
    PathRender r;
    auto img = r.frame(p);
    EXPECT_EQ(img->at(50,70)[0], 255);
    EXPECT_EQ(img->at(70,50)[0], 0);
    Id mask = edit::addEffect(p, id, false, "mask");
    edit::setPropConstant(p, mask, "width", 10.0);
    edit::setPropConstant(p, mask, "height", 10.0);
    img = r.frame(p);
    EXPECT_EQ(img->at(50,70)[0], 0); // Mask remains at local origin, not path bounds center.
}

TEST(PathRender, BlurAndShadowExtendPastGeometryBounds) {
    Project p = pathProject();
    Id id = pathClip(p);
    Id blur = edit::addEffect(p, id, false, "blur");
    edit::setPropConstant(p, blur, "radius", 4.0);
    PathRender r;
    auto img = r.frame(p);
    EXPECT_GT(img->at(62,50)[0], 0);
    Id shadow = edit::addEffect(p, id, false, "drop_shadow");
    edit::setPropConstant(p, shadow, "color", Color{0,1,0,1});
    edit::setPropConstant(p, shadow, "opacity", 1.0);
    edit::setPropConstant(p, shadow, "x", 20.0);
    edit::setPropConstant(p, shadow, "blur", 0.0);
    img = r.frame(p);
    EXPECT_GT(img->at(70,56)[1], 0);
}

TEST(PathRender, MalformedLoadedAndAnimatedPathsReportRecoverableErrors) {
    Project p = pathProject();
    Id id = pathClip(p);
    p.findClip(id)->props["path"] = Property::of(std::string("M bad"));
    p = projectFromJson(projectToJson(p));
    PathRender r;
    EXPECT_NO_THROW(r.frame(p));
    EXPECT_FALSE(r.ev.errors().empty());
    edit::setPropExpr(p, id, "path", "lt < 1 and 'M bad' or 'M -5 -5 L 5 -5 L 5 5 Z'");
    r.frame(p, 0);
    EXPECT_FALSE(r.ev.errors().empty());
    r.frame(p, 1.5);
    EXPECT_TRUE(r.ev.errors().empty());
}

TEST(PathRender, LuaPathInferenceMorphTrimAndClipSplit) {
    Project p = pathProject();
    p.script = R"(mf.clip{track='V1',path='M -10 0 L 10 0',stroke='#ffffff',strokeWidth=4,
      fill='#00000000',trimEnd=function() return progress end,duration=2})";
    PathRender r;
    auto img = r.frame(p, 1);
    ASSERT_EQ(r.ev.project().clips.size(), 1u);
    EXPECT_EQ(r.ev.project().clips.front().kind, ClipKind::Path);
    EXPECT_GT(img->at(43,50)[0], 0);
    EXPECT_EQ(img->at(57,50)[0], 0);
    EXPECT_TRUE(r.ev.errors().empty());
    p = pathProject();
    Id id = pathClip(p);
    edit::setPropConstant(p,id,"path",std::string("M -10 -10 L 10 -10 L 10 10 L -10 10 Z"));
    edit::setPropConstant(p,id,"targetPath",std::string("M 10 -10 L 30 -10 L 30 10 L 10 10 Z"));
    edit::setKeyframe(p,id,"morph",0,0);
    edit::setKeyframe(p,id,"morph",2,1);
    auto before = r.frame(p, 1.5);
    auto split = edit::splitClip(p,id,1);
    ASSERT_FALSE(split.empty());
    auto after = r.frame(p, 1.5);
    EXPECT_EQ(before->px, after->px);
    EXPECT_EQ(after->at(65,50)[0],255);
}

TEST(PathRender, ExistingTransitionsBlendPathClips) {
    Project p = pathProject();
    Id a = pathClip(p), b = pathClip(p, 2);
    edit::setPropConstant(p,b,"fill",Color{0,0,1,1});
    edit::addTransition(p,a,b,"crossfade",1);
    PathRender r;
    auto img = r.frame(p,2);
    EXPECT_NEAR(img->at(50,50)[0],128,1);
    EXPECT_NEAR(img->at(50,50)[2],128,1);
}

TEST(PathRender, MotionPathAndOrientationApplyToExistingVisualClips) {
    Project p = pathProject();
    Clip c;
    c.kind = ClipKind::Solid; c.track = p.findTrackByName("V1")->id; c.duration = 2;
    Id id = edit::addClip(p, c);
    edit::setPropConstant(p,id,"color",Color{1,1,1,1});
    edit::setPropConstant(p,id,"scale",0.1);
    edit::setPropConstant(p,id,"motionPath",std::string("M 0 -20 L 0 20"));
    edit::setPropConstant(p,id,"motionProgress",0.0);
    edit::setPropConstant(p,id,"motionRotate",1.0);
    PathRender r;
    auto img = r.frame(p);
    EXPECT_EQ(img->at(50,30)[0],255);
    EXPECT_EQ(img->at(50,50)[0],0);
    EXPECT_TRUE(r.ev.errors().empty());
    edit::setPropExpr(p,id,"motionPath","lt < 1 and 'bad' or 'M 0 0 L 0 20'");
    r.frame(p,0);
    EXPECT_FALSE(r.ev.errors().empty());
    img = r.frame(p,1.5);
    EXPECT_TRUE(r.ev.errors().empty());
    EXPECT_EQ(img->at(50,50)[0],255);
}

TEST(PathRender, StrokeCapsPersistAndRenderThroughProjectInterface) {
    Project p = pathProject();
    Id id = pathClip(p);
    edit::setPropConstant(p,id,"path",std::string("M -10 0 L 10 0"));
    edit::setPropConstant(p,id,"fill",Color{0,0,0,0});
    edit::setPropConstant(p,id,"strokeWidth",8.0);
    edit::setPropConstant(p,id,"strokeLinecap",std::string("butt"));
    edit::setPropConstant(p,id,"strokeLinejoin",std::string("miter"));
    edit::setPropConstant(p,id,"strokeMiterLimit",5.0);
    p = projectFromJson(projectToJson(p));
    EXPECT_EQ(asString(p.findClip(id)->props.at("strokeLinecap").constant),"butt");
    PathRender r;
    auto butt = r.frame(p);
    edit::setPropConstant(p,id,"strokeLinecap",std::string("square"));
    auto square = r.frame(p);
    EXPECT_EQ(butt->at(62,50)[0],0);
    EXPECT_GT(square->at(62,50)[0],0);
    EXPECT_TRUE(r.ev.errors().empty());
}

TEST(PathRender, NonfiniteAnimationReportsErrorAndRecovers) {
    for (const char* prop : {"trimEnd", "morph", "motionProgress"}) {
        Project p = pathProject();
        Id id = pathClip(p);
        edit::setPropConstant(p,id,"motionPath",std::string("M 0 0 L 20 0"));
        edit::setPropExpr(p,id,prop,"lt < 1 and 0/0 or 1");
        PathRender r;
        EXPECT_NO_THROW(r.frame(p,0));
        EXPECT_FALSE(r.ev.errors().empty()) << prop;
        EXPECT_NO_THROW(r.frame(p,1.5));
        EXPECT_TRUE(r.ev.errors().empty()) << prop;
    }
}


TEST(PathRender, ProceduralLineIsVisibleWithDefaultStroke) {
    Project p = pathProject();
    Clip c;
    c.kind = ClipKind::Path;
    c.track = p.findTrackByName("V1")->id;
    c.props["shape"] = Property::of(std::string("line"));
    c.props["width"] = Property::of(40.0);
    c.props["height"] = Property::of(0.0);
    Id id = edit::addClip(p, c);
    EXPECT_DOUBLE_EQ(asNumber(p.findClip(id)->props.at("strokeWidth").constant), 2.0);
    PathRender r;
    auto img = r.frame(p);
    EXPECT_GT(img->at(50,50)[0], 0);
    EXPECT_TRUE(r.ev.errors().empty());
}
