#include "phlex/core/index_router.hpp"
#include "phlex/model/child_tracker.hpp"
#include "phlex/model/product_store.hpp"

#include "fmt/std.h"
#include "oneapi/tbb/flow_graph.h"
#include "phlex/utilities/hashing.hpp"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cassert>
#include <ranges>
#include <stdexcept>

using namespace phlex::experimental;

namespace {
  using layer_path_t = std::vector<std::string>;

  std::size_t layer_hash_for_path(layer_path_t const& layer_path)
  {
    std::size_t result = "job"_id.hash();
    for (auto const& layer_name : layer_path | std::views::drop(1)) {
      result = hash(result, identifier{layer_name}.hash());
    }
    return result;
  }

  bool is_strict_prefix(layer_path_t const& candidate, layer_path_t const& other)
  {
    // FIXME: Use std::ranges::starts_with(other, candidate) once the compilers support it (C++23)
    return candidate.size() < other.size() and
           std::ranges::mismatch(other, candidate).in2 == std::ranges::end(candidate);
  }

  std::string delimited_layer_path(std::string_view const layer_path)
  {
    if (not layer_path.starts_with("/")) {
      return fmt::format("/{}", layer_path);
    }
    return std::string{layer_path};
  }

  flush_counts_ptr make_flush_counts_ptr(child_tracker const& fc)
  {
    std::map<phlex::data_cell_index::hash_type, std::size_t> child_counts;
    for (auto const& [layer_hash, count] : fc.committed_counts()) {
      child_counts.emplace(layer_hash, count.load());
    }
    return std::make_shared<flush_counts const>(std::move(child_counts));
  }
}

namespace phlex::experimental {

  //========================================================================================
  // multilayer_slot implementation
  namespace detail {
    class multilayer_slot {
    public:
      multilayer_slot(tbb::flow::graph& g,
                      identifier layer,
                      tbb::flow::receiver<indexed_end_token>* flush_port,
                      tbb::flow::receiver<index_message>* input_port);

      void put_message(data_cell_index_ptr const& index, std::size_t message_id);
      void put_end_token(data_cell_index_ptr const& index, child_tracker const& fc);

      bool matches_exactly(std::string const& layer_path) const;
      bool is_parent_of(data_cell_index_ptr const& index) const;

    private:
      identifier layer_;
      index_set_node broadcaster_;
      flush_node flusher_;
    };

    multilayer_slot::multilayer_slot(tbb::flow::graph& g,
                                     identifier layer,
                                     tbb::flow::receiver<indexed_end_token>* flush_port,
                                     tbb::flow::receiver<index_message>* input_port) :
      layer_{std::move(layer)}, broadcaster_{g}, flusher_{g}
    {
      make_edge(broadcaster_, *input_port);
      make_edge(flusher_, *flush_port);
    }

    void multilayer_slot::put_message(data_cell_index_ptr const& index, std::size_t message_id)
    {
      if (layer_ == index->layer_name()) {
        broadcaster_.try_put({.index = index, .msg_id = message_id, .cache = false});
        return;
      }

      broadcaster_.try_put({.index = index->parent(layer_), .msg_id = message_id});
    }

    void multilayer_slot::put_end_token(data_cell_index_ptr const& index, child_tracker const& fc)
    {
      // We're going to have to be a little more careful about this.  The committed total count may
      // not be enough granularity for some downstream nodes.
      flusher_.try_put({.index = index, .count = static_cast<int>(fc.committed_total_count())});
    }

    bool multilayer_slot::matches_exactly(std::string const& layer_path) const
    {
      return layer_path.ends_with(delimited_layer_path(static_cast<std::string_view>(layer_)));
    }

    bool multilayer_slot::is_parent_of(data_cell_index_ptr const& index) const
    {
      return index->parent(layer_) != nullptr;
    }
  }

  //========================================================================================
  // index_router implementation
  index_router::index_router(tbb::flow::graph& g) :
    index_receiver_{g,
                    tbb::flow::unlimited,
                    [this](index_message const& msg) -> data_cell_index_ptr {
                      auto const& [index, message_id, _] = msg;
                      assert(index);
                      return route(index, index_is_lowest_layer(index), message_id);
                    }},
    flush_receiver_{g,
                    tbb::flow::unlimited,
                    [this](unfold_flush input) -> tbb::flow::continue_msg {
                      auto&& [index, layer_hash, count] = input;
                      counter(index)->update_expected_count(layer_hash, count);
                      // Because the flush receiver receives flush values, the index cannot
                      // correspond to a lowest layer.
                      flush_if_done(index, false);
                      return {};
                    }},
    flusher_{g}
  {
  }

