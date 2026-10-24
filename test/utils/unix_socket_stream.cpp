#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <glibmm/miscutils.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "util/unix_socket_stream.hpp"

namespace {

class Server {
 public:
  Server() {
    std::array<char, 40> temp{};
    std::strcpy(temp.data(), "/tmp/waybar-socket-test-XXXXXX");
    auto* directory = mkdtemp(temp.data());
    REQUIRE(directory != nullptr);
    directory_ = directory;
    path = directory_ + "/stream";
  }

  ~Server() {
    listener_watch_.disconnect();
    reader_watch_.disconnect();
    writer_watch_.disconnect();
    if (client_ != -1) ::close(client_);
    if (listener_ != -1) ::close(listener_);
    ::unlink(path.c_str());
    ::rmdir(directory_.c_str());
  }

  void listen() {
    listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    REQUIRE(listener_ != -1);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path.c_str());
    REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener_, 4) == 0);
    listener_watch_ = Glib::signal_io().connect(
        [this](Glib::IOCondition) {
          if (client_ != -1) ::close(client_);
          reader_watch_.disconnect();
          writer_watch_.disconnect();
          client_ = ::accept(listener_, nullptr, nullptr);
          REQUIRE(client_ != -1);
          incoming_.clear();
          outgoing_.clear();
          finish_ = false;
          reader_watch_ = Glib::signal_io().connect(
              [this](Glib::IOCondition) {
                std::array<char, 4096> bytes{};
                auto count = ::recv(client_, bytes.data(), bytes.size(), MSG_DONTWAIT);
                if (count <= 0) return false;
                incoming_.append(bytes.data(), count);
                if (incoming_.find('\n') != std::string::npos) {
                  if (on_request) on_request(incoming_);
                  return false;
                }
                return true;
              },
              client_, Glib::IO_IN | Glib::IO_HUP);
          if (on_connect) on_connect();
          return true;
        },
        listener_, Glib::IO_IN);
  }

  void send(std::string data, bool finish = false) {
    outgoing_ += data;
    finish_ = finish;
    writer_watch_.disconnect();
    writer_watch_ = Glib::signal_io().connect(
        [this](Glib::IOCondition) {
          const auto count =
              ::send(client_, outgoing_.data(), outgoing_.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
          if (count > 0) outgoing_.erase(0, count);
          if (outgoing_.empty()) {
            if (finish_) ::shutdown(client_, SHUT_WR);
            return false;
          }
          return true;
        },
        client_, Glib::IO_OUT);
  }

  std::string path;
  std::function<void(const std::string&)> on_request;
  std::function<void()> on_connect;

 private:
  std::string directory_;
  std::string incoming_;
  std::string outgoing_;
  bool finish_{false};
  int listener_{-1};
  int client_{-1};
  sigc::connection listener_watch_;
  sigc::connection reader_watch_;
  sigc::connection writer_watch_;
};

class RunLoop {
 public:
  RunLoop() {
    timeout_ = Glib::signal_timeout().connect(
        [this] {
          timed_out = true;
          loop->quit();
          return false;
        },
        3000);
  }
  ~RunLoop() { timeout_.disconnect(); }
  Glib::RefPtr<Glib::MainLoop> loop = Glib::MainLoop::create();
  bool timed_out{false};

 private:
  sigc::connection timeout_;
};

}  // namespace

TEST_CASE("Unix socket subscriptions frame fragmented and coalesced records",
          "[util][unix_socket_stream]") {
  Server server;
  RunLoop run;
  std::vector<std::string> lines;
  std::string request;
  sigc::connection fragment;
  server.on_request = [&](const auto& received) {
    request = received;
    server.send("{\"text\":\"fir");
    fragment = Glib::signal_timeout().connect(
        [&] {
          REQUIRE(lines.empty());
          server.send("st\"}\r\n{\"text\":\"second\"}\npartial", true);
          return false;
        },
        20);
  };
  server.listen();
  waybar::util::UnixSocketStream stream([&](const auto& line) { lines.push_back(line); },
                                        [&] { run.loop->quit(); });
  stream.start(server.path, "details");
  run.loop->run();
  fragment.disconnect();
  REQUIRE_FALSE(run.timed_out);
  REQUIRE(request == "details\n");
  REQUIRE(lines == std::vector<std::string>{"{\"text\":\"first\"}", "{\"text\":\"second\"}"});
}

