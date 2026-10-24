#include "modules/custom.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <limits>
#include <regex>
#include <stdexcept>
#include <utility>

waybar::modules::Custom::Custom(const std::string& name, const std::string& id,
                                const Json::Value& config, const std::string& output_name,
                                Glib::RefPtr<Gdk::Monitor> monitor, bool vertical,
                                uint32_t bar_axis_length)
    : AIconLabel(config, "custom-" + name, id, "{}"),
      name_(name),
      output_name_(output_name),
      monitor_(std::move(monitor)),
      vertical_(vertical),
      bar_axis_length_(bar_axis_length),
      id_(id),
      tooltip_format_enabled_{config_["tooltip-format"].isString()},
      percentage_(0) {
  if (config.isNull()) {
    spdlog::warn("There is no configuration for 'custom/{}', element will be hidden", name);
  }

  if (config_["socket"].isString()) {
    socketWorker();
  } else if (!config_["signal"].empty() && config_["interval"].empty() &&
             config_["restart-interval"].empty()) {
    waitingWorker();
  } else if (interval_.count() > 0) {
    delayWorker();
  } else if (config_["exec"].isString()) {
    continuousWorker();
  }
  if (config_["image-path"].isString()) {
    image_path_ = config_["image-path"].asString();
  }
  if (config_["image-name"].isString()) {
    image_name_ = config_["image-name"].asString();
  }
  if (config["icon-size"].isUInt()) {
    app_icon_size_ = config["icon-size"].asUInt();
  }
  if (monitor_) {
    monitor_geometry_connection_ = monitor_->property_geometry().signal_changed().connect(
        sigc::mem_fun(*this, &Custom::applyWidthRequest));
  }
}

waybar::modules::Custom::~Custom() {
  restart_connection_.disconnect();
  monitor_geometry_connection_.disconnect();
  if (socket_stream_) {
    socket_stream_->stop();
  }
  if (continuous_stream_) {
    continuous_stream_->stop();
  }
}

void waybar::modules::Custom::socketWorker() {
  for (const auto* option : {"exec", "exec-if", "interval", "signal", "restart-interval"}) {
    if (!config_[option].isNull()) {
      throw std::invalid_argument(
          fmt::format("custom/{}: socket cannot be used with {}", name_, option));
    }
  }
  if (!config_["socket-request"].isNull() && !config_["socket-request"].isString()) {
    throw std::invalid_argument("socket-request must be a string");
  }
  unsigned retry_ms = 5000;
  if (!config_["socket-reconnect-interval"].isNull()) {
    if (!config_["socket-reconnect-interval"].isNumeric()) {
      throw std::invalid_argument("socket-reconnect-interval must be a positive number");
    }
    const auto seconds = config_["socket-reconnect-interval"].asDouble();
    if (!std::isfinite(seconds) || seconds <= 0 ||
        seconds > std::numeric_limits<unsigned>::max() / 1000.0) {
      throw std::invalid_argument("socket-reconnect-interval is out of range");
    }
    retry_ms = std::max(1U, static_cast<unsigned>(seconds * 1000));
  }

  // Expand variables without executing a shell, splitting words, or globbing.
  const auto path = config_["socket"].asString();
  const std::regex variable(R"(\$\{([A-Za-z_][A-Za-z_0-9]*)\}|\$([A-Za-z_][A-Za-z_0-9]*))");
  std::string expanded;
  size_t offset = 0;
  for (auto it = std::sregex_iterator(path.begin(), path.end(), variable);
       it != std::sregex_iterator(); ++it) {
    const auto& match = *it;
    expanded.append(path, offset, match.position() - offset);
    const auto name = match[1].matched ? match[1].str() : match[2].str();
    bool found = false;
    const auto value = Glib::getenv(name, found);
    if (!found) {
      throw std::invalid_argument("Socket path environment variable is unset: " + name);
    }
    expanded += value;
    offset = match.position() + match.length();
  }
  expanded.append(path, offset, std::string::npos);

  output_ = {.exit_code = 1, .out = ""};
  socket_stream_ = std::make_unique<util::UnixSocketStream>(
      [this](const std::string& output) {
        if (output_.exit_code != 0 || output_.out != output) {
          output_ = {.exit_code = 0, .out = output};
          dp.emit();
        }
      },
      [this] {
        output_ = {.exit_code = 1, .out = ""};
        dp.emit();
      });
  socket_stream_->start(expanded, config_["socket-request"].asString(), retry_ms);
  dp.emit();
}

