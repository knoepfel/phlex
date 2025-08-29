#include "phlex/model/level_id.hpp"
#include "phlex/module.hpp"

using namespace phlex::experimental;

namespace {
  int last_index(level_id const& id) { return static_cast<int>(id.number()); }
}

PHLEX_EXPERIMENTAL_REGISTER_ALGORITHMS(m, config)
{
  m.products(config.get<std::string>("produces", "a")) =
    m.transform("last_index", last_index, concurrency::unlimited).family("id");
}