  void index_router::establish_layers(
    std::vector<std::vector<std::string>> const& layer_paths_from_driver,
    std::vector<identifier> unfold_input_layer_names,
    std::vector<identifier> unfold_output_layer_names)
  {
    auto sorted_layer_paths = layer_paths_from_driver;
    std::ranges::sort(sorted_layer_paths);

    std::vector<std::vector<std::string>> lowest_layer_candidates;
    // In sorted order, a path can only be a prefix of paths that follow it.
    for (std::size_t i = 0; i < sorted_layer_paths.size(); ++i) {
      bool const is_not_lowest_layer =
        i + 1 < sorted_layer_paths.size() and
        is_strict_prefix(sorted_layer_paths[i], sorted_layer_paths[i + 1]);
      if (is_not_lowest_layer) {
        auto const layer_hash = layer_hash_for_path(sorted_layer_paths[i]);
        is_lowest_layer_hashes_.emplace(layer_hash, false);
      }
    }

    unfold_input_layer_names_ = unfold_input_layer_names;
    unfold_output_layer_names_ = unfold_output_layer_names;
  }

  void index_router::finalize(tbb::flow::graph& g,
                              provider_input_ports_t provider_input_ports,
                              std::map<std::string, named_index_ports> multilayer_join_ports)
  {
    // We must have at least one provider port, or there can be no data to process.
    assert(!provider_input_ports.empty());

    // Create the index-set broadcast nodes for providers
    for (auto& [input_product, provider_port] : provider_input_ports | std::views::values) {
      auto [it, _] =
        index_set_nodes_.emplace(input_product.layer, std::make_shared<detail::index_set_node>(g));
      make_edge(*it->second, *provider_port);
    }

    for (auto const& [node_name, join_ports] : multilayer_join_ports) {
      spdlog::trace("Making multilayer slots for {}", node_name);
      detail::multilayer_slots slots;
      slots.reserve(join_ports.size());
      for (auto const& [layer, flush_port, input_port] : join_ports) {
        auto slot = std::make_shared<detail::multilayer_slot>(g, layer, flush_port, input_port);
        slots.push_back(slot);
      }
      multilayer_join_slots_.emplace(identifier{node_name}, std::move(slots));
    }
  }

  data_cell_index_ptr index_router::route(data_cell_index_ptr const index, index_flushes flushes)
  {
    update_flush_counts(std::move(flushes));
    return route(index, index_is_lowest_layer(index), received_indices_.fetch_add(1));
  }

  data_cell_index_ptr index_router::route(data_cell_index_ptr index,
                                          bool const is_lowest_layer,
                                          std::size_t const message_id)
  {
    if (auto index_set_node = index_set_node_for(index)) {
      index_set_node->try_put({.index = index, .msg_id = message_id});
    }

    auto [message_slots, end_token_slots] = multilayer_slots_for(index);
    for (auto const& slot : *message_slots) {
      slot->put_message(index, message_id);
    }

    // There should be no counter if the index is a lowest layer
    if (not is_lowest_layer) {
      counter(index)->set_flush_callback(
        [this, end_token_slots = std::move(end_token_slots), index, message_id](
          child_tracker const& fc) {
          for (auto const& slot : *end_token_slots) {
            slot->put_end_token(index, fc);
          }

          // Used only for folds, until folds use the slot infrastructure above.
          flusher_.try_put({index, make_flush_counts_ptr(fc), message_id});
        });
    }

    flush_if_done(index, is_lowest_layer);

    return index;
  }

  void index_router::drain(index_flushes flushes) { update_flush_counts(std::move(flushes)); }

  void index_router::register_unfold_count_per_input_layer(std::map<identifier, std::size_t> counts)
  {
    // Called once during finalize(), before any indices are routed, so no concurrent access.
    unfold_count_per_input_layer_ = std::move(counts);
  }

  bool index_router::index_is_lowest_layer(data_cell_index_ptr const& index)
  {
    auto it = is_lowest_layer_hashes_.find(index->layer_hash());
    if (it != is_lowest_layer_hashes_.end()) {
      return it->second;
    }

    if (std::ranges::contains(unfold_input_layer_names_, index->layer_name())) {
      // FIXME: Need to make sure that the index is a child of existing layers
      return is_lowest_layer_hashes_.emplace(index->layer_hash(), false).first->second;
    }

    if (std::ranges::contains(unfold_output_layer_names_, index->layer_name())) {
      return is_lowest_layer_hashes_.emplace(index->layer_hash(), true).first->second;
    }

    // If the index is neither and input or an output to an unfold, it is assumed to be a lowest layer.
    return is_lowest_layer_hashes_.emplace(index->layer_hash(), true).first->second;
  }

  detail::index_set_node_ptr index_router::index_set_node_for(data_cell_index_ptr const& index)
  {
    auto const layer_hash = index->layer_hash();
    if (auto it = index_set_node_cache_.find(layer_hash); it != index_set_node_cache_.end()) {
      return it->second;
    }

    std::string const layerish_path{static_cast<std::string_view>(index->layer_name())};
    auto broadcaster = index_set_node_for(layerish_path);
    index_set_node_cache_.insert({layer_hash, broadcaster});
    return broadcaster;
  }