void waybar::modules::Custom::delayWorker() {
  if (!config_["exec"].isString() && !config_["exec-if"].isString()) {
    dp.emit();
    return;
  }

  thread_ = [this] {
    for (auto it = this->pid_children_.begin(); it != this->pid_children_.end();) {
      int status = 0;
      const auto pid = static_cast<pid_t>(*it);
      const auto waited = waitpid(pid, &status, WNOHANG);
      if (waited == 0) {
        ++it;
        continue;
      }
      if (waited == -1 && errno != ECHILD) {
        ++it;
        continue;
      }
      it = this->pid_children_.erase(it);
    }

    bool can_update = true;
    if (config_["exec-if"].isString()) {
      output_ = util::command::execNoRead(config_["exec-if"].asString());
      if (output_.exit_code != 0) {
        can_update = false;
        dp.emit();
      }
    }
    if (can_update) {
      if (config_["exec"].isString()) {
        output_ = util::command::exec(config_["exec"].asString(), output_name_);
      }
      dp.emit();
    }
    thread_.sleep_for(interval_);
  };
}

void waybar::modules::Custom::continuousWorker() {
  continuous_stream_ = std::make_unique<util::command::LineStream>(
      output_name_,
      [this](const std::string& output) {
        output_ = {.exit_code = 0, .out = output};
        dp.emit();
      },
      [this](int exit_code) { handleContinuousProcessExit(exit_code); });
  startContinuousProcess(true);
}

void waybar::modules::Custom::startContinuousProcess(bool throw_on_failure) {
  const auto cmd = config_["exec"].asString();

  try {
    continuous_stream_->start(cmd);
  } catch (const Glib::SpawnError& e) {
    if (throw_on_failure) {
      throw std::runtime_error("Unable to open " + cmd + ": " + e.what().raw());
    }
    output_ = {.exit_code = 1, .out = ""};
    dp.emit();
    spdlog::error("Unable to restart {}: {}", name_, e.what().raw());
    scheduleContinuousRestart();
  } catch (const std::exception& e) {
    if (throw_on_failure) {
      throw;
    }
    output_ = {.exit_code = 1, .out = ""};
    dp.emit();
    spdlog::error("Unable to restart {}: {}", name_, e.what());
    scheduleContinuousRestart();
  }
}

void waybar::modules::Custom::handleContinuousProcessExit(int exit_code) {
  if (exit_code != 0) {
    output_ = {.exit_code = exit_code, .out = ""};
    dp.emit();
    spdlog::error("{} stopped unexpectedly, is it endless?", name_);
  }

  scheduleContinuousRestart();
}

void waybar::modules::Custom::scheduleContinuousRestart() {
  restart_connection_.disconnect();
  if (!config_["restart-interval"].isNumeric() || config_["restart-interval"].asDouble() <= 0) {
    // A non-positive restart-interval must not busy-respawn the script
    // (that starves the GTK main loop); treat it as "do not restart".
    return;
  }

  restart_connection_ = Glib::signal_timeout().connect(
      [this] {
        startContinuousProcess(false);
        return false;
      },
      std::max(1U, static_cast<unsigned>(config_["restart-interval"].asDouble() * 1000)));
}

void waybar::modules::Custom::waitingWorker() {
  thread_ = [this] {
    bool can_update = true;
    if (config_["exec-if"].isString()) {
      output_ = util::command::execNoRead(config_["exec-if"].asString());
      if (output_.exit_code != 0) {
        can_update = false;
        dp.emit();
      }
    }
    if (can_update) {
      if (config_["exec"].isString()) {
        output_ = util::command::exec(config_["exec"].asString(), output_name_);
      }
      dp.emit();
    }
    thread_.sleep();
  };
}

void waybar::modules::Custom::refresh(int sig) {
#ifdef SIGRTMIN
  if (config_["signal"].isInt() && sig == SIGRTMIN + config_["signal"].asInt()) {
    thread_.wake_up();
  }
#endif
}

void waybar::modules::Custom::handleEvent() {
  if (!config_["exec-on-event"].isBool() || config_["exec-on-event"].asBool()) {
    thread_.wake_up();
  }
}

bool waybar::modules::Custom::handleScroll(GdkEventScroll* e) {
  auto ret = ALabel::handleScroll(e);
  handleEvent();
  return ret;
}

bool waybar::modules::Custom::handleToggle(GdkEventButton* const& e) {
  auto ret = ALabel::handleToggle(e);
  handleEvent();
  return ret;
}

