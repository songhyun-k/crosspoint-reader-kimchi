#pragma once

namespace BoardConfig {
struct ViewableInsets {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
};
struct Profile {
  ViewableInsets viewableInsets;
};
inline constexpr Profile ACTIVE{};
}  // namespace BoardConfig
