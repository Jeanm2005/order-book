#pragma once
#include <cstdint>

enum class Side : std::uint8_t { Buy, Sell };

using Price = std::int64_t;
using Quantity = std::int64_t;
using OrderId = std::uint64_t;
using Nanos = std::uint64_t;

struct alignas(64) Order {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
    Nanos timestamp;

    Order* prev = nullptr;
    Order* next = nullptr;
};