auto waybar::modules::Custom::update() -> void {
  // Hide label if output is empty
  if ((config_["exec"].isString() || config_["exec-if"].isString() ||
       config_["socket"].isString()) &&
      (output_.out.empty() || output_.exit_code != 0)) {
    event_box_.hide();
    width_request_.reset();
  } else {
    if (config_["return-type"].asString() == "json") {
      parseOutputJson();
    } else {
      parseOutputRaw();
    }

    try {
      auto str = fmt::format(fmt::runtime(format_), fmt::arg("text", text_), fmt::arg("alt", alt_),
                             fmt::arg("icon", getIcon(percentage_, alt_)),
                             fmt::arg("percentage", percentage_));
      if ((config_["hide-empty-text"].asBool() && text_.empty()) ||
          (str.empty() && image_path_.empty() && image_name_.empty() && !width_request_)) {
        event_box_.hide();
      } else {
        setLabelMarkup(str);
        if (tooltipEnabled()) {
          std::string tooltip_markup;
          if (tooltip_format_enabled_) {
            auto tooltip = config_["tooltip-format"].asString();
            tooltip_markup = fmt::format(fmt::runtime(tooltip), fmt::arg("text", text_),
                                         fmt::arg("tooltip", tooltip_), fmt::arg("alt", alt_),
                                         fmt::arg("icon", getIcon(percentage_, alt_)),
                                         fmt::arg("percentage", percentage_));
          } else if (text_ == tooltip_) {
            tooltip_markup = str;
          } else {
            tooltip_markup = tooltip_;
          }

          setTooltipMarkup(tooltip_markup);
        }
        auto style = label_.get_style_context();
        auto classes = style->list_classes();
        for (auto const& c : classes) {
          if (c == id_) continue;
          style->remove_class(c);
        }
        // Module-level classes belong on box_, which carries the #custom-<name>
        // widget name and MODULE_CLASS (see AIconLabel). Adding them to label_
        // as well makes box_ and label_ both match a .<class> selector, so any
        // background, border or padding is applied twice, inset by the box's
        // padding.
        auto box_style = box_.get_style_context();
        for (auto const& c : box_style->list_classes()) {
          if (c == id_ || c == MODULE_CLASS) continue;
          box_style->remove_class(c);
        }
        for (auto const& c : class_) {
          box_style->add_class(c);
        }
        style->add_class("flat");
        style->add_class("text-button");
        auto image_style = image_.get_style_context();
        image_style->add_class("image-button");
        event_box_.show();
        if (!image_path_.empty()) {
          try {
            auto pixbuf =
                Gdk::Pixbuf::create_from_file(image_path_, app_icon_size_, app_icon_size_);
            image_.set(pixbuf);
          } catch (const Glib::Error& e) {
            spdlog::warn("custom {}: failed to load image-path '{}': {}", name_, image_path_,
                         std::string(e.what()));
            image_.clear();
          }
        } else if (!image_name_.empty()) {
          image_.set_from_icon_name(image_name_, Gtk::ICON_SIZE_INVALID);
          image_.set_pixel_size(app_icon_size_);
        }

        label_.set_visible(!str.empty());
      }
    } catch (const fmt::format_error& e) {
      spdlog::warn("{}: {}", name_, e.what());
      event_box_.show();
      label_.set_visible(true);
      setLabelMarkup(fmt::format(fmt::runtime("{text}"), fmt::arg("text", text_)));
      if (tooltipEnabled()) {
        setTooltipMarkup(tooltip_.empty() ? Glib::ustring(text_) : Glib::ustring(tooltip_));
      }
    }
  }
  applyWidthRequest();
  // Call parent update
  AIconLabel::update();

  // Show a configured image-path/image-name image after the base update() so
  // AIconLabel::update()'s icon gate cannot re-hide it. Leave the embedded-icon
  // and "icon" cases to the base class (they have no image-path/image-name).
  if (!image_name_.empty() || !image_path_.empty()) {
    image_.set_visible(true);
  }
}

void waybar::modules::Custom::applyWidthRequest() {
  int width_request = -1;
  int height_request = -1;
  if (width_request_) {
    double requested_length = width_request_->value;
    if (width_request_->unit == WidthUnit::PERCENTAGE) {
      int bar_axis_length = 0;
      if (bar_axis_length_ > 0) {
        bar_axis_length = static_cast<int>(std::min<uint32_t>(
            bar_axis_length_, static_cast<uint32_t>(std::numeric_limits<int>::max())));
      } else if (monitor_) {
        const auto geometry = *monitor_->property_geometry().get_value().gobj();
        bar_axis_length = vertical_ ? geometry.height : geometry.width;
      }
      if (bar_axis_length > 0) {
        requested_length = static_cast<double>(bar_axis_length) * requested_length / 100.0;
      } else {
        requested_length = -1.0;
      }
    }

    if (requested_length >= 0.0) {
      const auto max_length = static_cast<double>(std::numeric_limits<int>::max());
      const int length = requested_length >= max_length
                             ? std::numeric_limits<int>::max()
                             : static_cast<int>(std::lround(requested_length));
      if (vertical_) {
        height_request = length;
      } else {
        width_request = length;
      }
    }
  }

  if (width_request == applied_width_request_ && height_request == applied_height_request_) {
    return;
  }

  event_box_.set_size_request(width_request, height_request);
  applied_width_request_ = width_request;
  applied_height_request_ = height_request;
}

