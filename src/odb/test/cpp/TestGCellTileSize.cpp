// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <array>
#include <stdexcept>
#include <vector>

#include "gtest/gtest.h"
#include "helper.h"
#include "odb/db.h"

namespace odb {
namespace {

class TestGCellTileSize : public SimpleDbFixture
{
 protected:
  void SetUp() override { createSimpleDB(); }

  dbTech* tech() { return db_->getTech(); }
  dbBlock* block() { return db_->getChip()->getBlock(); }

  // Creates a frontside ROUTING layer with a track grid of the given pitch.
  // Layers are numbered in creation order (dbTechLayer::getRoutingLevel()),
  // matching how set_routing_layers/dbBlock::setMaxRoutingLayer resolve a
  // layer name.
  dbTechLayer* makeRoutingLayer(const char* name, int pitch)
  {
    dbTechLayer* layer
        = dbTechLayer::create(tech(), name, dbTechLayerType::ROUTING);
    layer->setDirection(dbTechLayerDir::HORIZONTAL);
    dbTrackGrid* track = dbTrackGrid::create(block(), layer);
    track->addGridPatternY(0, 1000, pitch);
    return layer;
  }

  // Creates a backside ROUTING layer with no track grid -- getGCellTileSize
  // never needs a backside layer's track grid, only its presence in the
  // raw getRoutingLevel() numbering.
  dbTechLayer* makeBacksideLayer(const char* name)
  {
    dbTechLayer* layer
        = dbTechLayer::create(tech(), name, dbTechLayerType::ROUTING);
    layer->setBackside(true);
    return layer;
  }

  // Creates a frontside ROUTING layer with a track grid made of multiple
  // (origin, count, step) grid patterns, to exercise the multi-pattern
  // getGridY() path instead of the single-pattern one makeRoutingLayer()
  // uses.
  dbTechLayer* makeMultiPatternRoutingLayer(
      const char* name,
      const std::vector<std::array<int, 3>>& patterns)
  {
    dbTechLayer* layer
        = dbTechLayer::create(tech(), name, dbTechLayerType::ROUTING);
    layer->setDirection(dbTechLayerDir::HORIZONTAL);
    dbTrackGrid* track = dbTrackGrid::create(block(), layer);
    for (const auto& [origin, count, step] : patterns) {
      track->addGridPatternY(origin, count, step);
    }
    return layer;
  }

  // Creates a frontside ROUTING layer with NO track grid, to exercise the
  // floor loop's early-call tolerance (getGCellTileSize() can run from
  // gpl's routability check before every layer is tracked).
  dbTechLayer* makeUntrackedRoutingLayer(const char* name)
  {
    dbTechLayer* layer
        = dbTechLayer::create(tech(), name, dbTechLayerType::ROUTING);
    layer->setDirection(dbTechLayerDir::HORIZONTAL);
    return layer;
  }

  // Creates a frontside ROUTING layer with a track grid but a never-set
  // (NONE) direction, to exercise the floor loop's direction dispatch
  // error path.
  dbTechLayer* makeNoDirectionRoutingLayer(const char* name, int pitch)
  {
    dbTechLayer* layer
        = dbTechLayer::create(tech(), name, dbTechLayerType::ROUTING);
    // Deliberately no layer->setDirection() call.
    dbTrackGrid* track = dbTrackGrid::create(block(), layer);
    track->addGridPatternY(0, 1000, pitch);
    return layer;
  }

