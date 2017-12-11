#include "rpc/client.h"
#include "rpc/config.h"
#include "rpc/rpc_error.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "asio.hpp"
#include "format.h"

#include "rpc/detail/async_writer.h"
#include "rpc/detail/dev_utils.h"
#include "rpc/detail/response.h"

using namespace RPCLIB_ASIO;
using RPCLIB_ASIO::ip::tcp;
using namespace rpc::detail;

namespace rpc {

static constexpr uint32_t default_buffer_size =
    rpc::constants::DEFAULT_BUFFER_SIZE;

struct client::impl {
  impl(client *parent, std::string const &addr, uint16_t port)
      : parent_(parent),
        io_(),
        strand_(io_),
        call_idx_(0),
        addr_(addr),
        port_(port),
        is_connected_(false),
        state_(connection_state::initial),
        writer_(std::make_shared<detail::async_writer>(
            &io_,
            RPCLIB_ASIO::ip::tcp::socket(io_))),
        timeout_(nonstd::nullopt) {
    pac_.reserve_buffer(default_buffer_size);
  }

  std::future<connection_state> do_connect() {
    LOG_INFO("Initiating connection.");
    tcp::resolver resolver(io_);
    auto endpoint_iterator = resolver.resolve({addr_, std::to_string(port_)});
    auto conn_promise = std::make_shared<std::promise<connection_state>>();
    auto ft = conn_promise->get_future();
    RPCLIB_ASIO::async_connect(
        writer_->socket_, endpoint_iterator,
        [this, conn_promise](std::error_code ec, tcp::resolver::iterator) {
          if (!ec) {
            std::unique_lock<std::mutex> lock(mut_connection_finished_);
            LOG_INFO("Client connected to {}:{}", addr_, port_);
            is_connected_ = true;
            set_state(connection_state::connected);
            conn_promise->set_value(connection_state::connected);
            conn_finished_.notify_all();
            do_read();
          } else {
            LOG_ERROR("Error during connection: {}", ec);
            set_state(connection_state::disconnected);
            conn_promise->set_value(connection_state::disconnected);
          }
        });
    return ft;
  }

  void do_read() {
    LOG_TRACE("do_read");
    constexpr std::size_t max_read_bytes = default_buffer_size;
    writer_->socket_.async_read_some(
        RPCLIB_ASIO::buffer(pac_.buffer(), max_read_bytes),
        // I don't think max_read_bytes needs to be captured explicitly
        // (since it's constexpr), but MSVC insists.
        [this, max_read_bytes](std::error_code ec, std::size_t length) {
          if (!ec) {
            LOG_TRACE("Read chunk of size {}", length);
            pac_.buffer_consumed(length);

            RPCLIB_MSGPACK::unpacked result;
            while (pac_.next(result)) {
              auto r = response(std::move(result));
              auto id = r.get_id();
              auto &current_call = ongoing_calls_[id];
              try {
                if (r.get_error()) {
                  throw rpc_error("rpc::rpc_error during call",
                                  std::get<0>(current_call), r.get_error());
                }
                std::get<1>(current_call).set_value(std::move(*r.get_result()));
              } catch (...) {
                std::get<1>(current_call)
                    .set_exception(std::current_exception());
              }
              strand_.post([this, id]() { ongoing_calls_.erase(id); });
            }

            // resizing strategy: if the remaining buffer size is
            // less than the maximum bytes requested from asio,
            // then request max_read_bytes. This prompts the unpacker
            // to resize its buffer doubling its size
            // (https://github.com/msgpack/msgpack-c/issues/567#issuecomment-280810018)
            if (pac_.buffer_capacity() < max_read_bytes) {
              LOG_TRACE("Reserving extra buffer: {}", max_read_bytes);
              pac_.reserve_buffer(max_read_bytes);
            }
            do_read();
          } else if (ec == RPCLIB_ASIO::error::eof) {
            LOG_WARN("The server closed the connection.");
            set_state(connection_state::disconnected);
          } else if (ec == RPCLIB_ASIO::error::connection_reset) {
            // Yes, this should be connection_state::reset,
            // but on windows, disconnection results in reset. May be
            // asio bug, may be a windows socket pecularity. Should be
            // investigated later.
            set_state(connection_state::disconnected);
            LOG_WARN("The connection was reset.");
          } else {
            LOG_ERROR("Unhandled error code: {} | '{}'", ec, ec.message());
          }
        });
  }

  std::future<connection_state> async_reconnect() { return do_connect(); }

  connection_state reconnect() { return async_reconnect().get(); }

  connection_state get_connection_state() const { return state_; }

  void set_state(connection_state state) {
    connection_state prev = state_;
    state_ = state;
    if (callback_) {
      (*callback_)(*parent_, prev, state_);
    }
  }

