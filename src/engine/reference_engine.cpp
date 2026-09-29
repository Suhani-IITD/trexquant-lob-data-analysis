#include "exchange/engine/reference_engine.hpp"

#include "exchange/core/checked_arithmetic.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace exchange {
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::logic_error(message);
    }
}
} // namespace

ReferenceEngine::ReferenceEngine(EngineConfig config, EpochId epoch)
    : config_(std::move(config)), epoch_(epoch) {
    const auto errors = validate_config(config_);
    if (!errors.empty()) {
        throw std::invalid_argument(errors.front().field + ": " + errors.front().description);
    }
    if (epoch_.value() == 0) {
        throw std::invalid_argument("Epoch must be nonzero");
    }
    for (const auto& instrument : config_.instruments) {
        books_.try_emplace(instrument.id, instrument);
    }
    active_.max_load_factor(0.7F);
    active_.reserve(config_.capacity.maximum_active_orders);
}

CommandResult ReferenceEngine::process(const CommandEnvelope& command) {
    CommandResult result;
    process_into(command, result);
    return result;
}

void ReferenceEngine::process_into(const CommandEnvelope& command, CommandResult& result) {
    const auto expected = checked_add(last_ingress_.value(), 1);
    if (!expected || command.ingress.value() != *expected || command.epoch != epoch_ ||
        command.session.value() == 0) {
        throw std::invalid_argument("Invalid epoch, session, or noncontiguous ingress sequence");
    }
    result.reports.clear();
    result.events.clear();
    result.ingress = command.ingress;
    result.market_before = result.market_after = last_market_;
    std::visit(
        [&](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, NewInput>) {
                return process_new(command, input, result);
            } else if constexpr (std::is_same_v<Input, CancelInput>) {
                return process_cancel(command, input, result);
            } else {
                // Phase 2: Replace is now an atomic engine command rather than unsupported.
                return process_replace(command, input, result);
            }
        }, command.input);
    last_ingress_ = command.ingress;
#ifndef NDEBUG
    validate();
#endif
}

void ReferenceEngine::reject(const CommandEnvelope& command, OrderIdentity order,
                                      ExecutionStatus status, RejectReason reason, CommandResult& result) const {
    result.reports.push_back({command.ingress, 0, command.correlation, command.session, order,
                              status, Quantity{}, Quantity{}, std::nullopt, reason});
    return;
}

void ReferenceEngine::emit_report(CommandResult& result, const CommandEnvelope& command,
                                  SessionId recipient, OrderIdentity order, ExecutionStatus status,
                                  Quantity last, Quantity leaves, std::optional<PriceTicks> price) {
    result.reports.push_back({command.ingress, static_cast<std::uint64_t>(result.reports.size()),
                              command.correlation, recipient, order, status, last, leaves, price,
                              std::nullopt});
}

void ReferenceEngine::emit_event(CommandResult& result, InstrumentId instrument, MarketEvent event) {
    // Entire sequence range and output capacity were checked before mutation.
    last_market_ = MarketSequence{last_market_.value() + 1};
    result.events.push_back({last_market_, result.ingress, instrument, std::move(event)});
    result.market_after = last_market_;
}

void ReferenceEngine::process_new(const CommandEnvelope& command, const NewInput& input, CommandResult& result) {
    const auto rejected = [&](RejectReason reason) {
        return reject(command, {input.id, OrderGeneration{}}, ExecutionStatus::Rejected, reason, result);
    };
    if (input.id.value() == 0 || (input.side != Side::Buy && input.side != Side::Sell) ||
        (input.type != OrderType::Limit && input.type != OrderType::Market) ||
        (input.tif != TimeInForce::GTC && input.tif != TimeInForce::IOC)) {
        return rejected(RejectReason::InvalidField);
    }
    // Phase 2: IOC supports limit and market orders; a market order can never rest.
    if (input.type == OrderType::Market && input.tif != TimeInForce::IOC) {
        return rejected(RejectReason::UnsupportedOrderType);
    }
    const auto instrument = books_.find(input.instrument);
    if (instrument == books_.end()) {
        return rejected(RejectReason::InvalidInstrument);
    }
    auto& book = instrument->second;
    const bool invalid_price = input.type == OrderType::Market
        ? input.price.has_value()
        : !input.price || *input.price < book.config_.minimum || *input.price > book.config_.maximum;
    if (invalid_price) {
        return rejected(RejectReason::InvalidPrice);
    }
    if (input.quantity.value() == 0 || input.quantity > book.config_.maximum_order_quantity) {
        return rejected(RejectReason::InvalidQuantity);
    }
    if (active_.contains(input.id)) {
        return rejected(RejectReason::DuplicateOrderId);
    }
    return match_order(command, input, result);
}