TEST_CASE("Unix socket subscriptions reconnect and resend their request",
          "[util][unix_socket_stream]") {
  Server server;
  RunLoop run;
  unsigned requests = 0;
  unsigned disconnects = 0;
  std::vector<std::string> lines;
  server.on_request = [&](const auto& request) {
    REQUIRE(request == "icon\n");
    ++requests;
    server.send(std::to_string(requests) + "\n" + (requests == 1 ? "stale" : ""), true);
  };
  server.listen();
  waybar::util::UnixSocketStream stream(
      [&](const auto& line) {
        lines.push_back(line);
        if (lines.size() == 2) run.loop->quit();
      },
      [&] { ++disconnects; });
  stream.start(server.path, "icon", 10);
  run.loop->run();
  REQUIRE_FALSE(run.timed_out);
  REQUIRE(requests == 2);
  REQUIRE(disconnects >= 1);
  REQUIRE(lines == std::vector<std::string>{"1", "2"});
}

TEST_CASE("Unix socket subscriptions recover when the server starts later",
          "[util][unix_socket_stream]") {
  Server server;
  RunLoop run;
  unsigned disconnects = 0;
  std::vector<std::string> lines;
  server.on_connect = [&] { server.send("ready\n"); };
  auto delayed_start = Glib::signal_timeout().connect(
      [&] {
        server.listen();
        return false;
      },
      30);
  waybar::util::UnixSocketStream stream(
      [&](const auto& line) {
        lines.push_back(line);
        run.loop->quit();
      },
      [&] { ++disconnects; });
  stream.start(server.path, "", 10);
  run.loop->run();
  delayed_start.disconnect();
  REQUIRE_FALSE(run.timed_out);
  REQUIRE(disconnects >= 1);
  REQUIRE(lines == std::vector<std::string>{"ready"});
}

TEST_CASE("Stopping a Unix socket subscription cancels pending retries",
          "[util][unix_socket_stream]") {
  Server server;
  RunLoop run;
  unsigned disconnects = 0;
  waybar::util::UnixSocketStream stream([](const auto&) {}, [&] { ++disconnects; });
  stream.start(server.path, "", 5);
  REQUIRE(disconnects == 1);
  stream.stop();
  auto finish = Glib::signal_timeout().connect(
      [&] {
        run.loop->quit();
        return false;
      },
      40);
  run.loop->run();
  finish.disconnect();
  REQUIRE_FALSE(run.timed_out);
  REQUIRE(disconnects == 1);
}

TEST_CASE("Unix socket subscriptions reject oversized records", "[util][unix_socket_stream]") {
  Server server;
  RunLoop run;
  bool emitted = false;
  server.on_connect = [&] { server.send(std::string(1024 * 1024 + 1, 'x') + '\n'); };
  server.listen();
  waybar::util::UnixSocketStream stream([&](const auto&) { emitted = true; },
                                        [&] { run.loop->quit(); });
  stream.start(server.path, "");
  run.loop->run();
  REQUIRE_FALSE(run.timed_out);
  REQUIRE_FALSE(emitted);
}

TEST_CASE("Unix socket subscriptions validate paths and requests", "[util][unix_socket_stream]") {
  waybar::util::UnixSocketStream stream([](const auto&) {}, [] {});
  REQUIRE_THROWS_AS(stream.start("", ""), std::invalid_argument);
  REQUIRE_THROWS_AS(stream.start(std::string(200, 'x'), ""), std::invalid_argument);
  REQUIRE_THROWS_AS(stream.start("/tmp/unused", "details\nicon"), std::invalid_argument);
}

TEST_CASE("Socket paths expand variables without invoking a shell", "[util][unix_socket_stream]") {
  Glib::setenv("WAYBAR_SOCKET_PATH_TEST", "/tmp/with spaces", true);
  REQUIRE(waybar::util::expandSocketPath("${WAYBAR_SOCKET_PATH_TEST}/stream") ==
          "/tmp/with spaces/stream");
  REQUIRE(waybar::util::expandSocketPath("$WAYBAR_SOCKET_PATH_TEST/$(echo literal)") ==
          "/tmp/with spaces/$(echo literal)");
  Glib::unsetenv("WAYBAR_SOCKET_PATH_TEST");
  REQUIRE_THROWS_AS(waybar::util::expandSocketPath("$WAYBAR_SOCKET_PATH_TEST/stream"),
                    std::invalid_argument);
}
