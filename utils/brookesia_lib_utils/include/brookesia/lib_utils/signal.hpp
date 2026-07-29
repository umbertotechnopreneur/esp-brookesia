/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace esp_brookesia::lib_utils {

/*
 * Minimal, thread-safe signal/slot facility covering the subset of the
 * `boost::signals2` API that this code base actually uses: void-returning
 * signals, `connect()` returning a `connection`, `scoped_connection` RAII,
 * `connected()`/`disconnect()` and reentrancy-safe emission. It deliberately
 * omits the heavyweight features (return-value combiners, automatic `track()`
 * lifetime management, shared connection blocks) that are unused here, which is
 * what makes it dramatically smaller than `boost::signals2`.
 */

namespace signal_detail {

struct SlotControl;

class SlotOwner {
public:
    virtual ~SlotOwner() = default;
    virtual void disconnect_slot(const SlotControl *control) noexcept = 0;
};

struct SlotControl {
    std::atomic<bool> connected{true};
    std::weak_ptr<SlotOwner> owner;

    void disconnect() noexcept
    {
        if (!connected.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (auto locked_owner = owner.lock()) {
            locked_owner->disconnect_slot(this);
        }
    }
};

} // namespace signal_detail

/**
 * @brief Lightweight handle to a single signal/slot connection.
 *
 * Copyable; all copies refer to the same underlying connection state.
 */
class connection {
public:
    connection() noexcept = default;
    explicit connection(std::shared_ptr<signal_detail::SlotControl> control) noexcept
        : control_(std::move(control)) {}

    bool connected() const noexcept
    {
        auto control = control_.lock();
        return control && control->connected.load(std::memory_order_acquire);
    }

    void disconnect() const noexcept
    {
        if (auto control = control_.lock()) {
            control->disconnect();
        }
    }

private:
    std::weak_ptr<signal_detail::SlotControl> control_;
};

/**
 * @brief RAII connection that disconnects on destruction.
 *
 * Movable but not copyable, mirroring `boost::signals2::scoped_connection`.
 */
class scoped_connection {
public:
    scoped_connection() noexcept = default;
    scoped_connection(const connection &conn) noexcept : conn_(conn) {}      // NOLINT(google-explicit-constructor)
    scoped_connection(connection &&conn) noexcept : conn_(std::move(conn)) {} // NOLINT(google-explicit-constructor)

    scoped_connection(const scoped_connection &) = delete;
    scoped_connection &operator=(const scoped_connection &) = delete;

    scoped_connection(scoped_connection &&other) noexcept : conn_(other.conn_)
    {
        other.conn_ = connection{};
    }

    scoped_connection &operator=(scoped_connection &&other) noexcept
    {
        if (this != &other) {
            conn_.disconnect();
            conn_ = other.conn_;
            other.conn_ = connection{};
        }
        return *this;
    }

    scoped_connection &operator=(const connection &conn) noexcept
    {
        conn_.disconnect();
        conn_ = conn;
        return *this;
    }

    ~scoped_connection()
    {
        conn_.disconnect();
    }

    bool connected() const noexcept
    {
        return conn_.connected();
    }

    void disconnect() const noexcept
    {
        conn_.disconnect();
    }

    connection release() noexcept
    {
        connection released = conn_;
        conn_ = connection{};
        return released;
    }

private:
    connection conn_;
};

template <typename Signature>
class signal;

/**
 * @brief Thread-safe multicast signal for void-returning slots.
 *
 * @tparam Args Slot parameter types.
 */
template <typename... Args>
class signal<void(Args...)> {
public:
    using slot_type = std::function<void(Args...)>;

    signal() : state_(std::make_shared<State>()) {}
    signal(const signal &) = delete;
    signal &operator=(const signal &) = delete;

    // Non-copyable but movable, matching boost::signals2::signal. Existing connections
    // stay valid across a move because the shared slot state moves with them.
    signal(signal &&other) noexcept : state_(std::move(other.state_)) {}

    signal &operator=(signal &&other) noexcept
    {
        if (this != &other) {
            disconnect_all_slots();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~signal()
    {
        disconnect_all_slots();
    }

    /**
     * @brief Register a slot and return a handle controlling its lifetime.
     */
    connection connect(slot_type slot)
    {
        auto state = state_;
        if (state == nullptr) {
            state = std::make_shared<State>();
            state_ = state;
        }

        auto entry = std::make_shared<Slot>(std::move(slot));
        entry->owner = state;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->slots.push_back(entry);
        }
        return connection{
            std::static_pointer_cast<signal_detail::SlotControl>(entry)
        };
    }

    /**
     * @brief Invoke every connected slot.
     *
     * A snapshot of the slot list is taken under the lock so slots may safely
     * connect/disconnect (including themselves) during emission, and so
     * emission is safe against concurrent mutation from other threads.
     */
    void operator()(Args... args) const
    {
        const auto state = state_;
        if (state == nullptr) {
            return;
        }

        std::vector<std::shared_ptr<Slot>> snapshot;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            snapshot = state->slots;
        }

        for (const auto &entry : snapshot) {
            if (entry->connected.load(std::memory_order_acquire)) {
                entry->fn(args...);
            }
        }
    }

    /**
     * @brief Disconnect every slot currently attached to this signal.
     */
    void disconnect_all_slots()
    {
        const auto state = state_;
        if (state == nullptr) {
            return;
        }

        std::vector<std::shared_ptr<Slot>> disconnected;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            for (const auto &entry : state->slots) {
                entry->connected.store(false, std::memory_order_release);
            }
            disconnected.swap(state->slots);
        }
    }

    /**
     * @brief Number of currently connected slots.
     */
    std::size_t num_slots() const
    {
        const auto state = state_;
        if (state == nullptr) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        std::size_t count = 0;
        for (const auto &entry : state->slots) {
            if (entry->connected.load(std::memory_order_acquire)) {
                ++count;
            }
        }
        return count;
    }

    bool empty() const
    {
        return num_slots() == 0;
    }

private:
    struct Slot final : signal_detail::SlotControl {
        explicit Slot(slot_type callback) : fn(std::move(callback)) {}

        slot_type fn;
    };

    struct State final : signal_detail::SlotOwner {
        void disconnect_slot(
            const signal_detail::SlotControl *control
        ) noexcept override
        {
            std::shared_ptr<Slot> disconnected;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto it = slots.begin(); it != slots.end(); ++it) {
                    if (static_cast<signal_detail::SlotControl *>(it->get()) ==
                        control) {
                        disconnected = std::move(*it);
                        slots.erase(it);
                        break;
                    }
                }
            }
        }

        mutable std::mutex mutex;
        std::vector<std::shared_ptr<Slot>> slots;
    };

    std::shared_ptr<State> state_;
};

} // namespace esp_brookesia::lib_utils
