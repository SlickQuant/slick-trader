#pragma once

// Use slick-orderbook library for order book management
#include <slick/orderbook/orderbook_l2.hpp>
#include <slick/orderbook/orderbook_l3.hpp>
#include <slick/orderbook/orderbook_manager.hpp>
#include <slick/orderbook/types.hpp>
#include <slick/orderbook/events.hpp>
#include <slick/orderbook/observer.hpp>

namespace slick::trader {

// Convenience aliases for slick-orderbook types
using OrderBook = slick::orderbook::OrderBookL2;
using OrderBookManager = slick::orderbook::OrderBookManager<slick::orderbook::OrderBookL2>;
using OrderBookObserver = slick::orderbook::IOrderBookObserver;
using OrderBookSide = slick::orderbook::Side;
using OrderBookPrice = slick::orderbook::Price;
using OrderBookQuantity = slick::orderbook::Quantity;
using TopOfBook = slick::orderbook::TopOfBook;
using PriceLevelUpdate = slick::orderbook::PriceLevelUpdate;

} // namespace slick::trader
