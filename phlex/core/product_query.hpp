#ifndef PHLEX_CORE_PRODUCT_QUERY_HPP
#define PHLEX_CORE_PRODUCT_QUERY_HPP

#include "phlex/model/identifier.hpp"
#include "phlex/model/product_specification.hpp"
#include "phlex/model/type_id.hpp"

#include <concepts>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// Used for the _id and _idq literals
using namespace phlex::experimental::literals;

namespace phlex {
  namespace detail {
    template <typename T>
    class required {
    public:
      template <typename U>
        requires std::constructible_from<T, U>
      required(U&& u) : content_{std::forward_like<T>(u)}
      {
        if (content_.empty()) {
          throw std::runtime_error("Cannot specify the empty string as a required field.");
        }
      }

      operator T const&() const noexcept { return content_; }

    private:
      T content_;
    };
  }

  struct product_query {
    detail::required<experimental::identifier> creator;
    detail::required<experimental::identifier> layer;
    std::optional<experimental::identifier> suffix;
    std::optional<experimental::identifier> stage;
    experimental::type_id type;

    // Check that all products selected by /other/ would satisfy this query
    bool match(product_query const& other) const;

    // Check if a product_specification satisfies this query
    bool match(experimental::product_specification const& spec) const;

    std::string to_string() const;

    bool operator==(product_query const& rhs) const;
    std::strong_ordering operator<=>(product_query const& rhs) const;

    // temporary additional members for transition
    experimental::product_specification spec() const;
  };

  using product_queries = std::vector<product_query>;
  namespace detail {
    // C is a container of product_queries
    template <typename C, typename T>
      requires std::is_same_v<typename std::remove_cvref_t<C>::value_type, product_query> &&
               experimental::is_tuple<T>::value
    class product_queries_type_setter {};
    template <typename C, typename... Ts>
    class product_queries_type_setter<C, std::tuple<Ts...>> {
    private:
      std::size_t index_ = 0;

      template <typename T>
      void set_type(C& container)
      {
        container.at(index_).type = experimental::make_type_id<T>();
        ++index_;
      }

    public:
      void operator()(C& container)
      {
        assert(container.size() == sizeof...(Ts));
        (set_type<Ts>(container), ...);
      }
    };
  }

  template <typename Tup, typename C>
    requires std::is_same_v<typename std::remove_cvref_t<C>::value_type, product_query> &&
             experimental::is_tuple<Tup>::value
  void populate_types(C& container)
  {
    detail::product_queries_type_setter<decltype(container), Tup> populate_types{};
    populate_types(container);
  }
}

#endif // PHLEX_CORE_PRODUCT_QUERY_HPP
