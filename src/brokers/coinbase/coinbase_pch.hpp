// Precompiled header for Coinbase connectors
// Include heavy template headers here to speed up compilation

#pragma once

// Standard library
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <optional>

// Third-party heavy headers
#include <nlohmann/json.hpp>
#include <coinbase/websocket.hpp>
#include <coinbase/rest.hpp>
#include <coinbase/common.hpp>

// SlickTrader headers
#include <slick/logger.hpp>
#include <slick/queue.hpp>
