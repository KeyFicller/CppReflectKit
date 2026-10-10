#include "scaffold/forward_chain.h"

// Compose a forward chain at compile time from a struct's reflected members and
// assert it equals the hand-written pipeline. Everything here is constexpr, so it
// is all static_assert.

// The chain built from the reflected members equals the hand-written pipeline.
static_assert(FFNetworkLayer::forward(kInput) ==
                  OutputLayer::forward(ReLULayer::forward(LinearLayer::forward(kInput))),
              "the reflection-built chain equals the hand-written pipeline");

// Member declaration order is pipeline order.
static_assert(layer_count() == 3, "FFNetworkLayer reflects three nested layers");
static_assert(first_layer() == "m_linear_layer", "the first layer is m_linear_layer");
