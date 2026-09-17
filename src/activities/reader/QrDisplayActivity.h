#pragma once
#include <I18n.h>

#include <string>

#include "activities/Activity.h"

class QrDisplayActivity final : public Activity {
 public:
  explicit QrDisplayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string textPayload)
      : Activity("QrDisplay", renderer, mappedInput), textPayload(std::move(textPayload)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string textPayload;
};
