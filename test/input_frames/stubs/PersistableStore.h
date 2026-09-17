#pragma once
// Settings remain in memory; their fields/defaults are the production class.
template <typename T>
class PersistableStore {
 public:
  static T& getInstance() {
    static T instance;
    return instance;
  }
};
