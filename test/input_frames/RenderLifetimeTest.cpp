#include <gtest/gtest.h>

#include <future>

#include "activities/Activity.h"

HalDisplay display;
namespace {
HalGPIO managerInput;
GfxRenderer managerRenderer(display);
MappedInputManager managerMappedInput(managerInput, managerRenderer);

struct RenderTrace {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned renders = 0;
  unsigned releasedRenders = 0;
  bool cleanupEntered = false;
  bool releaseCleanup = false;
  bool cleaned = false;
  bool cleanupOnWorker = false;
  bool destroyed = false;

  void waitForRender(unsigned number) {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return renders >= number; });
  }
  void releaseRender(unsigned number) {
    std::lock_guard lock(mutex);
    releasedRenders = number;
    changed.notify_all();
  }
};

// Records only callback boundaries. Scheduling, locking, target selection and
// acknowledgement all run through the production ActivityManager.
class ObservedActivity : public Activity {
  std::shared_ptr<RenderTrace> trace;

 public:
  explicit ObservedActivity(std::shared_ptr<RenderTrace> trace)
      : Activity("Observed", managerRenderer, managerMappedInput), trace(std::move(trace)) {}
  ~ObservedActivity() override {
    std::lock_guard lock(trace->mutex);
    trace->destroyed = true;
  }
  void render(RenderLock&&) override {
    std::unique_lock lock(trace->mutex);
    const auto number = ++trace->renders;
    trace->changed.notify_all();
    trace->changed.wait(lock, [&] { return trace->releasedRenders >= number; });
  }
  void onRenderExit() override {
    std::unique_lock lock(trace->mutex);
    trace->cleanupEntered = true;
    trace->cleanupOnWorker = inputTest::currentTask == inputTest::task;
    trace->changed.notify_all();
    trace->changed.wait(lock, [&] { return trace->releaseCleanup; });
    trace->cleaned = true;
    trace->changed.notify_all();
  }
};

void waitUntilNotifiedTaskSleeps(StaticTask_t& task) {
  std::unique_lock lock(task.mutex);
  task.condition.wait(lock, [&] { return task.sleeping; });
}
unsigned notifications(StaticTask_t& task) {
  std::lock_guard lock(task.mutex);
  return task.notifications;
}
}  // namespace

// Like firmware, this manager is process-lifetime; tests use the native Release runner.
ActivityManager activityManager(managerRenderer, managerMappedInput);

TEST(RenderLifetimeTest, ResultRecipientCanConsumePayloadWithoutCopying) {
  FootnoteEntry entry;
  std::strcpy(entry.number, "1");
  std::strcpy(entry.href, "#note");
  ActivityResult result{FootnoteResult{"#note", {entry}}};
  const auto* entries = std::get<FootnoteResult>(result.data).footnotes.data();
  ActivityResultHandler observer = [](const ActivityResult& value) {
    EXPECT_EQ(std::get<FootnoteResult>(value.data).href, "#note");
  };
  observer(std::move(result));
  std::vector<FootnoteEntry> received;
  ActivityResultHandler consumer = [&](ActivityResult&& value) {
    received = std::move(std::get<FootnoteResult>(value.data).footnotes);
  };
  consumer(std::move(result));
  EXPECT_EQ(received.data(), entries);
  ASSERT_EQ(received.size(), 1u);
  EXPECT_STREQ(received.front().href, "#note");
}

TEST(RenderLifetimeTest, OlderRenderCannotAcknowledgeANewWaitOrRetirement) {
  const auto trace = std::make_shared<RenderTrace>();
  const auto cleanup = std::make_shared<RenderTrace>();
  auto stackedActivity = std::make_unique<ObservedActivity>(cleanup);
  EXPECT_FALSE(activityManager.retireActivity(*stackedActivity));
  EXPECT_FALSE(cleanup->cleaned);
  activityManager.replaceActivity(std::make_unique<ObservedActivity>(trace));
  activityManager.begin();
  inputTest::waitForSample();

  activityManager.requestUpdate(true);
  trace->waitForRender(1);
  StaticTask_t waiter;
  auto waiting = std::async(std::launch::async, [&] {
    inputTest::currentTask = &waiter;
    activityManager.requestUpdateAndWait();
    inputTest::currentTask = nullptr;
  });
  waitUntilNotifiedTaskSleeps(waiter);
  trace->releaseRender(1);
  trace->waitForRender(2);
  // This check observes the notification itself; it cannot pass merely because
  // the waiting host thread has not yet been rescheduled.
  EXPECT_EQ(notifications(waiter), 0u);
  EXPECT_EQ(waiting.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
  trace->releaseRender(2);
  waiting.get();
  EXPECT_EQ(notifications(waiter), 1u);

  activityManager.requestUpdate(true);
  trace->waitForRender(3);
  StaticTask_t retirementWaiter;
  auto retirement = std::async(std::launch::async, [&] {
    inputTest::currentTask = &retirementWaiter;
    const bool retired = activityManager.retireActivity(*stackedActivity);
    inputTest::currentTask = nullptr;
    return retired;
  });
  waitUntilNotifiedTaskSleeps(retirementWaiter);
  trace->releaseRender(3);
  {
    std::unique_lock lock(cleanup->mutex);
    cleanup->changed.wait(lock, [&] { return cleanup->cleanupEntered; });
    EXPECT_TRUE(cleanup->cleanupOnWorker);
    EXPECT_FALSE(cleanup->cleaned);
    EXPECT_FALSE(cleanup->destroyed);
    EXPECT_EQ(notifications(retirementWaiter), 0u);
    EXPECT_EQ(retirement.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
    cleanup->releaseCleanup = true;
    cleanup->changed.notify_all();
  }
  EXPECT_TRUE(retirement.get());
  EXPECT_EQ(notifications(retirementWaiter), 1u);
  {
    std::unique_lock lock(cleanup->mutex);
    EXPECT_TRUE(cleanup->cleaned);
    // Preserve safe fixture teardown even when the acknowledgement assertion fails.
    cleanup->changed.wait(lock, [&] { return cleanup->cleaned; });
  }
  stackedActivity.reset();
  EXPECT_TRUE(cleanup->destroyed);
  vTaskDelete(inputTest::task);
}
