// FastGIS fork (2026-09-26): Norway's and Denmark's untagged tracks are open to
// motor vehicles; a rider may avoid them with avoid_national_default_tracks.
#include "mjolnir/adminconstants.h"
#include "sif/costfactory.h"
#include "worker.h"

#include <gtest/gtest.h>

using namespace valhalla;

namespace {

constexpr size_t kTrackColumn = 2; // trunk, trunk_link, TRACK, footway, ...

TEST(NationalTrackDefault, NorwayAndDenmarkTracksKeepTheLuaDefaults) {
  for (const char* country : {"Norway", "Denmark", "Sweden", "Finland"}) {
    auto it = mjolnir::kCountryAccess.find(country);
    ASSERT_NE(it, mjolnir::kCountryAccess.end()) << country;
    EXPECT_EQ(it->second.at(kTrackColumn), -1) << country;
  }
  // Norway still restricts footways/cycleways to non-motor modes.
  EXPECT_NE(mjolnir::kCountryAccess.at("Norway").at(3), -1);
}

TEST(NationalTrackDefault, OptionParsesAndDefaultsToInclude) {
  for (const char* costing : {"auto", "motorcycle"}) {
    Api on, off;
    ParseApi(std::string(R"({"locations":[{"lat":60,"lon":5},{"lat":60.1,"lon":5.1}],"costing":")") +
                 costing + R"(","costing_options":{")" + costing +
                 R"(":{"avoid_national_default_tracks":true}}})",
             Options::route, on);
    ParseApi(std::string(R"({"locations":[{"lat":60,"lon":5},{"lat":60.1,"lon":5.1}],"costing":")") +
                 costing + R"("})",
             Options::route, off);
    const auto& ton = on.options().costings().begin()->second.options();
    const auto& toff = off.options().costings().begin()->second.options();
    EXPECT_TRUE(ton.avoid_national_default_tracks()) << costing;
    EXPECT_FALSE(toff.avoid_national_default_tracks()) << costing;
  }
}

} // namespace
