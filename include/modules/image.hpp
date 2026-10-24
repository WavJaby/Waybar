#pragma once

#include <fmt/format.h>
#include <gtkmm/image.h>

#include <csignal>
#include <string>

#include "ALabel.hpp"
#include "gtkmm/box.h"
#include "util/command.hpp"
#include "util/json.hpp"
#include "util/sleeper_thread.hpp"
#include "util/unix_socket_stream.hpp"

namespace waybar::modules {

namespace image {

class IStrategy {
 public:
  virtual ~IStrategy() = default;
  // Runs on the worker thread before update(). Use it for blocking work (e.g.
  // spawning a user script) so the GTK main loop isn't stalled. Default no-op.
  virtual void fetch() {}
  virtual void setSocketOutput(const Json::Value&) {}
  virtual void update() = 0;
};

class SingleImageStrategy : public IStrategy {
 public:
  SingleImageStrategy(const std::string&, const Json::Value&, const std::string&, Gtk::EventBox&,
                      bool);
  ~SingleImageStrategy() override = default;
  void update() override;
  void setSocketOutput(const Json::Value&) override;

 private:
  void parseOutputRaw();

  util::command::res output_;
  Json::Value config_;
  Gtk::Image image_;
  std::string path_;
  std::string tooltip_;
  Json::Value revision_;
  bool image_dirty_{true};
  int size_;
  Gtk::Box box_;
  bool hasTooltip_;
};

class MultipleImageStrategy : public IStrategy {
 public:
  MultipleImageStrategy(const std::string&, const Json::Value&, const std::string&, Gtk::EventBox&);
  ~MultipleImageStrategy() override = default;
  void fetch() override;
  void update() override;

 private:
  struct ImageData {
    std::string path;
    std::string marker;
    std::string tooltip;
    std::string on_click;
    std::shared_ptr<Gtk::Image> img;
    std::shared_ptr<Gtk::Button> btn;
  };

  void setImagesData(const Json::Value&);
  void setupAndDraw();
  void resetBoxAndMemory();
  void handleClick(const Glib::ustring& data);

  Json::Value config_;
  int size_;
  Gtk::Box box_;
  std::vector<ImageData> images_data_;
  // stdout captured by fetch() on the worker thread and consumed by update()
  std::string exec_output_;
};

}  // namespace image

class Image : public AModule {
 public:
  Image(const std::string&, const Json::Value&);
  virtual ~Image();
  auto update() -> void override;
  void refresh(int /*signal*/) override;

 private:
  void delayWorker();
  void socketWorker();
  void handleEvent();
  static std::unique_ptr<image::IStrategy> getStrategy(const std::string&, const Json::Value&,
                                                       const std::string&, Gtk::EventBox&, bool);

  std::chrono::milliseconds interval_;
  std::unique_ptr<image::IStrategy> strategy_;
  std::unique_ptr<util::UnixSocketStream> socket_stream_;
  std::string last_socket_output_;
  util::SleeperThread thread_;
};

}  // namespace waybar::modules
