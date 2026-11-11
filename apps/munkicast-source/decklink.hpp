// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QImage>
#include <functional>
#include <memory>
#include <span>
#include <string>

class Decklink {
  public:
    using Video = std::function<void(QImage)>;
    using Audio = std::function<void(std::span<const float>)>;
    static std::unique_ptr<Decklink> open(int index, const std::string& mode, Video video,
                                          Audio audio, std::string& error);
    ~Decklink();

  private:
    struct State;
    explicit Decklink(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