  // Creates a frontside ROUTING layer with a track grid containing only a
  // single track (no adjacent-gap to floor against).
  dbTechLayer* makeSingleTrackRoutingLayer(const char* name)
  {
    dbTechLayer* layer
        = dbTechLayer::create(tech(), name, dbTechLayerType::ROUTING);
    layer->setDirection(dbTechLayerDir::HORIZONTAL);
    dbTrackGrid* track = dbTrackGrid::create(block(), layer);
    track->addGridPatternY(0, 1, 100);
    return layer;
  }
};

// gt2n's real (public, BSD-3-Clause: github.com/azadnaeemi/GT2N) tech LEF
// pitches, src/techlib/gt2_fspdn_tech.lef, converted to DBU at gt2n's own
// DATABASE MICRONS 2000: M2 0.024um=48, M3 0.028um=56, M4 0.042um=84,
// M12/M13 0.72um=1440.
constexpr int kGt2nM2Pitch = 48;
constexpr int kGt2nM3Pitch = 56;
constexpr int kGt2nM4Pitch = 84;
constexpr int kGt2nTopLayerPitch = 1440;  // M12/M13

// Baseline behavior is unchanged: with a typical bottom-to-top pitch
// progression and no backside layers, the tile size is still the median of
// M2/M3/M4 track spacing times 15 pitches, and the new per-layer floor loop
// (M4..top) doesn't perturb it because M4 is included in the baseline set
// and no higher layer is wider.
TEST_F(TestGCellTileSize, BaselineMedianOfM2M3M4Unchanged)
{
  makeRoutingLayer("M1", kGt2nM3Pitch);
  makeRoutingLayer("M2", kGt2nM2Pitch);
  makeRoutingLayer("M3", kGt2nM3Pitch);
  makeRoutingLayer("M4", kGt2nM4Pitch);
  block()->setMaxRoutingLayer(4);

  // median(48, 56, 84) = 56
  EXPECT_EQ(block()->getGCellTileSize(), kGt2nM3Pitch * 15);
}

// Reproduces the original bug with gt2n's actual pitches (M5-M11 omitted
// for test minimality -- only the M2-M4 baseline and the top enabled
// layer's own pitch matter to getGCellTileSize()). Before the fix, the
// tile size was sized only from M2-M4 and could end up smaller than a
// single top-layer track pitch, so some GCells got zero top-layer tracks
// and DRT-0406 ("No tracks found") fired the moment global routing
// assigned a wire to one of those tiles. The fix floors the tile size at
// the pitch of every enabled layer at/above M4, not just the top one.
//
// One pitch is enough here, not a padded margin: DRT's real track
// assignment (FlexTAWorker::getTrackIdx, src/drt/src/ta/FlexTA_assign.cpp)
// treats a GCell's track range as half-open, so any span of at least one
// pitch is guaranteed a track regardless of the grid's phase. See the
// comment in dbBlock.cpp for the full argument, including why a 2x margin
// (an earlier version of this fix) is provably unnecessary and was
// overly aggressive on ordinary technology stacks.
TEST_F(TestGCellTileSize, FloorsTileSizeForWideTopLayerPitch)
{
  makeRoutingLayer("M1", kGt2nM3Pitch);
  makeRoutingLayer("M2", kGt2nM2Pitch);
  makeRoutingLayer("M3", kGt2nM3Pitch);
  makeRoutingLayer("M4", kGt2nM4Pitch);
  makeRoutingLayer("M12", kGt2nTopLayerPitch);
  block()->setMaxRoutingLayer(5);

  // Baseline: median(48, 56, 84) * 15 = 840, smaller than one M12/M13
  // pitch (1440) -- exactly the real failure mode. The floor requires one
  // top-layer pitch (1440), which dominates and matches gt2n's real M12
  // pitch exactly.
  EXPECT_EQ(block()->getGCellTileSize(), kGt2nTopLayerPitch);
}

// The floor must check every enabled layer from M4 up to the top, not just
// the top layer, because pitch does not necessarily increase monotonically
// with layer index (unlike gt2n's real stack, which is monotonic -- this
// is a deliberately constructed edge case the real motivating technology
// doesn't exercise, so it stays synthetic rather than using real pitches).
// Here the widest layer (M5) sits *below* the top layer (M6), which is
// comparatively narrow -- a "check only the top layer" implementation (an
// earlier version of this fix) would miss M5 entirely and leave the same
// DRT-0406 failure mode on M5.
TEST_F(TestGCellTileSize, FloorConsidersEveryLayerNotJustTheTop)
{
  makeRoutingLayer("M1", 60);
  makeRoutingLayer("M2", 80);
  makeRoutingLayer("M3", 90);
  makeRoutingLayer("M4", 100);
  makeRoutingLayer("M5", 3000);  // widest layer, not the top
  makeRoutingLayer("M6", 200);   // top layer, narrower than M5
  block()->setMaxRoutingLayer(6);

  // Baseline: median(80, 90, 100) * 15 = 1350.
  // Top-layer-only floor would give max(1350, 200) = 1350 -- still
  // smaller than M5's own pitch (3000), reproducing DRT-0406 on M5.
  // Checking every layer M4..M6 gives max(1350, 100, 3000, 200) = 3000,
  // which covers M5.
  EXPECT_EQ(block()->getGCellTileSize(), 3000);
}

// Regression test for a second bug found while generalizing the fix, in the
// already-merged backside-skip code this patch builds on (not introduced by
// this patch): max_routing_layer_ is a *raw* routing level that counts
// backside layers, while the M2/M3/M4 lookup counts frontside layers only.
// On a BSPDN tech with backside layers ordered ahead of the frontside stack
// (gt2n's LEF ordering), a raw max_routing_layer_ can be >= 4 even though
// only one frontside layer is actually enabled. The old code would then
// take the M2/M3/M4 branch, fail to find a 2nd/3rd frontside layer
// (nullptr), and crash dereferencing tech_layer->getName() on that null
// pointer. This fix counts *enabled frontside* layers explicitly so the
// single-layer branch is taken correctly instead.
TEST_F(TestGCellTileSize, BacksideLayersDoNotSkewEnabledLayerCount)
{
  makeBacksideLayer("BPR");
  makeBacksideLayer("BM1");
  makeBacksideLayer("BM2");
  makeBacksideLayer("BRDL");
  dbTechLayer* m1 = makeRoutingLayer("M1", kGt2nM3Pitch);
  ASSERT_EQ(m1->getRoutingLevel(), 5);  // 4 backside + M1

  // Raw routing level 5 -- would fail the old "< 4" check even though only
  // one frontside layer (M1) is enabled.
  block()->setMaxRoutingLayer(5);

  EXPECT_EQ(block()->getGCellTileSize(), kGt2nM3Pitch * 15);
}

// Pins the QoR regression an earlier version of this fix caused (a flat 2x
// floor, applied to every enabled layer at/above M4), using nangate45's
// real tech LEF pitches (test/Nangate45/Nangate45_tech.lef, DATABASE
// MICRONS 2000): metal2 0.19um=380, metal3 0.14um=280, metal4 0.28um=560,
// metal9/metal10 1.6um=3200. nangate45's real M2-4-to-M9/M10 ratio is
// 3200/median(380,280,560) = 3200/380 ~= 8.4x, which exceeds
// pitches_in_tile / 2 = 7.5x, so a 2x floor forced the tile size up on an
// ordinary, already-working stack -- confirmed against the real suite: a
// 2x floor broke 39/165 //src/grt/test targets, including plain
// nangate45 tests (gcd-tcl_test, congestion1-tcl_test) with no gt2n
// involvement at all. A 1x floor only overrides the baseline when a
// layer's pitch exceeds pitches_in_tile (15x) -- 8.4x does not -- so this
// stack must come out unfloored, matching nangate45's real, long-working
// behavior.
TEST_F(TestGCellTileSize, DoesNotFloorOrdinaryNangate45Stack)
{
  makeRoutingLayer("M1", 280);
  makeRoutingLayer("M2", 380);
  makeRoutingLayer("M3", 280);
  makeRoutingLayer("M4", 560);
  makeRoutingLayer("M9", 3200);
  block()->setMaxRoutingLayer(5);

  // Baseline: median(380, 280, 560) * 15 = 380 * 15 = 5700, comfortably
  // >= one top-layer pitch (3200), so the floor must be a no-op.
  EXPECT_EQ(block()->getGCellTileSize(), 380 * 15);
}

// The floor must use the true worst-case gap between adjacent tracks, not
// dbTrackGrid::getAverageTrackSpacing()'s averaged pitch. For a layer with
// more than one grid pattern, _dbTrackGrid::getAverageTrackPattern()
// computes ceil(span / count) -- a mean over every track on the layer, not
// a guaranteed minimum periodicity. Here M5 has two widely separated
// two-track patterns: {0, 50} and {10050, 10100}. The true worst gap
// (10000, between 50 and 10050) is far larger than the averaged pitch
// (ceil(10100 / 4) = 2525) that an earlier, average-based version of this
// fix would have floored against -- which would have left the tile small
// enough to land entirely inside the real 10000-wide gap, reproducing
// DRT-0406 despite the floor appearing to have "done something" (2525 >
// the 1350 baseline).
TEST_F(TestGCellTileSize, FloorUsesWorstGapNotAveragedPitch)
{
  makeRoutingLayer("M1", 60);
  makeRoutingLayer("M2", 80);
  makeRoutingLayer("M3", 90);
  makeRoutingLayer("M4", 100);
  makeMultiPatternRoutingLayer("M5", {{0, 2, 50}, {10050, 2, 50}});
  block()->setMaxRoutingLayer(5);

  // Baseline: median(80, 90, 100) * 15 = 1350.
  // True M5 track coordinates: {0, 50, 10050, 10100}; worst gap = 10000.
  EXPECT_EQ(block()->getGCellTileSize(), 10000);
}

// getGCellTileSize() must fail loudly, not silently, when called before
// setMaxRoutingLayer() -- max_routing_layer_ defaults to -1, and silently
// treating that as "every frontside layer is enabled" would size the tile
// for a routing-layer range the caller never actually requested.
TEST_F(TestGCellTileSize, ErrorsWhenMaxRoutingLayerNotSet)
{
  makeRoutingLayer("M1", 60);
  makeRoutingLayer("M2", 80);
  makeRoutingLayer("M3", 90);
  makeRoutingLayer("M4", 100);
  // Deliberately no block()->setMaxRoutingLayer() call.

  try {
    block()->getGCellTileSize();
    FAIL() << "Expected ODB-1219";
  } catch (const std::exception& e) {
    EXPECT_STREQ(e.what(), "ODB-1219");
  } catch (...) {
    FAIL() << "Unexpected exception (other than ODB-1219)";
  }
}

// 0 is likewise never a real dbTechLayer::getRoutingLevel() value (routing
// levels start at 1), so it must be rejected the same way -1 is, not
// treated as "every frontside layer enabled" (the enabled_frontside_layers
// counting loop's break condition never fires for 0, since its raw_level
// counter starts incrementing from 1).
TEST_F(TestGCellTileSize, ErrorsWhenMaxRoutingLayerIsZero)
{
  makeRoutingLayer("M1", 60);
  makeRoutingLayer("M2", 80);
  makeRoutingLayer("M3", 90);
  makeRoutingLayer("M4", 100);
  block()->setMaxRoutingLayer(0);

  try {
    block()->getGCellTileSize();
    FAIL() << "Expected ODB-1219";
  } catch (const std::exception& e) {
    EXPECT_STREQ(e.what(), "ODB-1219");
  } catch (...) {
    FAIL() << "Unexpected exception (other than ODB-1219)";
  }
}

// Pins the floor loop's early-call tolerance: getGCellTileSize() can run
// from global placement's routability-driven congestion check
// (gpl::RouteBase -> grt::GlobalRouter::initRoutingLayers) before every
// enabled layer's track grid has been created. A layer with no track grid
// yet must be silently skipped by the floor, not error, and the baseline
// (M2-M4) result must still come through unperturbed.
TEST_F(TestGCellTileSize, SkipsFloorForLayerWithNoTrackGridYet)
{
  makeRoutingLayer("M1", kGt2nM3Pitch);
  makeRoutingLayer("M2", kGt2nM2Pitch);
  makeRoutingLayer("M3", kGt2nM3Pitch);
  makeRoutingLayer("M4", kGt2nM4Pitch);
  makeUntrackedRoutingLayer("M5");
  block()->setMaxRoutingLayer(5);

  // Baseline: median(48, 56, 84) * 15 = 840. M5 has no track grid, so the
  // floor loop skips it instead of erroring or crashing.
  EXPECT_EQ(block()->getGCellTileSize(), kGt2nM3Pitch * 15);
}

// A routing layer whose direction was never set (dbTechLayerDir::NONE)
// must error explicitly in the floor loop's direction dispatch, matching
// dbTrackGrid::getAverageTrackSpacing()'s own dispatch for the same case
// (ODB-416, "Layer {} has invalid direction."), rather than silently
// falling into the getGridX() branch and reading the wrong axis.
TEST_F(TestGCellTileSize, ErrorsOnLayerWithNoDirectionSet)
{
  makeRoutingLayer("M1", 60);
  makeRoutingLayer("M2", 80);
  makeRoutingLayer("M3", 90);
  makeRoutingLayer("M4", 100);
  makeNoDirectionRoutingLayer("M5", 3000);
  block()->setMaxRoutingLayer(5);

  try {
    block()->getGCellTileSize();
    FAIL() << "Expected ODB-1220";
  } catch (const std::exception& e) {
    EXPECT_STREQ(e.what(), "ODB-1220");
  } catch (...) {
    FAIL() << "Unexpected exception (other than ODB-1220)";
  }
}

// An odd max_track_gap must round *up* to the nearest even value, not
// down: rounding down would shave a floor that's a hard lower bound (not
// a heuristic estimate) below the true required minimum, undermining the
// exact half-open-span guarantee the floor relies on.
TEST_F(TestGCellTileSize, OddMaxTrackGapRoundsUpNotDown)
{
  makeRoutingLayer("M1", 6);
  makeRoutingLayer("M2", 6);
  makeRoutingLayer("M3", 6);
  makeRoutingLayer("M4", 6);
  makeRoutingLayer("M5", 101);  // odd pitch
  block()->setMaxRoutingLayer(5);

  // Baseline: median(6, 6, 6) * 15 = 90. M5's true gap is 101 (odd); if
  // rounded down (matching getAverageTrackSpacing()'s baseline rounding)
  // the floor would be 101, still above the 90 baseline. Rounded up
  // instead, it's 102.
  EXPECT_EQ(block()->getGCellTileSize(), 102);
}

// The small-stack (<4 frontside layers) branch has nothing to fall back
// on if its sole top layer has fewer than two tracks -- unlike the main
// floor loop, which can skip that layer and still return a baseline. A
// tile_size of 0 here would divide-by-zero downstream
// (grt::GlobalRouter::initGrid()'s dx / tile_size); this must error
// instead.
TEST_F(TestGCellTileSize, ErrorsOnInsufficientTracksInSmallStack)
{
  makeSingleTrackRoutingLayer("M1");
  makeSingleTrackRoutingLayer("M2");
  block()->setMaxRoutingLayer(2);

  try {
    block()->getGCellTileSize();
    FAIL() << "Expected ODB-1221";
  } catch (const std::exception& e) {
    EXPECT_STREQ(e.what(), "ODB-1221");
  } catch (...) {
    FAIL() << "Unexpected exception (other than ODB-1221)";
  }
}

// max_routing_layer_ can resolve to a raw level whose every ROUTING layer
// up to it is backside (a BSPDN stack with no frontside layer enabled at
// all). This must error with a clear message, not fall through to a
// generic "layer <null> not found".
TEST_F(TestGCellTileSize, ErrorsWhenNoFrontsideLayerEnabled)
{
  dbTechLayer* b1 = makeBacksideLayer("BPR");
  makeBacksideLayer("BM1");
  ASSERT_EQ(b1->getRoutingLevel(), 1);
  block()->setMaxRoutingLayer(2);  // both enabled layers are backside

  try {
    block()->getGCellTileSize();
    FAIL() << "Expected ODB-1222";
  } catch (const std::exception& e) {
    EXPECT_STREQ(e.what(), "ODB-1222");
  } catch (...) {
    FAIL() << "Unexpected exception (other than ODB-1222)";
  }
}

// The small-stack (<4 frontside layers) branch switched from
// getAverageTrackSpacing()'s averaged pitch to getMaxTrackGap()'s true
// worst-case gap, the same fix FloorUsesWorstGapNotAveragedPitch pins for
// the main floor loop -- but every other small-stack test uses a
// single-pattern grid where the two coincide, so that switch is only
// exercised here, with a genuine multi-pattern grid.
TEST_F(TestGCellTileSize, SmallStackUsesWorstGapNotAveragedPitch)
{
  makeMultiPatternRoutingLayer("M1", {{0, 2, 50}, {10050, 2, 50}});
  block()->setMaxRoutingLayer(1);

  // True M1 track coordinates: {0, 50, 10050, 10100}; worst gap = 10000.
  // The averaged pitch (ceil(10100/4) = 2525, rounded down to 2524)
  // would give 2524 * 15 = 37860 instead.
  EXPECT_EQ(block()->getGCellTileSize(), 10000 * 15);
}

// The small-stack branch's boundary-shrink term
// (max_track_gap + 2*getBoundaryShrink(layer)) must actually be able to
// win over its own 15x-pitch side of the max(), the same way the main
// floor loop's comment notes "nothing guarantees [15x] covers it in
// general" -- an unusually large via relative to its own layer's fine
// pitch. M1 here has
// a fine 40 DBU pitch (15x = 600) but an artificially huge default via
// enclosure to M2 (2x boundary shrink = 2,000,000), so the boundary term
// must dominate.
TEST_F(TestGCellTileSize, SmallStackBoundaryShrinkCanDominate15xPitch)
{
  dbTechLayer* m1 = makeRoutingLayer("M1", 40);
  m1->setRectOnly(true);

  dbTechLayer* cut = dbTechLayer::create(tech(), "V1CUT", dbTechLayerType::CUT);
  dbTechLayer* m2 = dbTechLayer::create(tech(), "M2", dbTechLayerType::ROUTING);
  m2->setDirection(dbTechLayerDir::VERTICAL);

  dbTechVia* via = dbTechVia::create(tech(), "V1_0");
  dbBox::create(via, cut, -10, -10, 10, 10);
  dbBox::create(via, m1, -50, -50, 50, 50);
  dbBox::create(via, m2, -1000000, -1000000, 1000000, 1000000);

  block()->setMaxRoutingLayer(1);  // only M1 enabled -- small-stack branch

  // 15x pitch: 40 * 15 = 600. Boundary shrink: merged enclosure height is
  // 2,000,000 DBU (M1 is HORIZONTAL), so single-end shrink is 1,000,000,
  // doubled for the both-ends case: 2,000,000. Required:
  // 40 + 2,000,000 = 2,000,040, which dominates 600.
  EXPECT_EQ(block()->getGCellTileSize(), 40 + 2 * 1000000);
}

// The small-stack branch's own upper boundary
// (enabled_frontside_layers == upper_layer_for_gcell_size - 1, i.e.
// exactly 3) -- every other small-stack test uses 1 or 2 layers only.
TEST_F(TestGCellTileSize, SmallStackHandlesExactlyThreeFrontsideLayers)
{
  makeRoutingLayer("M1", 60);
  makeRoutingLayer("M2", 80);
  makeRoutingLayer("M3", 90);
  block()->setMaxRoutingLayer(3);

  // Small-stack branch uses M3's own gap (90), not a median of 3 layers
  // (that path needs 4 enabled layers). 90 * 15 = 1350.
  EXPECT_EQ(block()->getGCellTileSize(), 90 * 15);
}

// The floor loop's per-layer requirement must include the die-boundary
// via-overhang margin (getBoundaryShrink), not just the interior-case
// track gap, for a layer DRT treats as unidirectional. Uses gt2n's real
// M12 pitch and its real M12-M13 via geometry (VIA V12_0: M12 RECT
// -0.200/-0.180 to 0.200/0.180, M13 RECT -0.180/-0.800 to 0.180/0.800,
// converted to DBU at gt2n's DATABASE MICRONS 2000) to confirm this
// against the exact numbers that motivated the fix, not invented ones.
// Only one candidate via exists here, so this alone doesn't prove the
// via-selection logic picks correctly when there's a real choice to make
// -- see FloorPicksSingleCutDefaultViaAmongMultipleCandidates for that.
TEST_F(TestGCellTileSize, FloorIncludesBoundaryShrinkForUnidirectionalLayer)
{
  makeRoutingLayer("M1", kGt2nM3Pitch);
  makeRoutingLayer("M2", kGt2nM2Pitch);
  makeRoutingLayer("M3", kGt2nM3Pitch);
  makeRoutingLayer("M4", kGt2nM4Pitch);
  dbTechLayer* m12 = makeRoutingLayer("M12", kGt2nTopLayerPitch);
  m12->setRectOnly(true);  // gt2n's real M12 carries LEF58_RECTONLY

  dbTechLayer* m13
      = dbTechLayer::create(tech(), "M13", dbTechLayerType::ROUTING);
  m13->setDirection(dbTechLayerDir::VERTICAL);

  dbTechVia* via = dbTechVia::create(tech(), "V12_0");
  dbBox::create(via, m12, -400, -360, 400, 360);    // M12 RECT, in DBU
  dbBox::create(via, m13, -360, -1600, 360, 1600);  // M13 RECT, in DBU

  block()->setMaxRoutingLayer(5);  // M1-M4, M12 enabled; M13 is not

  // Baseline: median(48, 56, 84) * 15 = 840. M12's own interior
  // requirement is its pitch, 1440. The default M12-M13 via's merged
  // enclosure is 800 DBU wide, 3200 DBU tall; M12 is HORIZONTAL, so the
  // single-end boundary shrink is half the height: 1600. DRT can shrink a
  // GCell from both ends independently (see getBoundaryShrink()'s
  // comment), so the floor uses 2x that margin. Required for M12:
  // 1440 + 2*1600 = 4640, which dominates the 840 baseline.
  EXPECT_EQ(block()->getGCellTileSize(), 1440 + 2 * 1600);
}

// getBoundaryShrink()'s above/below physical-adjacency scan (and
// tech_top_layer) must skip backside layers even when one is interleaved
// *between* two frontside layers, not just when backside layers all
// precede the frontside stack (gt2n's real layout, and every other test in
// this file). DRT's own frTech never contains a backside layer at all
// (io::Parser::setLayers(), src/drt/src/io/io.cpp filters them out before
// computing its own top-layer/adjacency notions), so treating one as "the
// layer above" here would pick the wrong neighbor. Same setup as
// FloorIncludesBoundaryShrinkForUnidirectionalLayer (M12 RECTONLY,
// default M12-M13 via), except a backside layer is created between M12 and
// M13 -- with no via connecting M12 to that backside layer. Before this
// fix, the above-scan would land on the backside layer first, find no
// connecting via, and silently return a 0 boundary shrink instead of using
// the real M12-M13 via -- reproducing the same die-boundary zero-track
// failure this whole fix exists to close, just for a different (but
// plausible) backside-layer ordering than gt2n's.
TEST_F(TestGCellTileSize, BoundaryShrinkSkipsInterleavedBacksideLayer)
{
  makeRoutingLayer("M1", kGt2nM3Pitch);
  makeRoutingLayer("M2", kGt2nM2Pitch);
  makeRoutingLayer("M3", kGt2nM3Pitch);
  makeRoutingLayer("M4", kGt2nM4Pitch);
  dbTechLayer* m12 = makeRoutingLayer("M12", kGt2nTopLayerPitch);
  m12->setRectOnly(true);

  makeBacksideLayer("BM_INTERLEAVED");  // no via connects to this

  dbTechLayer* m13
      = dbTechLayer::create(tech(), "M13", dbTechLayerType::ROUTING);
  m13->setDirection(dbTechLayerDir::VERTICAL);

  dbTechVia* via = dbTechVia::create(tech(), "V12_0");
  dbBox::create(via, m12, -400, -360, 400, 360);
  dbBox::create(via, m13, -360, -1600, 360, 1600);

  block()->setMaxRoutingLayer(5);  // M1-M4, M12 enabled; M13 is not

  // Identical math to FloorIncludesBoundaryShrinkForUnidirectionalLayer:
  // 1440 + 2*1600 = 4640. A buggy above-scan that doesn't skip the
  // interleaved backside layer would instead get 1440 + 0 = 1440.
  EXPECT_EQ(block()->getGCellTileSize(), 1440 + 2 * 1600);
}

// When multiple candidate vias exist between the same two layers,
// getBoundaryShrink() must pick the one DRT's own selection
// (io::Parser::initDefaultVias(), src/drt/src/io/io_parser_helper.cpp)
// would: fewer cut shapes first (single-cut strongly preferred over a
// multi-cut array via), then the LEF DEFAULT flag as a tiebreak among
// equal cut counts. This is the case dbBlock::getDefaultVias() can't be
// trusted for (it's keyed on an unrelated "OR_DEFAULT" LEF property, and
// falls back to iteration order with no cut-count or DEFAULT-flag
// weighting at all when nothing is OR_DEFAULT-tagged -- the common case
// for an unmodified vendor LEF). Three candidate vias between M12 and
// M13, deliberately not in cut-count/default order in creation order, so
// picking "the first created" or "the OR_DEFAULT-tagged one" (there is
// none) would silently pick wrong:
//   - a 2-cut array via with a huge enclosure (should lose: not
//     single-cut)
//   - a 1-cut via, not marked DEFAULT, with a small enclosure (should
//     lose the cut-count tie to the one below via the DEFAULT flag)
//   - a 1-cut via, marked DEFAULT, with gt2n's real V12_0 enclosure
//     (should win)
TEST_F(TestGCellTileSize, FloorPicksSingleCutDefaultViaAmongMultipleCandidates)
{
  makeRoutingLayer("M1", kGt2nM3Pitch);
  makeRoutingLayer("M2", kGt2nM2Pitch);
  makeRoutingLayer("M3", kGt2nM3Pitch);
  makeRoutingLayer("M4", kGt2nM4Pitch);
  dbTechLayer* m12 = makeRoutingLayer("M12", kGt2nTopLayerPitch);
  m12->setRectOnly(true);

  // The cut layer must be created *between* M12 and M13, matching real
  // LEF stacking order: dbBox::create(dbTechVia*, ...) infers a via's
  // top/bottom metal layers from the layer with the highest/lowest
  // creation-order number among all of the via's box layers (cut
  // included), so a cut layer created after both metals would itself be
  // mistaken for the top layer.
  dbTechLayer* cut
      = dbTechLayer::create(tech(), "V12CUT", dbTechLayerType::CUT);

  dbTechLayer* m13
      = dbTechLayer::create(tech(), "M13", dbTechLayerType::ROUTING);
  m13->setDirection(dbTechLayerDir::VERTICAL);

  // 2-cut array via, huge enclosure -- must lose on cut count alone.
  dbTechVia* array_via = dbTechVia::create(tech(), "V12_ARRAY");
  dbBox::create(array_via, cut, -100, -100, 100, 100);
  dbBox::create(array_via, cut, 200, 200, 400, 400);
  dbBox::create(array_via, m12, -10000, -10000, 10000, 10000);
  dbBox::create(array_via, m13, -10000, -10000, 10000, 10000);

  // 1-cut via, not DEFAULT, small enclosure -- ties on cut count with
  // the via below, must lose on the DEFAULT flag.
  dbTechVia* non_default_via = dbTechVia::create(tech(), "V12_ALT");
  dbBox::create(non_default_via, cut, -20, -20, 20, 20);
  dbBox::create(non_default_via, m12, -50, -50, 50, 50);
  dbBox::create(non_default_via, m13, -50, -50, 50, 50);

  // 1-cut via, DEFAULT, gt2n's real V12_0 enclosure -- must win.
  dbTechVia* default_via = dbTechVia::create(tech(), "V12_0");
  default_via->setDefault();
  dbBox::create(default_via, cut, -20, -20, 20, 20);
  dbBox::create(default_via, m12, -400, -360, 400, 360);
  dbBox::create(default_via, m13, -360, -1600, 360, 1600);

  block()->setMaxRoutingLayer(5);

  // Same expected result as FloorIncludesBoundaryShrinkForUnidirectionalLayer
  // (1440 + 2*1600 = 4640) -- proves the right via was picked out of
  // three, not just the only one available.
  EXPECT_EQ(block()->getGCellTileSize(), 1440 + 2 * 1600);
}

// getBoundaryShrink() must also apply when `layer` is the tech's own
// physical top (no layer above it at all) -- it should look at the via
// connecting it to the layer *below* instead, mirroring DRT's own
// lNum +/- 1 fallback. Every other boundary-shrink test uses a non-top
// enabled layer (M12, with M13 created above it); this test makes the
// enabled top layer also the tech's physical top.
TEST_F(TestGCellTileSize, BoundaryShrinkLooksBelowForTechsPhysicalTopLayer)
{
  makeRoutingLayer("M1", kGt2nM3Pitch);
  makeRoutingLayer("M2", kGt2nM2Pitch);
  makeRoutingLayer("M3", kGt2nM3Pitch);
  dbTechLayer* m4 = makeRoutingLayer("M4", kGt2nM4Pitch);
  m4->setRectOnly(true);
  // No layer created after M4: M4 is both the enabled top layer and the
  // tech's absolute physical top layer.

  dbTechVia* via = dbTechVia::create(tech(), "V3_4");
  dbBox::create(via, tech()->findLayer("M3"), -400, -360, 400, 360);
  dbBox::create(via, m4, -360, -1600, 360, 1600);

  block()->setMaxRoutingLayer(4);

  // Baseline: median(48, 56, 84) * 15 = 840. M4's own gap is 84; its
  // single-end boundary shrink (M4 is HORIZONTAL) is half the merged
  // enclosure height: 1600, doubled for the both-ends case. Required:
  // 84 + 2*1600 = 3284, which dominates.
  EXPECT_EQ(block()->getGCellTileSize(), 84 + 2 * 1600);
}

// If no via at all connects a unidirectional layer to its neighbor,
// getBoundaryShrink() must return 0 (no extra margin) rather than
// erroring or crashing -- the interior-case floor still applies on its
// own.
TEST_F(TestGCellTileSize, BoundaryShrinkIsZeroWithNoConnectingVia)
{
  makeRoutingLayer("M1", kGt2nM3Pitch);
  makeRoutingLayer("M2", kGt2nM2Pitch);
  makeRoutingLayer("M3", kGt2nM3Pitch);
  makeRoutingLayer("M4", kGt2nM4Pitch);
  dbTechLayer* m12 = makeRoutingLayer("M12", kGt2nTopLayerPitch);
  m12->setRectOnly(true);
  // No via created at all, and no M13 above it (M12 is the tech's own
  // physical top too), so there is nothing to floor against.

  block()->setMaxRoutingLayer(5);

  EXPECT_EQ(block()->getGCellTileSize(), kGt2nTopLayerPitch);
}

// max_routing_layer_ set beyond the tech's actual ROUTING layer count is
// invalid input, the same class of problem as -1 or 0 (both already
// guarded), just from the other direction: the enabled_frontside_layers
// counting loop's break condition never fires, and without this check it
// would silently treat every layer in the tech as enabled instead of
// erroring.
TEST_F(TestGCellTileSize, ErrorsWhenMaxRoutingLayerExceedsLayerCount)
{
  makeRoutingLayer("M1", 60);
  makeRoutingLayer("M2", 80);
  makeRoutingLayer("M3", 90);
  makeRoutingLayer("M4", 100);
  block()->setMaxRoutingLayer(5);  // only 4 routing layers exist

  try {
    block()->getGCellTileSize();
    FAIL() << "Expected ODB-1223";
  } catch (const std::exception& e) {
    EXPECT_STREQ(e.what(), "ODB-1223");
  } catch (...) {
    FAIL() << "Unexpected exception (other than ODB-1223)";
  }
}

// The small-stack branch's requireTrackGrid() call still hard-errors with
// the pre-existing ODB-0358 when a found layer has no track grid yet
// (distinct from the main floor loop further down, which tolerates this
// and skips instead -- see SkipsFloorForLayerWithNoTrackGridYet).
TEST_F(TestGCellTileSize, ErrorsOnFoundLayerWithNoTrackGridInSmallStack)
{
  makeUntrackedRoutingLayer("M1");
  block()->setMaxRoutingLayer(1);

  try {
    block()->getGCellTileSize();
    FAIL() << "Expected ODB-0358";
  } catch (const std::exception& e) {
    EXPECT_STREQ(e.what(), "ODB-0358");
  } catch (...) {
    FAIL() << "Unexpected exception (other than ODB-0358)";
  }
}

}  // namespace
}  // namespace odb