  //! \brief Waits for the write queue and writes any buffers to the network
  //! connection. Should be executed throught strand_.
  void write(RPCLIB_MSGPACK::sbuffer item) { writer_->write(std::move(item)); }

  nonstd::optional<int64_t> get_timeout() { return timeout_; }

  void set_timeout(int64_t value) { timeout_ = value; }

  void clear_timeout() { timeout_ = nonstd::nullopt; }

  void set_state_handler(state_handler_t callback) {
    callback_ = std::move(callback);
  }

  void wait_conn() {
    std::unique_lock<std::mutex> lock(mut_connection_finished_);
    // TODO: there is a race condition here, run with TSAN!
    connection_state state = state_;
    if (state != connection_state::connected) {
      if (auto timeout = timeout_) {
        auto result =
            conn_finished_.wait_for(lock, std::chrono::milliseconds(*timeout));
        if (result == std::cv_status::timeout) {
          throw rpc::timeout(
              RPCLIB_FMT::format("Timeout of {}ms while connecting to {}:{}",
                                 *get_timeout(), addr_, port_));
        }
      } else {
        conn_finished_.wait(lock);
      }
    }
  }

  using call_t =
      std::pair<std::string, std::promise<RPCLIB_MSGPACK::object_handle>>;

  client *parent_;
  RPCLIB_ASIO::io_service io_;
  RPCLIB_ASIO::strand strand_;
  std::atomic<int> call_idx_;  /// The index of the last call made
  std::unordered_map<uint32_t, call_t> ongoing_calls_;
  std::string addr_;
  uint16_t port_;
  RPCLIB_MSGPACK::unpacker pac_;
  std::atomic_bool is_connected_;

  /// The connection state after do_connect.
  std::promise<rpc::connection_state> conn_state_prom_;

  std::condition_variable conn_finished_;
  std::mutex mut_connection_finished_;

  std::thread io_thread_;
  std::atomic<connection_state> state_;
  std::shared_ptr<detail::async_writer> writer_;
  nonstd::optional<int64_t> timeout_;
  nonstd::optional<state_handler_t> callback_;
  RPCLIB_CREATE_LOG_CHANNEL(client)
};  // namespace rpc

void client::common_init() {
  pimpl->do_connect();
  std::thread io_thread([this]() {
    RPCLIB_CREATE_LOG_CHANNEL(client)
    name_thread("client");
    pimpl->io_.run();
  });
  pimpl->io_thread_ = std::move(io_thread);
}

client::client(std::string const &addr, uint16_t port)
    : pimpl(new client::impl(this, addr, port)) {
  common_init();
}

client::client(std::string const &addr, uint16_t port, state_handler_t cb)
    : pimpl(new client::impl(this, addr, port)) {
  set_state_handler(cb);
  common_init();
}

void client::wait_conn() {
  pimpl->wait_conn();
}

int client::get_next_call_idx() {
  ++(pimpl->call_idx_);
  return pimpl->call_idx_;
}

void client::post(std::shared_ptr<RPCLIB_MSGPACK::sbuffer> buffer,
                  int idx,
                  std::string const &func_name,
                  std::shared_ptr<rsp_promise> p) {
  pimpl->strand_.post([=]() {
    pimpl->ongoing_calls_.insert(
        std::make_pair(idx, std::make_pair(func_name, std::move(*p))));
    pimpl->write(std::move(*buffer));
  });
}

void client::post(RPCLIB_MSGPACK::sbuffer *buffer) {
  pimpl->strand_.post([=]() {
    pimpl->write(std::move(*buffer));
    delete buffer;
  });
}

connection_state client::get_connection_state() const {
  return pimpl->get_connection_state();
}

nonstd::optional<int64_t> client::get_timeout() const {
  return pimpl->get_timeout();
}

void client::set_timeout(int64_t value) {
  pimpl->set_timeout(value);
}

void client::clear_timeout() {
  pimpl->clear_timeout();
}

void client::wait_all_responses() {
  for (auto &c : pimpl->ongoing_calls_) {
    c.second.second.get_future().wait();
  }
}

RPCLIB_NORETURN void client::throw_timeout(std::string const &func_name) {
  throw rpc::timeout(
      RPCLIB_FMT::format("Timeout of {}ms while calling RPC function '{}'",
                         *get_timeout(), func_name));
}

void client::set_state_handler(state_handler_t callback) {
  pimpl->set_state_handler(callback);
}

std::future<connection_state> client::async_reconnect() {
  return pimpl->async_reconnect();
}

connection_state client::reconnect() {
  return pimpl->reconnect();
}

client::~client() {
  pimpl->io_.stop();
  pimpl->io_thread_.join();
}

bool is_connected(client const &c) {
  return c.get_connection_state() == rpc::connection_state::connected;
}

}  // namespace rpc