  auto index_router::index_set_node_for(std::string const& layer_path) -> detail::index_set_node_ptr
  {
    std::string const search_token = delimited_layer_path(layer_path);

    std::vector<decltype(index_set_nodes_.begin())> candidates;
    for (auto it = index_set_nodes_.begin(), e = index_set_nodes_.end(); it != e; ++it) {
      if (search_token.ends_with(delimited_layer_path(static_cast<std::string_view>(it->first)))) {
        candidates.push_back(it);
      }
    }

    if (candidates.size() == 1ull) {
      return candidates[0]->second;
    }

    if (candidates.empty()) {
      return nullptr;
    }

    std::string msg = fmt::format("Multiple layers match specification {}:\n", layer_path);
    for (auto const& it : candidates) {
      msg += fmt::format("\n- {}", it->first);
    }
    throw std::runtime_error(msg);
  }

  std::pair<detail::multilayer_slots_ptr, detail::multilayer_slots_ptr>
  index_router::multilayer_slots_for(data_cell_index_ptr const& index)
  {
    auto const layer_hash = index->layer_hash();

    // Fast path: shared lock allows concurrent reads of cached entries.
    {
      multilayer_slot_cache_const_accessor acc;
      if (multilayer_slot_cache_.find(acc, layer_hash)) {
        return {acc->second.message_slots, acc->second.end_token_slots};
      }
    }

    // Slow path: exclusive lock serializes concurrent cache misses for the same layer.
    multilayer_slot_cache_accessor acc;
    auto const inserted = multilayer_slot_cache_.insert(acc, layer_hash);
    if (not inserted) {
      return {acc->second.message_slots, acc->second.end_token_slots};
    }

    auto const layer_path = index->layer_path();
    detail::multilayer_slots message_slots;
    detail::multilayer_slots end_token_slots;

    // For each multi-layer join node, determine which slots are relevant to this index.
    // Message entries: All slots from a node are added if (1) at least one slot exactly
    //                  matches the current layer, and (2) all slots either exactly match
    //                  or are parent layers of the current index.
    // End-token entries: Only slots that exactly match the current layer are added.
    for (auto& [node_name, slots] : multilayer_join_slots_) {
      detail::multilayer_slots matching_slots;
      matching_slots.reserve(slots.size());

      bool has_exact_match = false;
      std::size_t matched_count = 0;

      for (auto& slot : slots) {
        if (slot->matches_exactly(layer_path)) {
          has_exact_match = true;
          end_token_slots.push_back(slot);
          matching_slots.push_back(slot);
          ++matched_count;
        } else if (slot->is_parent_of(index)) {
          matching_slots.push_back(slot);
          ++matched_count;
        }
      }

      // Add all matching slots to message entries only if we have an exact match and
      // all slots from this node matched something (either exactly or as a parent).
      if (has_exact_match and matched_count == slots.size()) {
        message_slots.insert(message_slots.end(),
                             std::make_move_iterator(matching_slots.begin()),
                             std::make_move_iterator(matching_slots.end()));
      }
    }

    acc->second.message_slots =
      std::make_shared<detail::multilayer_slots const>(std::move(message_slots));
    acc->second.end_token_slots =
      std::make_shared<detail::multilayer_slots const>(std::move(end_token_slots));
    return {acc->second.message_slots, acc->second.end_token_slots};
  }

  void index_router::update_flush_counts(index_flushes flushes)
  {
    for (auto const& [index, flush_counts] : flushes) {
      counter(index)->update_expected_counts(*flush_counts);
      flush_if_done(index, false);
    }
  }

  child_tracker_ptr index_router::counter(data_cell_index_ptr const& index)
  {
    if (const_accessor a; child_trackers_.find(a, index->hash())) {
      return a->second;
    }

    accessor a;
    // If multiple unfolds consume this layer, the counter must wait for a flush message
    // from each of them before it can evaluate done().  Without this, the first unfold to
    // finish could cause the counter to fire before the others have reported their counts.
    std::size_t const expected_flush_count = [&]() -> std::size_t {
      auto it = unfold_count_per_input_layer_.find(index->layer_name());
      return it != unfold_count_per_input_layer_.end() ? it->second : 0;
    }();
    child_trackers_.emplace(
      a, index->hash(), std::make_shared<child_tracker>(index, expected_flush_count));
    return a->second;
  }

  void index_router::flush_if_done(data_cell_index_ptr index, bool const is_lowest_layer)
  {
    assert(index);

    auto parent = index->parent();

    // Lowest-layer indices do not have their own counters.
    // Increment the parent counter for the lowest-layer index instead.
    if (is_lowest_layer) {
      // Sometimes the lowest layer is the job, which has no parent
      if (parent) {
        counter(parent)->increment(index->layer_hash());
      }
      index = parent;
    }

    while (index) {
      // Use an exclusive accessor so that only one thread can release a given counter.
      // This prevents double-release when multiple threads call
      // flush_if_done concurrently for the same index.
      accessor a;
      if (not child_trackers_.find(a, index->hash())) {
        // This can happen when two threads process the same parent index,
        // and one of them releases it before the other completes.
        return;
      }

      if (not a->second->all_children_accounted()) {
        return;
      }

      a->second->send_flush();

      if (auto parent = index->parent()) {
        auto parent_counter = counter(parent);
        parent_counter->update_committed_counts(a->second->committed_counts());
        parent_counter->increment(index->layer_hash());
        index = parent;
      }
      child_trackers_.erase(a);
    }
  }
}
