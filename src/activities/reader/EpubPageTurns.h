#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstdint>
#include <optional>

class Section;

// The reader owns this FIFO for its entire lifetime. Admission and completion
// run on main; the render task reports preparation errors to main separately.
class EpubPageTurns {
 public:
  // Twelve supported taps during a one-second stall, plus four outstanding turns.
  static constexpr unsigned CAPACITY = 16;
  enum class Action : uint8_t { PreviousPage, NextPage, PreviousChapter, NextChapter };
  enum class Outcome : uint8_t { Applied, Boundary, ContextChanged, Failed };
  enum class Step : uint8_t { Empty, NeedsNextPage, Applied, SectionChanged, Boundary };
  struct Request {
    uint32_t sequence;
    uint32_t capturedAtMs;
    uint32_t acceptedAtMs;
    Action action;
  };
  struct Counts {
    uint32_t accepted = 0;
    uint32_t rejected = 0;
    uint32_t applied = 0;
    uint32_t cancelled = 0;
    uint32_t failed = 0;
    uint32_t pending = 0;
    uint32_t highWater = 0;
    uint32_t maxWaitMs = 0;
  };

 private:
  StaticQueue_t queueControl{};
  alignas(Request) uint8_t storage[CAPACITY * sizeof(Request)]{};
  QueueHandle_t queue = nullptr;
  Counts counts;

 public:
  EpubPageTurns();
  ~EpubPageTurns();
  EpubPageTurns(const EpubPageTurns&) = delete;
  EpubPageTurns& operator=(const EpubPageTurns&) = delete;

  bool available() const { return queue != nullptr; }
  bool accept(Action action, uint32_t capturedAtMs, uint32_t nowMs);
  bool peek(Request& request) const;
  void complete(Outcome outcome, uint32_t nowMs);
  void cancelPending(Outcome outcome, uint32_t nowMs);
  Counts getCounts() const;
  // Caller holds RenderLock. A partial watermark is never a chapter boundary.
  // SectionChanged requires the caller to release the old Section and load the
  // new spine at page zero, or pendingPageJump when returning to its last page.
  Step applyTo(Section& section, int& spineIndex, int spineCount, std::optional<uint16_t>& pendingPageJump,
               uint32_t nowMs);
};
