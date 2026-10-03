#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "core/fuzzy.h"

// Expected values are worked out by hand from the rules in fuzzy.h (fzf's constants):
// match 16, boundary bonus 8 (x2 on the first pattern char), camel bonus 7, a run keeps
// the bonus of its start (at least 4), gap -3 then -1 per further char.

TEST_CASE("no match returns -1")
{
    CHECK(fuzzy_score("xyz", "EngineData.EngineSpeed") == -1);
    CHECK(fuzzy_score("engspd", "EngineData.OilPressure") == -1); // "e n g s" but no p after the s
    CHECK(fuzzy_score("ab", "") == -1);
    CHECK(fuzzy_score("ba", "ab") == -1); // order matters
}

TEST_CASE("empty pattern matches everything with score 0")
{
    std::vector<int> pos{1, 2};
    CHECK(fuzzy_score("", "EngineSpeed", &pos) == 0);
    CHECK(pos.empty());
    CHECK(fuzzy_score("", "") == 0);
}

TEST_CASE("engspd finds EngineSpeed and scores the tight window")
{
    std::vector<int> pos;
    // Forward scan ends at the final 'd' (21); the backward scan starts the window at the
    // second "Engine" (11). E: 16+2*8, n,g: 16+8 each (run), "ine": -3-1-1,
    // S: 16+7 (camel), p: 16+7 (run keeps 7), "ee": -3-1, d: 16  => 133.
    CHECK(fuzzy_score("engspd", "EngineData.EngineSpeed", &pos) == 133);
    CHECK(pos == std::vector<int>{11, 12, 13, 17, 18, 21});
    CHECK(fuzzy_score("ENGSPD", "EngineData.EngineSpeed") == 133); // case-insensitive
    CHECK(fuzzy_score("engspd", "EngineData.EngineSpeed") > fuzzy_score("engspd", "EngineData.OilPressure"));
}

TEST_CASE("exact prefix beats a scattered match")
{
    // "EngineSpeed": 16+16, 16+8, 16+8 = 80.
    // "ExhaustNoxGain": E 16+16, N 16+7, G 16+7, gaps "xhaust" -3-5 and "ox" -3-1 = 66.
    CHECK(fuzzy_score("eng", "EngineSpeed") == 80);
    CHECK(fuzzy_score("eng", "ExhaustNoxGain") == 66);
    CHECK(fuzzy_score("eng", "EngineSpeed") > fuzzy_score("eng", "ExhaustNoxGain"));
}

TEST_CASE("camelCase and separator boundaries beat mid-word matches")
{
    // "EngineSpeed": S 16+2*7, p 16+7 = 53. "EngineDisplay": s mid-word 16, p 16+4 = 36.
    CHECK(fuzzy_score("sp", "EngineSpeed") == 53);
    CHECK(fuzzy_score("sp", "EngineDisplay") == 36);
    // After '_' is a boundary (8) like after '.'; mid-word is 0.
    CHECK(fuzzy_score("o", "eng_oil") == 16 + 16);
    CHECK(fuzzy_score("o", "engoil") == 16);
    CHECK(fuzzy_score("o", "Engine.oil") == fuzzy_score("o", "eng_oil"));
}

TEST_CASE("consecutive matches beat gapped ones")
{
    // "rpm" in "EngRpm": R camel 16+2*7, p,m run 16+7 each = 76.
    // "RearPump": the backward scan starts at the mid-word 'r' of "Rear": r 16, P camel 16+7,
    // "u" -3, m 16 = 52.
    CHECK(fuzzy_score("rpm", "EngRpm") == 76);
    CHECK(fuzzy_score("rpm", "RearPump") == 52);
}

TEST_CASE("smart_find: ripgrep smart case")
{
    CHECK(smart_find("Tentacle1.Angle", "angle") == 10); // lower-case pattern: any case
    CHECK(smart_find("Tentacle1.angle", "Angle") == std::string_view::npos); // upper-case: exact
    CHECK(smart_find("Tentacle1.Angle", "Angle") == 10);
    CHECK(smart_find("abc", "") == 0);
    CHECK(smart_find("abc", "x") == std::string_view::npos);
}
