#ifndef phlex_utilities_simple_map_hpp
#define phlex_utilities_simple_map_hpp

// ==============================================================================
// The type simple_ptr_map<Ptr> is nearly equivalent to the type:
//
//   std::map<std::string, Ptr>
//
// Except that only three public member functions are available:
//
//   - begin()
//   - end()
//   - get(std::string const& key), where a bare pointer to the stored element is returned
//     (if it exists, otherwise the null pointer is returned)
// ==============================================================================

#include <map>
#include <string>

namespace phlex::experimental {
  template <typename Ptr>
  class simple_ptr_map {
    std::map<std::string, Ptr> data_;

  public:
    using mapped_type = typename std::map<std::string, Ptr>::mapped_type;

    auto try_emplace(std::string node_name, Ptr ptr)
    {
      return data_.try_emplace(std::move(node_name), std::move(ptr));
    }

    auto begin() const { return data_.begin(); }
    auto end() const { return data_.end(); }

    typename Ptr::element_type* get(std::string const& node_name) const
    {
      if (auto it = data_.find(node_name); it != data_.end()) {
        return it->second.get();
      }
      return nullptr;
    }
  };
}

#endif // phlex_utilities_simple_map_hpp