// Phase 2: generalizes Phase 1's GTC matcher without duplicating fill/insert logic.
// The optional replacement stays live throughout planning and preparation. Removing
// it early would make a capacity/allocation failure destroy the original order.
void ReferenceEngine::match_order(const CommandEnvelope& command, const NewInput& input, CommandResult& result,
                                           std::optional<OrderLocation> replaced_order) {
    auto& book = books_.at(input.instrument);
    const OrderIdentity taker{input.id, replaced_order ? replaced_order->order->generation
                                                     : OrderGeneration{command.ingress.value()}};
    const auto rejected = [&](RejectReason reason) {
        return reject(command, replaced_order ? taker : OrderIdentity{input.id, OrderGeneration{}},
                      replaced_order ? ExecutionStatus::ReplaceRejected : ExecutionStatus::Rejected,
                      reason, result);
    };

    // Read-only planning. Rejecting final capacity must not execute any of these fills.
#ifdef EXCHANGE_REUSE_FILL_BUFFER
    auto& fills = fill_buffer_;
    fills.clear();
#else
    std::vector<PlannedFill> fills;
#endif
    auto remaining = input.quantity.value();
    std::size_t depleted_orders = 0;
    std::size_t depleted_levels = 0;
    const auto scan = [&](const auto& levels) {
        for (const auto& [price, level] : levels) {
            // Phase 2: absence of a limit means traverse all available opposite prices.
            const bool outside_limit = input.price &&
                (input.side == Side::Buy ? price > *input.price : price < *input.price);
            if (remaining == 0 || outside_limit) {
                break;
            }
            auto level_remaining = level.total.value();
            for (const auto& maker : level.fifo) {
                if (remaining == 0) {
                    break;
                }
                const auto executed = std::min(remaining, maker.remaining.value());
                fills.push_back({maker.id, price, Quantity{executed}});
                remaining -= executed;
                level_remaining -= executed;
                if (executed == maker.remaining.value()) {
                    ++depleted_orders;
                }
            }
            if (level_remaining == 0) {
                ++depleted_levels;
            }
        }
    };
    if (input.side == Side::Buy) {
        scan(book.asks_);
    } else {
        scan(book.bids_);
    }

    // Phase 2: capacity describes the final book. IOC needs no resting slot; a
    // replacement releases its old slot and possibly its old level before insertion.
    const bool rests = input.tif == TimeInForce::GTC && remaining != 0;
    const auto resting_quantity = rests ? remaining : 0;
    const auto old_quantity = replaced_order ? replaced_order->order->remaining : Quantity{};
    const auto old_price = replaced_order
        ? std::optional{std::visit([](auto handle) { return handle.level->first; }, replaced_order->level)}
        : std::nullopt;
    const bool removes_old_level = replaced_order && std::visit(
        [](auto handle) { return handle.level->second.fifo.size() == 1; }, replaced_order->level);
    const bool same_level = replaced_order && old_price == input.price;
    auto bid = rests ? book.bids_.find(*input.price) : book.bids_.end();
    auto ask = rests ? book.asks_.find(*input.price) : book.asks_.end();
    const bool missing_level = input.side == Side::Buy ? bid == book.bids_.end() : ask == book.asks_.end();
    const bool new_level = missing_level || (same_level && removes_old_level);
    auto prior_total = missing_level ? 0 : (input.side == Side::Buy ? bid->second.total.value()
                                                                 : ask->second.total.value());
    if (rests && same_level) {
        prior_total -= old_quantity.value();
    }
    const auto final_total = checked_add(prior_total, resting_quantity);
    const auto retained_orders = active_.size() - depleted_orders - (replaced_order ? 1U : 0U);
    const auto retained_levels = active_levels_ - depleted_levels - (removes_old_level ? 1U : 0U);
    if (rests &&
        (retained_orders >= config_.capacity.maximum_active_orders ||
         (new_level && retained_levels >= config_.capacity.maximum_active_levels) ||
         !final_total || *final_total > book.config_.maximum_level_quantity.value())) {
        return rejected(RejectReason::ResourceExhausted);
    }
    // Phase 2: at most two fixed reports (acknowledgement/expiry), plus two per fill;
    // at most two fixed events (replacement Cancel/remainder Add), plus one per fill.
    if (fills.size() > (std::numeric_limits<std::size_t>::max() - 2) / 2) {
        throw std::length_error("Command result size overflow");
    }
    const bool expires = !rests && remaining != 0;
    const auto report_count = 1 + 2 * fills.size() + (expires ? 1U : 0U);
    const auto event_count = fills.size() + (rests ? 1U : 0U) + (replaced_order ? 1U : 0U);
    if (!std::in_range<std::uint64_t>(event_count) ||
        !checked_add(last_market_.value(), static_cast<std::uint64_t>(event_count))) {
        throw std::overflow_error("Market sequence exhausted");
    }
    result.reports.reserve(report_count);
    result.events.reserve(event_count);

    // Prepare all allocating insertions privately. Splice/node transfer uses equal allocators.
    std::list<RestingOrder> staged_orders;
    std::optional<BidLevels::node_type> staged_bid;
    std::optional<AskLevels::node_type> staged_ask;
    ActiveIndex::node_type staged_index;
    if (rests) {
        staged_orders.push_back({input.id, taker.generation, command.session, Quantity{remaining},
                                 command.ingress});
        if (new_level && input.side == Side::Buy) {
            BidLevels staging;
            staging.try_emplace(*input.price, PriceLevel{*input.price, Quantity{}, {}});
            staged_bid.emplace(staging.extract(staging.begin()));
        } else if (new_level) {
            AskLevels staging;
            staging.try_emplace(*input.price, PriceLevel{*input.price, Quantity{}, {}});
            staged_ask.emplace(staging.extract(staging.begin()));
        }
        ActiveIndex staging;
        staging.emplace(input.id, OrderLocation{});
        staged_index = staging.extract(input.id);
    }

    // Commit: no expected rejection or allocation remains. Unexpected invariant failure is fatal.
    emit_report(result, command, command.session, taker,
                replaced_order ? ExecutionStatus::Replaced : ExecutionStatus::Accepted, Quantity{},
                input.quantity, std::nullopt);
    // Phase 2: only after every allocation succeeds can a replacement relinquish
    // its original FIFO node. Its generation survives; its new priority is ingress.
    if (replaced_order) {
        std::visit([&](auto handle) {
            auto& total = handle.level->second.total;
            total = Quantity{total.value() - old_quantity.value()};
        }, replaced_order->level);
        remove_order(*replaced_order);
        emit_event(result, input.instrument, CancelEvent{taker, old_quantity});
    }
    auto taker_remaining = input.quantity.value();
    for (const auto& fill : fills) {
        const auto location = active_.at(fill.maker);
        auto& maker = *location.order;
        maker.remaining = Quantity{maker.remaining.value() - fill.executed.value()};
        taker_remaining -= fill.executed.value();
        std::visit([&](auto handle) {
            auto& total = handle.level->second.total;
            total = Quantity{total.value() - fill.executed.value()};
        }, location.level);
        const OrderIdentity maker_identity{maker.id, maker.generation};
        emit_report(result, command, maker.owner, maker_identity,
                    maker.remaining.value() == 0 ? ExecutionStatus::Fill : ExecutionStatus::PartialFill,
                    fill.executed, maker.remaining, fill.price);
        emit_report(result, command, command.session, taker,
                    taker_remaining == 0 ? ExecutionStatus::Fill : ExecutionStatus::PartialFill,
                    fill.executed, Quantity{taker_remaining}, fill.price);
        emit_event(result, input.instrument, TradeEvent{maker_identity, taker, fill.price, fill.executed});
        if (maker.remaining.value() == 0) {
            remove_order(location);
        }
    }
    if (rests) {
        OrderLocation location{};
        location.instrument = input.instrument;
        location.order = staged_orders.begin();
        if (input.side == Side::Buy) {
            if (staged_bid) {
                auto insertion = book.bids_.insert(std::move(*staged_bid));
                require(insertion.inserted, "Prepared bid level already exists");
                bid = insertion.position;
                ++active_levels_;
            }
            bid->second.fifo.splice(bid->second.fifo.end(), staged_orders);
            bid->second.total = Quantity{*final_total};
            location.level = BidLocation{bid};
        } else {
            if (staged_ask) {
                auto insertion = book.asks_.insert(std::move(*staged_ask));
                require(insertion.inserted, "Prepared ask level already exists");
                ask = insertion.position;
                ++active_levels_;
            }
            ask->second.fifo.splice(ask->second.fifo.end(), staged_orders);
            ask->second.total = Quantity{*final_total};
            location.level = AskLocation{ask};
        }
        staged_index.mapped() = location;
        require(active_.insert(std::move(staged_index)).inserted, "Prepared order ID already exists");
        emit_event(result, input.instrument,
                   AddEvent{taker, input.side, *input.price, Quantity{remaining}, command.ingress});
    }
    // Phase 2: unfilled IOC demand was never public book state. Expire privately,
    // with zero executable leaves and no Cancel event or market-sequence increment.
    if (expires) {
        emit_report(result, command, command.session, taker, ExecutionStatus::Expired,
                    Quantity{}, Quantity{}, std::nullopt);
    }
    return;
}

