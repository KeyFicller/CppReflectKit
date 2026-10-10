#pragma once

// Scaffolding for test_forward_chain.cpp: the layers, the reflection-built chain
// machinery, and the tiny consteval helpers. Included by exactly one test
// translation unit, so it keeps its file-scoped anonymous namespace.

#include <algorithm>
#include <array>
#include <cstddef>
#include <meta>
#include <string_view>
#include <vector>

struct LinearLayer {
    using WType = std::array<std::array<float, 2>, 3>;
    using BType = std::array<float, 3>;

    static constexpr WType W = {{
        {0.5f, -0.2f},
        {0.1f, 0.8f},
        {-0.3f, 0.4f}
    }};

    static constexpr BType B = {0.1f, -0.1f, 0.2f};

    static constexpr std::array<float, 3> forward(const std::array<float, 2>& x)
    {
        std::array<float, 3> out{};
        for (int i = 0; i < 3; ++i) {
            float sum = B[i];
            for (int j = 0; j < 2; ++j) {
                sum += W[i][j] * x[j];
            }
            out[i] = sum;
        }
        return out;
    }
};

struct ReLULayer {
    static constexpr std::array<float, 3> forward(const std::array<float, 3>& x)
    {
        std::array<float, 3> out{};
        for (int i = 0; i < 3; ++i) {
            out[i] = std::max(0.0f, x[i]);
        }
        return out;
    }
};

struct OutputLayer {
    using WType = std::array<float, 3>;
    static constexpr WType W = {0.7f, -0.5f, 0.3f};
    static constexpr float B = 0.05;

    static constexpr float forward(const std::array<float, 3>& x)
    {
        float sum = B;
        for (int i = 0; i < 3; ++i) {
            sum += W[i] * x[i];
        }
        return sum;
    }
};

template <std::meta::info... Members>
struct forward_chain;

template <std::meta::info Head>
struct forward_chain<Head> {
    template <typename Input>
    static constexpr auto apply(const Input& input)
    {
        using Layer = typename[:std::meta::type_of(Head):];
        return Layer::forward(input);
    }
};

template <std::meta::info Head, std::meta::info... Tail>
struct forward_chain<Head, Tail...> {
    template <typename Input>
    static constexpr auto apply(const Input& input)
    {
        using Layer = typename[:std::meta::type_of(Head):];
        return forward_chain<Tail...>::apply(Layer::forward(input));
    }
};

template <typename NestedLayer>
struct nested_forward {
    // The chain type is built by reflecting NestedLayer's members and substituting
    // them into forward_chain, so member order becomes pipeline order.
    using chain_type = typename[: []() consteval {
        std::vector<std::meta::info> args;
        for (auto member : std::meta::nonstatic_data_members_of(
                 ^^NestedLayer, std::meta::access_context::unchecked())) {
            args.push_back(std::meta::reflect_constant(member));
        }
        return std::meta::substitute(^^forward_chain, args);
    }() :];

    template <typename Input>
    static constexpr auto operator()(const Input& input)
    {
        return chain_type::apply(input);
    }
};

template <typename NestedLayer>
consteval auto generate_forward_function()
{
    return nested_forward<NestedLayer>{};
}

struct FFNetworkLayer {
    LinearLayer m_linear_layer;
    ReLULayer m_activation_layer;
    OutputLayer m_output_layer;

    static constexpr float forward(const std::array<float, 2>& x)
    {
        return generate_forward_function<FFNetworkLayer>()(x);
    }
};

namespace {

constexpr std::array<float, 2> kInput = {1.0f, 2.0f};

consteval std::size_t layer_count()
{
    return std::define_static_array(std::meta::nonstatic_data_members_of(
                                        ^^FFNetworkLayer, std::meta::access_context::unchecked()))
        .size();
}

consteval std::string_view first_layer()
{
    return std::meta::identifier_of(
        std::define_static_array(std::meta::nonstatic_data_members_of(
                                     ^^FFNetworkLayer, std::meta::access_context::unchecked()))[0]);
}

} // namespace
