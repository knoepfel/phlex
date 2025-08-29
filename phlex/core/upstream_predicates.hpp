#ifndef phlex_core_upstream_predicates_hpp
#define phlex_core_upstream_predicates_hpp

#include "phlex/configuration.hpp"
#include "phlex/core/registrar.hpp"

#include <string>
#include <vector>

namespace phlex::experimental {
  template <typename Ptr, std::size_t NumberOutputProducts>
  class upstream_predicates {
  public:
    explicit upstream_predicates(registrar<Ptr> reg, configuration const* config) :
      registrar_{std::move(reg)}
    {
      if (!config) {
        return;
      }
      registrar_.set_predicates(config->get_if_present<std::vector<std::string>>("when"));
    }

    auto when(std::vector<std::string> predicates)
    {
      if (!registrar_.has_predicates()) {
        registrar_.set_predicates(std::move(predicates));
      }
      return std::move(*this);
    }

    auto when(std::convertible_to<std::string> auto&&... names)
    {
      return when({std::forward<decltype(names)>(names)...});
    }

    auto release_registrar() { return std::move(registrar_); }

  private:
    registrar<Ptr> registrar_;
  };
}

#endif // phlex_core_upstream_predicates_hpp