// Phase 2: reductions/no-ops keep their original node and FIFO timestamp. Increases
// and reprices use the shared transactional matcher with the old lifecycle identity.
void ReferenceEngine::process_replace(const CommandEnvelope& command,
                                               const ReplaceInput& input, CommandResult& result) {
    const OrderIdentity identity{input.id, input.generation};
    const auto rejected = [&](RejectReason reason) {
        return reject(command, identity, ExecutionStatus::ReplaceRejected, reason, result);
    };
    if (input.id.value() == 0) {
        return rejected(RejectReason::InvalidField);
    }
    const auto found = active_.find(input.id);
    if (found == active_.end()) {
        return rejected(RejectReason::UnknownOrder);
    }
    const auto location = found->second;
    auto& resting_order = *location.order;
    if (resting_order.generation != input.generation) {
        return rejected(RejectReason::StaleOrder);
    }
    if (resting_order.owner != command.session) {
        return rejected(RejectReason::NotOwner);
    }
    const auto& instrument = books_.at(location.instrument).config_;
    if (input.new_price < instrument.minimum || input.new_price > instrument.maximum) {
        return rejected(RejectReason::InvalidPrice);
    }
    if (input.new_remaining.value() == 0 || input.new_remaining > instrument.maximum_order_quantity) {
        return rejected(RejectReason::InvalidQuantity);
    }
    const auto old_price = std::visit([](auto handle) { return handle.level->first; }, location.level);
    if (input.new_price == old_price && input.new_remaining <= resting_order.remaining) {
        const bool reduces = input.new_remaining < resting_order.remaining;
        if (reduces && !checked_add(last_market_.value(), 1)) {
            throw std::overflow_error("Market sequence exhausted");
        }
        result.reports.reserve(1);
        result.events.reserve(reduces ? 1U : 0U);
        emit_report(result, command, command.session, identity, ExecutionStatus::Replaced,
                    Quantity{}, input.new_remaining, std::nullopt);
        if (reduces) {
            const auto reduction = resting_order.remaining.value() - input.new_remaining.value();
            std::visit([&](auto handle) {
                auto& total = handle.level->second.total;
                total = Quantity{total.value() - reduction};
            }, location.level);
            resting_order.remaining = input.new_remaining;
            emit_event(result, location.instrument, ReduceEvent{identity, input.new_remaining});
        }
        return;
    }
    const auto side = std::holds_alternative<BidLocation>(location.level) ? Side::Buy : Side::Sell;
    const NewInput replacement{input.id, location.instrument, side, OrderType::Limit,
                               TimeInForce::GTC, input.new_price, input.new_remaining};
    return match_order(command, replacement, result, location);
}

