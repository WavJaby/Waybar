#pragma once

#include <glibmm/main.h>

#include <functional>
#include <string>

namespace waybar::util {

// Newline-delimited subscriptions driven entirely by the GLib main loop.
class UnixSocketStream {
 public:
  using OutputCallback = std::function<void(const std::string&)>;
  using DisconnectCallback = std::function<void()>;

  UnixSocketStream(OutputCallback on_output, DisconnectCallback on_disconnect);
  ~UnixSocketStream();
  UnixSocketStream(const UnixSocketStream&) = delete;
  UnixSocketStream& operator=(const UnixSocketStream&) = delete;

  void start(const std::string& path, const std::string& request, unsigned retry_ms = 5000);
  void stop();

 private:
  void connect();
  void disconnect();
  void close();
  bool handleWritable(Glib::IOCondition condition);
  bool handleReadable(Glib::IOCondition condition);
  void watchReadable();

  OutputCallback on_output_;
  DisconnectCallback on_disconnect_;
  std::string path_;
  std::string request_;
  std::string buffer_;
  size_t sent_{0};
  unsigned retry_ms_{5000};
  int fd_{-1};
  bool connecting_{false};
  sigc::connection io_connection_;
  sigc::connection retry_connection_;
  sigc::connection timeout_connection_;
};

}  // namespace waybar::util
