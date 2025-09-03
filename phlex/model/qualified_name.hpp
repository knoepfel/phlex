#ifndef phlex_model_qualified_name_hpp
#define phlex_model_qualified_name_hpp

#include "phlex/model/algorithm_name.hpp"

#include <span>
#include <string>

namespace phlex::experimental {
  class qualified_name {
  public:
    qualified_name();
    qualified_name(char const* name);
    qualified_name(std::string name);
    qualified_name(algorithm_name qualifier, std::string name);

    std::string full() const;
    algorithm_name const& qualifier() const noexcept { return qualifier_; }
    std::string const& plugin() const noexcept { return qualifier_.plugin(); }
    std::string const& algorithm() const noexcept { return qualifier_.algorithm(); }
    std::string const& name() const noexcept { return name_; }

    bool operator==(qualified_name const& other) const;
    bool operator!=(qualified_name const& other) const;
    bool operator<(qualified_name const& other) const;

    static qualified_name create(char const* c);
    static qualified_name create(std::string const& s);

  private:
    algorithm_name qualifier_;
    std::string name_;
  };

  using qualified_names = std::span<qualified_name const, std::dynamic_extent>;

  class to_qualified_name {
  public:
    explicit to_qualified_name(algorithm_name const& qualifier) : qualifier_{qualifier} {}
    qualified_name operator()(std::string const& name) const
    {
      return qualified_name{qualifier_, name};
    }

  private:
    algorithm_name const& qualifier_;
  };

  template <std::size_t M>
  std::array<qualified_name, M> to_qualified_names(std::string const& name,
                                                   std::span<std::string const> output_labels)
  {
    std::array<qualified_name, M> outputs;
    std::ranges::transform(output_labels, outputs.begin(), to_qualified_name{name});
    return outputs;
  }
}

#endif // phlex_model_qualified_name_hpp