void ReferenceEngine::process_cancel(const CommandEnvelope& command, const CancelInput& input, CommandResult& result) {
    const auto rejected = [&](RejectReason reason) {
        return reject(command, {input.id, input.generation}, ExecutionStatus::CancelRejected, reason, result);
    };
    if (input.id.value() == 0) {
        return rejected(RejectReason::InvalidField);
    }
    const auto found = active_.find(input.id);
    if (found == active_.end()) {
        return rejected(RejectReason::UnknownOrder);
    }
    const auto location = found->second;
    const auto maker = *location.order;
    if (maker.generation != input.generation) {
        return rejected(RejectReason::StaleOrder);
    }
    if (maker.owner != command.session) {
        return rejected(RejectReason::NotOwner);
    }
    if (!checked_add(last_market_.value(), 1)) {
        throw std::overflow_error("Market sequence exhausted");
    }
    result.reports.reserve(1);
    result.events.reserve(1);
    std::visit([&](auto handle) {
        auto& total = handle.level->second.total;
        total = Quantity{total.value() - maker.remaining.value()};
    }, location.level);
    remove_order(location);
    emit_report(result, command, command.session, {maker.id, maker.generation},
                ExecutionStatus::Cancelled, Quantity{}, Quantity{}, std::nullopt);
    emit_event(result, location.instrument, CancelEvent{{maker.id, maker.generation}, maker.remaining});
    return;
}

