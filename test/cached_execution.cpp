// =======================================================================================
// This test executes the following graph
//
//    Multiplexer
//      |  |  |
//     A1  |  |
//      |\ |  |
//      | \|  |
//     A2 B1  |
//      |\ |\ |
//      | \| \|
//     A3 B2  C
//
// where A1, A2, and A3 are transforms that execute at the "run" level; B1 and B2 are
// transforms that execute at the "subrun" level; and C is a transform that executes at
// the event level.
//
// This test verifies that for each "run", "subrun", and "event", the corresponding
// transforms execute only once.  This test assumes:
//
//  1 run
//    2 subruns per run
//      5 events per subrun
//
// Note that B1 and B2 rely on the output from A1 and A2; and C relies on the output from
// B1.  However, because the A transforms execute at a different cadence than the B
// transforms (and similar for C), the B transforms must use "cached" data from the A
// transforms.
// =======================================================================================

#include "phlex/core/framework_graph.hpp"
#include "phlex/source.hpp"
#include "test/cached_execution_source.hpp"

#include "catch2/catch_all.hpp"

using namespace phlex::experimental;
using namespace test;

namespace {
  int call_one(int) noexcept { return 1; }
  int call_two(int, int) noexcept { return 2; }
}

TEST_CASE("Cached function calls", "[data model]")
{
  framework_graph g{detail::create_next<cached_execution_source>()};

  g.products("one") =
    g.transform("A1", call_one, concurrency::unlimited).family("number"_in("run"));
  g.products("used_one") =
    g.transform("A2", call_one, concurrency::unlimited).family("one"_in("run"));
  g.products("done_one") =
    g.transform("A3", call_one, concurrency::unlimited).family("used_one"_in("run"));

  g.products("two") = g.transform("B1", call_two, concurrency::unlimited)
                        .family("one"_in("run"), "another"_in("subrun"));
  g.products("used_two") = g.transform("B2", call_two, concurrency::unlimited)
                             .family("used_one"_in("run"), "two"_in("subrun"));

  g.products("three") = g.transform("C", call_two, concurrency::unlimited)
                          .family("used_two"_in("subrun"), "still"_in("event"));

  g.execute("cached_execution_t");

  // FIXME: Need to improve the synchronization to supply strict equality
  CHECK(g.execution_counts("A1") >= n_runs);
  CHECK(g.execution_counts("A2") >= n_runs);
  CHECK(g.execution_counts("A3") >= n_runs);

  CHECK(g.execution_counts("B1") >= n_runs * n_subruns);
  CHECK(g.execution_counts("B2") >= n_runs * n_subruns);

  CHECK(g.execution_counts("C") == n_runs * n_subruns * n_events);
}