void waybar::modules::Custom::parseOutputRaw() {
  width_request_.reset();
  std::istringstream output(output_.out);
  std::string line;
  int i = 0;
  while (getline(output, line)) {
    Glib::ustring validated_line = line;
    if (!validated_line.validate()) {
      validated_line = validated_line.make_valid();
    }

    if (i == 0) {
      if (config_["escape"].isBool() && config_["escape"].asBool()) {
        text_ = Glib::Markup::escape_text(validated_line);
        tooltip_ = Glib::Markup::escape_text(validated_line);
      } else {
        text_ = validated_line;
        tooltip_ = validated_line;
      }
      tooltip_ = validated_line;
      class_.clear();
    } else if (i == 1) {
      if (config_["escape"].isBool() && config_["escape"].asBool()) {
        tooltip_ = Glib::Markup::escape_text(validated_line);
      } else {
        tooltip_ = validated_line;
      }
    } else if (i == 2) {
      class_.push_back(validated_line);
    } else {
      break;
    }
    i++;
  }
}

void waybar::modules::Custom::parseOutputJson() {
  width_request_.reset();
  std::istringstream output(output_.out);
  std::string line;
  class_.clear();
  // A script can emit invalid UTF-8; passing it unchecked to Pango/GTK aborts
  // the whole bar in g_utf8_* (see parseOutputRaw, which validates the same way).
  auto sanitize = [](const std::string& s) -> Glib::ustring {
    Glib::ustring value = s;
    if (!value.validate()) {
      value = value.make_valid();
    }
    return value;
  };
  while (getline(output, line)) {
    auto parsed = parser_.parse(line);
    const bool escape = config_["escape"].isBool() && config_["escape"].asBool();
    if (escape) {
      text_ = Glib::Markup::escape_text(sanitize(parsed["text"].asString()));
    } else {
      text_ = sanitize(parsed["text"].asString());
    }
    if (escape) {
      alt_ = Glib::Markup::escape_text(sanitize(parsed["alt"].asString()));
    } else {
      alt_ = sanitize(parsed["alt"].asString());
    }
    if (escape) {
      tooltip_ = Glib::Markup::escape_text(sanitize(parsed["tooltip"].asString()));
    } else {
      tooltip_ = sanitize(parsed["tooltip"].asString());
    }
    if (parsed["class"].isString()) {
      class_.push_back(parsed["class"].asString());
    } else if (parsed["class"].isArray()) {
      for (auto const& c : parsed["class"]) {
        class_.push_back(c.asString());
      }
    }

    if (parsed["width"].isString()) {
      auto width = parsed["width"].asString();
      const auto first = width.find_first_not_of(" \t\n\r\f\v");
      if (first != std::string::npos) {
        const auto last = width.find_last_not_of(" \t\n\r\f\v");
        width = width.substr(first, last - first + 1);

        WidthUnit unit = WidthUnit::PIXELS;
        std::size_t number_length = 0;
        if (width.back() == '%') {
          unit = WidthUnit::PERCENTAGE;
          number_length = width.size() - 1;
        } else if (width.size() >= 2 && width.compare(width.size() - 2, 2, "px") == 0) {
          unit = WidthUnit::PIXELS;
          number_length = width.size() - 2;
        }

        if (number_length > 0) {
          const auto* begin = width.data();
          const auto* end = begin + number_length;
          if (*begin == '+') {
            ++begin;
          }
          double value = 0.0;
          const auto result = std::from_chars(begin, end, value);
          if (begin != end && result.ec == std::errc{} && result.ptr == end &&
              std::isfinite(value) && value >= 0.0) {
            width_request_ = WidthRequest{value, unit};
          }
        }
      }
    }

    if (!parsed["percentage"].asString().empty() && parsed["percentage"].isNumeric()) {
      percentage_ = (int)lround(parsed["percentage"].asFloat());
    } else {
      percentage_ = 0;
    }
    break;
  }
}
