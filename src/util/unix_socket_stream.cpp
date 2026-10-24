#include "util/unix_socket_stream.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace waybar::util {

UnixSocketStream::UnixSocketStream(OutputCallback on_output, DisconnectCallback on_disconnect)
    : on_output_(std::move(on_output)), on_disconnect_(std::move(on_disconnect)) {}

UnixSocketStream::~UnixSocketStream() { stop(); }

void UnixSocketStream::start(const std::string& path, const std::string& request,
                             unsigned retry_ms) {
  stop();
  sockaddr_un address{};
  if (path.empty() || path.size() >= sizeof(address.sun_path) ||
      path.find('\0') != std::string::npos) {
    throw std::invalid_argument("Invalid Unix socket path");
  }
  if (request.find_first_of("\r\n") != std::string::npos ||
      request.find('\0') != std::string::npos || request.size() > 65536) {
    throw std::invalid_argument("Socket request must be a single line of at most 64 KiB");
  }
  path_ = path;
  request_ = request.empty() ? "" : request + '\n';
  retry_ms_ = std::max(1U, retry_ms);
  connect();
}

void UnixSocketStream::close() {
  io_connection_.disconnect();
  timeout_connection_.disconnect();
  if (fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }
  buffer_.clear();
  sent_ = 0;
  connecting_ = false;
}

void UnixSocketStream::stop() {
  retry_connection_.disconnect();
  close();
}

void UnixSocketStream::disconnect() {
  close();
  on_disconnect_();
  retry_connection_.disconnect();
  retry_connection_ = Glib::signal_timeout().connect(
      [this] {
        connect();
        return false;
      },
      retry_ms_);
}

void UnixSocketStream::connect() {
  fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ == -1) {
    disconnect();
    return;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
    if (errno != EINPROGRESS) {
      disconnect();
      return;
    }
    connecting_ = true;
  }
  if (!connecting_ && request_.empty()) {
    watchReadable();
    return;
  }
  io_connection_ =
      Glib::signal_io().connect(sigc::mem_fun(*this, &UnixSocketStream::handleWritable), fd_,
                                Glib::IO_OUT | Glib::IO_HUP | Glib::IO_ERR | Glib::IO_NVAL);
  // Only connection establishment and the initial write have a timeout.
  // A healthy, idle subscription has no timer and never wakes Waybar.
  timeout_connection_ = Glib::signal_timeout().connect(
      [this] {
        disconnect();
        return false;
      },
      5000);
}

bool UnixSocketStream::handleWritable(Glib::IOCondition condition) {
  if (condition & (Glib::IO_HUP | Glib::IO_ERR | Glib::IO_NVAL)) {
    disconnect();
    return false;
  }
  if (connecting_) {
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) == -1 || error != 0) {
      disconnect();
      return false;
    }
    connecting_ = false;
  }
  while (sent_ < request_.size()) {
    auto count = ::send(fd_, request_.data() + sent_, request_.size() - sent_, MSG_NOSIGNAL);
    if (count > 0) {
      sent_ += static_cast<size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return true;
    } else {
      disconnect();
      return false;
    }
  }
  io_connection_.disconnect();
  timeout_connection_.disconnect();
  watchReadable();
  return false;
}

void UnixSocketStream::watchReadable() {
  io_connection_ =
      Glib::signal_io().connect(sigc::mem_fun(*this, &UnixSocketStream::handleReadable), fd_,
                                Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR | Glib::IO_NVAL);
}

bool UnixSocketStream::handleReadable(Glib::IOCondition condition) {
  std::array<char, 4096> chunk{};
  // Yield to the UI even when the producer is continuously sending data.
  for (unsigned reads = 0; reads < 16; ++reads) {
    const auto count = ::recv(fd_, chunk.data(), chunk.size(), 0);
    if (count > 0) {
      buffer_.append(chunk.data(), static_cast<size_t>(count));
      size_t offset = 0;
      for (auto end = buffer_.find('\n', offset); end != std::string::npos;
           end = buffer_.find('\n', offset)) {
        if (end - offset > 1024 * 1024) {
          disconnect();
          return false;
        }
        auto line = buffer_.substr(offset, end - offset);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        on_output_(line);
        offset = end + 1;
      }
      buffer_.erase(0, offset);
      if (buffer_.size() > 1024 * 1024) {
        disconnect();
        return false;
      }
    } else if (count == 0) {
      // A partial JSON record is never forwarded, including on disconnect.
      disconnect();
      return false;
    } else if (errno == EINTR) {
      continue;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (condition & (Glib::IO_HUP | Glib::IO_ERR | Glib::IO_NVAL)) {
        disconnect();
        return false;
      }
      return true;
    } else {
      disconnect();
      return false;
    }
  }
  return true;
}

}  // namespace waybar::util
