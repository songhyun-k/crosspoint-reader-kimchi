#include "EpubPageTurns.h"

#include <Epub/Section.h>
#include <Logging.h>

#include <algorithm>

EpubPageTurns::EpubPageTurns() {
  queue = xQueueCreateStatic(CAPACITY, sizeof(Request), storage, &queueControl);
  if (!queue) LOG_ERR("ERS", "Failed to create page-turn queue");
}

EpubPageTurns::~EpubPageTurns() {
  if (queue) vQueueDelete(queue);
}

bool EpubPageTurns::accept(const Action action, const uint32_t capturedAtMs, const uint32_t nowMs) {
  const Request request{counts.accepted + 1, capturedAtMs, nowMs, action};
  if (!queue || xQueueSend(queue, &request, 0) != pdTRUE) {
    if (counts.rejected == 0) LOG_ERR("ERS", "Page-turn queue unavailable or full; rejecting new requests");
    ++counts.rejected;
    return false;
  }
  ++counts.accepted;
  counts.highWater = std::max(counts.highWater, static_cast<uint32_t>(uxQueueMessagesWaiting(queue)));
  return true;
}

bool EpubPageTurns::peek(Request& request) const { return queue && xQueuePeek(queue, &request, 0) == pdTRUE; }

void EpubPageTurns::complete(const Outcome outcome, const uint32_t nowMs) {
  Request request{};
  if (!queue || xQueueReceive(queue, &request, 0) != pdTRUE) return;
  counts.maxWaitMs = std::max(counts.maxWaitMs, nowMs - request.acceptedAtMs);
  switch (outcome) {
    case Outcome::Applied:
      ++counts.applied;
      break;
    case Outcome::Boundary:
    case Outcome::ContextChanged:
      ++counts.cancelled;
      break;
    case Outcome::Failed:
      ++counts.failed;
      break;
  }
}

void EpubPageTurns::cancelPending(const Outcome outcome, const uint32_t nowMs) {
  Request request{};
  while (peek(request)) complete(outcome, nowMs);
}

EpubPageTurns::Counts EpubPageTurns::getCounts() const {
  Counts result = counts;
  result.pending = queue ? uxQueueMessagesWaiting(queue) : 0;
  return result;
}

EpubPageTurns::Step EpubPageTurns::applyTo(Section& section, int& spineIndex, const int spineCount,
                                           std::optional<uint16_t>& pendingPageJump, const uint32_t nowMs) {
  Request request{};
  if (!peek(request)) return Step::Empty;
  if (spineIndex >= spineCount) {
    complete(Outcome::Boundary, nowMs);
    return Step::Boundary;
  }
  const bool forward = request.action == Action::NextPage || request.action == Action::NextChapter;
  const bool chapter = request.action == Action::PreviousChapter || request.action == Action::NextChapter;
  if (forward && !chapter && section.currentPage + 1 >= static_cast<int>(section.pageCount) &&
      (section.isBuilding() || section.isPartial())) {
    return Step::NeedsNextPage;
  }

  Step step = Step::Applied;
  if (forward) {
    if (!chapter && section.currentPage + 1 < static_cast<int>(section.pageCount)) {
      ++section.currentPage;
    } else {
      ++spineIndex;
      pendingPageJump.reset();
      step = Step::SectionChanged;
    }
  } else if (section.currentPage > 0) {
    section.currentPage = chapter ? 0 : section.currentPage - 1;
  } else if (spineIndex > 0) {
    --spineIndex;
    pendingPageJump = chapter ? std::nullopt : std::make_optional(UINT16_MAX);
    step = Step::SectionChanged;
  } else {
    step = Step::Boundary;
  }
  complete(step == Step::Boundary ? Outcome::Boundary : Outcome::Applied, nowMs);
  return step;
}
