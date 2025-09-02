#ifndef phlex_core_declared_observer_hpp
#define phlex_core_declared_observer_hpp

#include "phlex/core/concepts.hpp"
#include "phlex/core/fwd.hpp"
#include "phlex/core/input_arguments.hpp"
#include "phlex/core/message.hpp"
#include "phlex/core/products_consumer.hpp"
#include "phlex/core/specified_label.hpp"
#include "phlex/core/store_counters.hpp"
#include "phlex/metaprogramming/type_deduction.hpp"
#include "phlex/model/algorithm_name.hpp"
#include "phlex/model/handle.hpp"
#include "phlex/model/level_id.hpp"
#include "phlex/model/product_store.hpp"
#include "phlex/model/qualified_name.hpp"
#include "phlex/utilities/sized_tuple.hpp"

#include "fmt/std.h"
#include "oneapi/tbb/concurrent_unordered_map.h"
#include "oneapi/tbb/flow_graph.h"
#include "spdlog/spdlog.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace phlex::experimental {

  class declared_observer : public products_consumer {
  public:
    declared_observer(algorithm_name name, std::vector<std::string> predicates);
    virtual ~declared_observer();
  };

  using declared_observer_ptr = std::unique_ptr<declared_observer>;

  // =====================================================================================

  template <typename AlgorithmBits>
  class observer_node : public declared_observer, private detect_flush_flag {
    using InputArgs = typename AlgorithmBits::input_parameter_types;
    using function_t = typename AlgorithmBits::bound_type;
    static constexpr auto N = AlgorithmBits::number_inputs;

    using stores_t = tbb::concurrent_hash_map<level_id::hash_type, bool>;
    using accessor = stores_t::accessor;

  public:
    static constexpr auto number_output_products = 0ull;
    using node_ptr_type = declared_observer_ptr;

    observer_node(algorithm_name name,
                  std::size_t concurrency,
                  std::vector<std::string> predicates,
                  tbb::flow::graph& g,
                  AlgorithmBits alg,
                  std::array<specified_label, N> input) :
      declared_observer{std::move(name), std::move(predicates)},
      product_labels_{std::move(input)},
      input_{form_input_arguments<InputArgs>(full_name(), product_labels_)},
      join_{make_join_or_none(g, std::make_index_sequence<N>{})},
      observer_{g,
                concurrency,
                [this, ft = alg.release_algorithm()](
                  messages_t<N> const& messages) -> oneapi::tbb::flow::continue_msg {
                  auto const& msg = most_derived(messages);
                  auto const& [store, message_id] = std::tie(msg.store, msg.id);
                  if (store->is_flush()) {
                    flag_for(store->id()->hash()).flush_received(message_id);
                  } else if (accessor a; needs_new(store, a)) {
                    call(ft, messages, std::make_index_sequence<N>{});
                    a->second = true;
                    flag_for(store->id()->hash()).mark_as_processed();
                  }

                  if (done_with(store)) {
                    stores_.erase(store->id()->hash());
                  }
                  return {};
                }}
    {
      make_edge(join_, observer_);
    }

    ~observer_node()
    {
      if (stores_.size() > 0ull) {
        spdlog::warn("Monitor {} has {} cached stores.", full_name(), stores_.size());
      }
      for (auto const& [id, _] : stores_) {
        spdlog::debug(" => ID: {}", id);
      }
    }

  private:
    tbb::flow::receiver<message>& port_for(specified_label const& product_label) override
    {
      return receiver_for<N>(join_, product_labels_, product_label);
    }

    std::vector<tbb::flow::receiver<message>*> ports() override { return input_ports<N>(join_); }

    specified_labels input() const override { return product_labels_; }

    bool needs_new(product_store_const_ptr const& store, accessor& a)
    {
      if (stores_.count(store->id()->hash()) > 0ull) {
        return false;
      }
      return stores_.insert(a, store->id()->hash());
    }

    template <std::size_t... Is>
    void call(function_t const& ft, messages_t<N> const& messages, std::index_sequence<Is...>)
    {
      ++calls_;
      return std::invoke(ft, std::get<Is>(input_).retrieve(messages)...);
    }

    std::size_t num_calls() const final { return calls_.load(); }

    std::array<specified_label, N> product_labels_;
    input_retriever_types<InputArgs> input_;
    join_or_none_t<N> join_;
    tbb::flow::function_node<messages_t<N>> observer_;
    tbb::concurrent_hash_map<level_id::hash_type, bool> stores_;
    std::atomic<std::size_t> calls_;
  };
}

#endif // phlex_core_declared_observer_hpp