void ReferenceEngine::remove_order(const OrderLocation& location) {
    const auto retained = location;
    auto& book = books_.at(retained.instrument);
    active_.erase(retained.order->id);
    std::visit([&](auto handle) {
        auto& level = handle.level->second;
        level.fifo.erase(retained.order);
        if (level.fifo.empty()) {
            if constexpr (std::is_same_v<decltype(handle), BidLocation>) {
                book.bids_.erase(handle.level);
            } else {
                book.asks_.erase(handle.level);
            }
            --active_levels_;
        }
    }, retained.level);
}

EngineSnapshot ReferenceEngine::snapshot() const {
    EngineSnapshot result;
    result.books.reserve(books_.size());
    for (const auto& [id, book] : books_) {
        static_cast<void>(id);
        result.books.push_back(book.snapshot());
    }
    return result;
}

EngineDepthSnapshot ReferenceEngine::depth_snapshot() const {
    EngineDepthSnapshot result;
    result.books.reserve(books_.size());
    for (const auto& [id, book] : books_) {
        static_cast<void>(id);
        result.books.push_back(book.depth_snapshot());
    }
    return result;
}

void ReferenceEngine::validate() const {
    std::size_t orders = 0;
    std::size_t levels = 0;
    for (const auto& [instrument, book] : books_) {
        require(!book.best_bid() || !book.best_ask() || *book.best_bid() < *book.best_ask(),
                "Crossed resting book");
        const auto inspect = [&](const auto& side_levels, Side side) {
            for (const auto& [price, level] : side_levels) {
                ++levels;
                require(!level.fifo.empty() && price == level.price, "Empty or mispriced level");
                require(price >= book.config_.minimum && price <= book.config_.maximum,
                        "Out-of-range resting price");
                std::uint64_t total = 0;
                IngressSequence previous{};
                for (auto order = level.fifo.begin(); order != level.fifo.end(); ++order) {
                    ++orders;
                    require(order->remaining.value() > 0 && order->generation.value() > 0,
                            "Invalid resting quantity or generation");
                    require(previous < order->priority && order->priority <= last_ingress_,
                            "Invalid FIFO priority");
                    previous = order->priority;
                    const auto sum = checked_add(total, order->remaining.value());
                    require(sum.has_value(), "Aggregate overflow");
                    total = *sum;
                    const auto indexed = active_.find(order->id);
                    require(indexed != active_.end(), "Missing ID index entry");
                    const auto& location = indexed->second;
                    require(location.instrument == instrument && location.order == order,
                            "Mismatched order location");
                    require(std::holds_alternative<BidLocation>(location.level) == (side == Side::Buy),
                            "Wrong indexed side");
                    require(std::visit([&](auto handle) { return &handle.level->second == &level; },
                                       location.level), "Wrong indexed price level");
                }
                require(total == level.total.value() && total <= book.config_.maximum_level_quantity.value(),
                        "Invalid level total");
            }
        };
        inspect(book.bids_, Side::Buy);
        inspect(book.asks_, Side::Sell);
    }
    require(orders == active_.size() && levels == active_levels_, "Index/book cardinality mismatch");
    require(orders <= config_.capacity.maximum_active_orders &&
            levels <= config_.capacity.maximum_active_levels, "Logical capacity exceeded");
}

AdmissionDriver::AdmissionDriver(EngineConfig config, EpochId epoch) : engine_(std::move(config), epoch) {}

CommandResult AdmissionDriver::submit(SessionId session, CorrelationId correlation, CommandInput input) {
    const auto next = checked_add(engine_.last_ingress().value(), 1);
    if (!next) {
        throw std::overflow_error("Ingress sequence exhausted");
    }
    return engine_.process({engine_.epoch(), IngressSequence{*next}, session, correlation, std::move(input)});
}

} // namespace exchange
