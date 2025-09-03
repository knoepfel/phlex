#ifndef phlex_core_double_bound_function_hpp
#define phlex_core_double_bound_function_hpp

#include "phlex/concurrency.hpp"
#include "phlex/configuration.hpp"
#include "phlex/core/concepts.hpp"
#include "phlex/core/declared_unfold.hpp"
#include "phlex/core/input_arguments.hpp"
#include "phlex/core/node_catalog.hpp"
#include "phlex/core/upstream_predicates.hpp"
#include "phlex/metaprogramming/delegate.hpp"
#include "phlex/metaprogramming/type_deduction.hpp"
#include "phlex/model/algorithm_name.hpp"

#include <concepts>
#include <functional>
#include <memory>

namespace phlex::experimental {

  template <typename Object, typename Predicate, typename Unfold>
  class double_bound_function {
    using input_parameter_types = constructor_parameter_types<Object>;

    static constexpr auto N = std::tuple_size_v<input_parameter_types>;
    static constexpr std::size_t M = number_output_objects<Unfold>;

    // FIXME: Should maybe use some type of static assert, but not in a way that
    //        constrains the arguments of the Predicate and the Unfold to be the same.
    //
    // static_assert(
    //   std::same_as<function_parameter_types<Predicate>, function_parameter_types<Unfold>>);

  public:
    double_bound_function(configuration const* config,
                          std::string name,
                          Predicate predicate,
                          Unfold unfold,
                          concurrency c,
                          tbb::flow::graph& g,
                          node_catalog& nodes,
                          std::vector<std::string>& errors,
                          std::string destination_data_layer) :
      config_{config},
      registrar_{nodes.registrar_for<declared_unfold_ptr>(errors)},
      name_{config ? config->get<std::string>("module_label") : "", std::move(name)},
      concurrency_{c.value},
      graph_{g},
      predicate_{std::move(predicate)},
      unfold_{std::move(unfold)},
      destination_layer_{std::move(destination_data_layer)}
    {
    }

    auto family(std::array<specified_label, N> input_args)
    {
      registrar_.set_creator(
        [this, inputs = std::move(input_args)](auto upstream_predicates, auto output_products) {
          return std::make_unique<unfold_node<Object, Predicate, Unfold>>(
            std::move(name_),
            concurrency_,
            std::move(upstream_predicates),
            graph_,
            std::move(predicate_),
            std::move(unfold_),
            std::move(inputs),
            std::move(output_products),
            std::move(destination_layer_));
        });
      return upstream_predicates<declared_unfold_ptr, M>{std::move(registrar_), config_};
    }

    auto family(label_compatible auto... input_args)
    {
      static_assert(N == sizeof...(input_args),
                    "The number of function parameters is not the same as the number of specified "
                    "input arguments.");
      return family({specified_label{std::forward<decltype(input_args)>(input_args)}...});
    }

  private:
    configuration const* config_;
    registrar<declared_unfold_ptr> registrar_;
    algorithm_name name_;
    std::size_t concurrency_;
    tbb::flow::graph& graph_;
    Predicate predicate_;
    Unfold unfold_;
    std::string destination_layer_;
  };
}

#endif // phlex_core_double_bound_function_hpp
