#include "phlex/module.hpp"
#include "test/plugins/add.hpp"

#include <cassert>

using namespace phlex::experimental;

// TODO: Option to select which algorithm to run via configuration?

PHLEX_EXPERIMENTAL_REGISTER_ALGORITHMS(m)
{
  m.products("sum") = m.transform("add", test::add, concurrency::unlimited).family("i", "j");
  m.observe(
     "verify", [](int actual) { assert(actual == 0); }, concurrency::unlimited)
    .family("sum");
}
