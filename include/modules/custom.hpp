#pragma once

#include <fmt/format.h>
#include <gdkmm/monitor.h>

#include <csignal>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "AIconLabel.hpp"
#include "util/command.hpp"
#include "util/command_line_stream.hpp"
#include "util/json.hpp"
#include "util/sleeper_thread.hpp"

namespace waybar::modules {

class Custom : public AIconLabel {
 public:
  Custom(const std::string&, const std::string&, const Json::Value&, const std::string&,
         Glib::RefPtr<Gdk::Monitor>, bool, uint32_t);
  virtual ~Custom();
  auto update() -> void override;
  void refresh(int /*signal*/) override;

 private:
  enum class WidthUnit { PIXELS, PERCENTAGE };
  struct WidthRequest {
    double value;
    WidthUnit unit;
  };

  void delayWorker();
  void continuousWorker();
  void startContinuousProcess(bool throw_on_failure);
  void handleContinuousProcessExit(int exit_code);
  void scheduleContinuousRestart();
  void waitingWorker();
  void parseOutputRaw();
  void parseOutputJson();
  void applyWidthRequest();
  void handleEvent();
  bool handleScroll(GdkEventScroll* e) override;
  bool handleToggle(GdkEventButton* const& e) override;

  const std::string name_;
  const std::string output_name_;
  Glib::RefPtr<Gdk::Monitor> monitor_;
  const bool vertical_;
  const uint32_t bar_axis_length_;
  std::string text_;
  std::string id_;
  std::string alt_;
  std::string tooltip_;
  std::string last_tooltip_markup_;
  std::string image_path_;
  std::string image_name_;
  unsigned app_icon_size_{24};
  const bool tooltip_format_enabled_;
  std::vector<std::string> class_;
  int percentage_;
  std::optional<WidthRequest> width_request_;
  int applied_width_request_{-1};
  int applied_height_request_{-1};
  util::command::res output_;
  util::JsonParser parser_;
  std::unique_ptr<util::command::LineStream> continuous_stream_;
  sigc::connection restart_connection_;
  sigc::connection monitor_geometry_connection_;

  util::SleeperThread thread_;
};

}  // namespace waybar::modules
