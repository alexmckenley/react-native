/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <hermes/hermes.h>
#include <jsi/jsi.h>
#include <react/renderer/runtimescheduler/RuntimeScheduler.h>
#include <memory>
#include <semaphore>

#include "StubClock.h"
#include "StubErrorUtils.h"
#include "StubQueue.h"

namespace facebook::react {

using namespace std::chrono_literals;

class RuntimeSchedulerTest : public testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    hostFunctionCallCount_ = 0;
    runtime_ = facebook::hermes::makeHermesRuntime();
    stubErrorUtils_ = StubErrorUtils::createAndInstallIfNeeded(*runtime_);
    stubQueue_ = std::make_unique<StubQueue>();

    RuntimeExecutor runtimeExecutor =
        [this](
            std::function<void(facebook::jsi::Runtime & runtime)>&& callback) {
          stubQueue_->runOnQueue([this, callback = std::move(callback)]() {
            callback(*runtime_);
          });
        };

    stubClock_ = std::make_unique<StubClock>(StubClock());

    auto stubNow = [this]() -> RuntimeSchedulerTimePoint {
      return stubClock_->getNow();
    };

    auto useModernRuntimeScheduler = GetParam();

    runtimeScheduler_ = std::make_unique<RuntimeScheduler>(
        runtimeExecutor, useModernRuntimeScheduler, stubNow);
  }

  jsi::Function createHostFunctionFromLambda(
      std::function<jsi::Value(bool)> callback) {
    return jsi::Function::createFromHostFunction(
        *runtime_,
        jsi::PropNameID::forUtf8(*runtime_, ""),
        3,
        [this, callback = std::move(callback)](
            jsi::Runtime& /*unused*/,
            const jsi::Value& /*unused*/,
            const jsi::Value* arguments,
            size_t /*unused*/) -> jsi::Value {
          ++hostFunctionCallCount_;
          auto didUserCallbackTimeout = arguments[0].getBool();
          return callback(didUserCallbackTimeout);
        });
  }

  uint hostFunctionCallCount_{};

  std::unique_ptr<facebook::hermes::HermesRuntime> runtime_;
  std::unique_ptr<StubClock> stubClock_;
  std::unique_ptr<StubQueue> stubQueue_;
  std::unique_ptr<RuntimeScheduler> runtimeScheduler_;
  std::shared_ptr<StubErrorUtils> stubErrorUtils_;
};

TEST_P(RuntimeSchedulerTest, now) {
  stubClock_->setTimePoint(1ms);

  EXPECT_EQ(runtimeScheduler_->now(), RuntimeSchedulerTimePoint(1ms));

  stubClock_->advanceTimeBy(10ms);

  EXPECT_EQ(runtimeScheduler_->now(), RuntimeSchedulerTimePoint(11ms));

  stubClock_->advanceTimeBy(6s);

  EXPECT_EQ(runtimeScheduler_->now(), RuntimeSchedulerTimePoint(6011ms));
}

TEST_P(RuntimeSchedulerTest, getShouldYield) {
  // Always returns false for now.
  EXPECT_FALSE(runtimeScheduler_->getShouldYield());
}

TEST_P(RuntimeSchedulerTest, scheduleSingleTask) {
  bool didRunTask = false;
  auto callback =
      createHostFunctionFromLambda([&didRunTask](bool didUserCallbackTimeout) {
        didRunTask = true;
        EXPECT_FALSE(didUserCallbackTimeout);
        return jsi::Value::undefined();
      });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority, std::move(callback));

  EXPECT_FALSE(didRunTask);
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  EXPECT_TRUE(didRunTask);
  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, scheduleImmediatePriorityTask) {
  bool didRunTask = false;
  auto callback =
      createHostFunctionFromLambda([&didRunTask](bool didUserCallbackTimeout) {
        didRunTask = true;
        EXPECT_TRUE(didUserCallbackTimeout);
        return jsi::Value::undefined();
      });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::ImmediatePriority, std::move(callback));

  EXPECT_FALSE(didRunTask);
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  EXPECT_TRUE(didRunTask);
  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, taskExpiration) {
  bool didRunTask = false;
  auto callback =
      createHostFunctionFromLambda([&didRunTask](bool didUserCallbackTimeout) {
        didRunTask = true;
        EXPECT_TRUE(didUserCallbackTimeout);
        return jsi::Value::undefined();
      });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority, std::move(callback));

  // Task with normal priority has 5s timeout.
  stubClock_->advanceTimeBy(6s);

  EXPECT_FALSE(didRunTask);
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  EXPECT_TRUE(didRunTask);
  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, scheduleTwoTasksWithSamePriority) {
  uint firstTaskCallOrder = 0;
  auto callbackOne = createHostFunctionFromLambda(
      [this, &firstTaskCallOrder](bool /*unused*/) {
        firstTaskCallOrder = hostFunctionCallCount_;
        return jsi::Value::undefined();
      });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority, std::move(callbackOne));

  uint secondTaskCallOrder;
  auto callbackTwo = createHostFunctionFromLambda(
      [this, &secondTaskCallOrder](bool /*unused*/) {
        secondTaskCallOrder = hostFunctionCallCount_;
        return jsi::Value::undefined();
      });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority, std::move(callbackTwo));

  EXPECT_EQ(firstTaskCallOrder, 0);
  EXPECT_EQ(secondTaskCallOrder, 0);
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  EXPECT_EQ(firstTaskCallOrder, 1);
  EXPECT_EQ(secondTaskCallOrder, 2);
  EXPECT_EQ(stubQueue_->size(), 0);
  EXPECT_EQ(hostFunctionCallCount_, 2);
}

TEST_P(RuntimeSchedulerTest, scheduleTwoTasksWithDifferentPriorities) {
  uint lowPriorityTaskCallOrder = 0;
  auto callbackOne = createHostFunctionFromLambda(
      [this, &lowPriorityTaskCallOrder](bool /*unused*/) {
        lowPriorityTaskCallOrder = hostFunctionCallCount_;
        return jsi::Value::undefined();
      });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::LowPriority, std::move(callbackOne));

  uint userBlockingPriorityTaskCallOrder;
  auto callbackTwo = createHostFunctionFromLambda(
      [this, &userBlockingPriorityTaskCallOrder](bool /*unused*/) {
        userBlockingPriorityTaskCallOrder = hostFunctionCallCount_;
        return jsi::Value::undefined();
      });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::UserBlockingPriority, std::move(callbackTwo));

  EXPECT_EQ(lowPriorityTaskCallOrder, 0);
  EXPECT_EQ(userBlockingPriorityTaskCallOrder, 0);
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  EXPECT_EQ(lowPriorityTaskCallOrder, 2);
  EXPECT_EQ(userBlockingPriorityTaskCallOrder, 1);
  EXPECT_EQ(stubQueue_->size(), 0);
  EXPECT_EQ(hostFunctionCallCount_, 2);
}

TEST_P(RuntimeSchedulerTest, cancelTask) {
  bool didRunTask = false;
  auto callback = createHostFunctionFromLambda([&didRunTask](bool /*unused*/) {
    didRunTask = true;
    return jsi::Value::undefined();
  });

  auto task = runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority, std::move(callback));

  EXPECT_FALSE(didRunTask);
  EXPECT_EQ(stubQueue_->size(), 1);

  runtimeScheduler_->cancelTask(*task);

  stubQueue_->tick();

  EXPECT_FALSE(didRunTask);
  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, continuationTask) {
  bool didRunTask = false;
  bool didContinuationTask = false;

  auto callback = createHostFunctionFromLambda([&](bool /*unused*/) {
    didRunTask = true;
    return jsi::Function::createFromHostFunction(
        *runtime_,
        jsi::PropNameID::forUtf8(*runtime_, ""),
        1,
        [&](jsi::Runtime& /*runtime*/,
            jsi::Value const& /*unused*/,
            jsi::Value const* /*arguments*/,
            size_t /*unused*/) noexcept -> jsi::Value {
          didContinuationTask = true;
          return jsi::Value::undefined();
        });
  });

  auto task = runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority, std::move(callback));

  EXPECT_FALSE(didRunTask);
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  EXPECT_TRUE(didRunTask);
  EXPECT_TRUE(didContinuationTask);
  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, getCurrentPriorityLevel) {
  auto callback =
      createHostFunctionFromLambda([this](bool /*didUserCallbackTimeout*/) {
        EXPECT_EQ(
            runtimeScheduler_->getCurrentPriorityLevel(),
            SchedulerPriority::ImmediatePriority);
        return jsi::Value::undefined();
      });

  EXPECT_EQ(
      runtimeScheduler_->getCurrentPriorityLevel(),
      SchedulerPriority::NormalPriority);

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::ImmediatePriority, std::move(callback));

  stubQueue_->tick();

  EXPECT_EQ(
      runtimeScheduler_->getCurrentPriorityLevel(),
      SchedulerPriority::NormalPriority);

  callback =
      createHostFunctionFromLambda([this](bool /*didUserCallbackTimeout*/) {
        EXPECT_EQ(
            runtimeScheduler_->getCurrentPriorityLevel(),
            SchedulerPriority::IdlePriority);
        return jsi::Value::undefined();
      });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::IdlePriority, std::move(callback));

  stubQueue_->tick();

  EXPECT_EQ(
      runtimeScheduler_->getCurrentPriorityLevel(),
      SchedulerPriority::NormalPriority);
}

TEST_P(RuntimeSchedulerTest, scheduleWorkWithYielding) {
  bool wasCalled = false;
  runtimeScheduler_->scheduleWork(
      [&](const jsi::Runtime& /*unused*/) { wasCalled = true; });

  EXPECT_FALSE(wasCalled);

  EXPECT_TRUE(runtimeScheduler_->getShouldYield());

  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  EXPECT_TRUE(wasCalled);
  EXPECT_FALSE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, normalTaskYieldsToPlatformEvent) {
  // Only for legacy runtime scheduler
  if (GetParam()) {
    return;
  }

  bool didRunJavaScriptTask = false;
  bool didRunPlatformWork = false;

  auto callback = createHostFunctionFromLambda([&](bool /*unused*/) {
    didRunJavaScriptTask = true;
    EXPECT_TRUE(didRunPlatformWork);
    return jsi::Value::undefined();
  });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority, std::move(callback));

  runtimeScheduler_->scheduleWork([&](const jsi::Runtime& /*unused*/) {
    didRunPlatformWork = true;
    EXPECT_FALSE(didRunJavaScriptTask);
    EXPECT_FALSE(runtimeScheduler_->getShouldYield());
  });

  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 2);

  stubQueue_->flush();

  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, expiredTaskDoesntYieldToPlatformEvent) {
  // Only for legacy runtime scheduler
  if (GetParam()) {
    return;
  }

  bool didRunJavaScriptTask = false;
  bool didRunPlatformWork = false;

  auto callback = createHostFunctionFromLambda([&](bool /*unused*/) {
    didRunJavaScriptTask = true;
    EXPECT_FALSE(didRunPlatformWork);
    return jsi::Value::undefined();
  });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority, std::move(callback));

  runtimeScheduler_->scheduleWork([&](const jsi::Runtime& /*unused*/) {
    didRunPlatformWork = true;
    EXPECT_TRUE(didRunJavaScriptTask);
  });

  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 2);

  stubClock_->advanceTimeBy(6s);

  stubQueue_->flush();

  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, immediateTaskDoesntYieldToPlatformEvent) {
  // Only for legacy runtime scheduler
  if (GetParam()) {
    return;
  }

  bool didRunJavaScriptTask = false;
  bool didRunPlatformWork = false;

  auto callback = createHostFunctionFromLambda([&](bool /*unused*/) {
    didRunJavaScriptTask = true;
    EXPECT_FALSE(didRunPlatformWork);
    return jsi::Value::undefined();
  });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::ImmediatePriority, std::move(callback));

  runtimeScheduler_->scheduleWork([&](const jsi::Runtime& /*unused*/) {
    didRunPlatformWork = true;
    EXPECT_TRUE(didRunJavaScriptTask);
  });

  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 2);

  stubQueue_->flush();

  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, scheduleTaskWithYielding) {
  // Only for modern runtime scheduler
  if (!GetParam()) {
    return;
  }

  bool wasCalled = false;
  runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority,
      [&](const jsi::Runtime& /*unused*/) { wasCalled = true; });

  EXPECT_FALSE(wasCalled);

  EXPECT_TRUE(runtimeScheduler_->getShouldYield());

  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  EXPECT_TRUE(wasCalled);
  EXPECT_FALSE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, normalTaskYieldsToSynchronousAccess) {
  // Only for modern runtime scheduler
  if (!GetParam()) {
    return;
  }

  uint syncTaskExecutionCount = 0;
  uint normalTaskExecutionCount = 0;

  std::binary_semaphore signalTaskToSync{0};

  // No tasks scheduled, not yielding necessary.
  EXPECT_FALSE(runtimeScheduler_->getShouldYield());

  std::thread t1([this, &signalTaskToSync, &syncTaskExecutionCount]() {
    // Wait for the normal task to start executing
    signalTaskToSync.acquire();

    runtimeScheduler_->executeNowOnTheSameThread(
        [&syncTaskExecutionCount](jsi::Runtime& /*runtime*/) {
          syncTaskExecutionCount++;
        });
  });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority,
      [this, &normalTaskExecutionCount, &signalTaskToSync](
          jsi::Runtime& /*unused*/) {
        // Notify the "main" thread that it should request sync access.
        signalTaskToSync.release();

        // Wait for the sync access to request yielding
        while (!runtimeScheduler_->getShouldYield()) {
          // This is just to avoid the loop to take significant CPU while
          // waiting for the yield request.
          std::chrono::duration<int, std::milli> timespan(10);
          std::this_thread::sleep_for(timespan);
        }

        normalTaskExecutionCount++;
      });

  EXPECT_EQ(normalTaskExecutionCount, 0);
  EXPECT_EQ(syncTaskExecutionCount, 0);
  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  // Only the normal task has been scheduled at this point.
  EXPECT_EQ(stubQueue_->size(), 1);

  // This will start executing the normal task, which will unblock the thread
  // that will request sync access
  stubQueue_->tick();

  // The normal task yielded to the synchronous access, which is now waiting
  // to execute
  EXPECT_EQ(normalTaskExecutionCount, 1);
  EXPECT_EQ(syncTaskExecutionCount, 0);
  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 1);

  // Execute the synchronous access and wait for completion
  stubQueue_->tick();
  t1.join();

  EXPECT_EQ(syncTaskExecutionCount, 1);
  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  // The previous task is still in the queue (although it was executed already).
  EXPECT_EQ(stubQueue_->size(), 1);

  // Just empty the queue
  stubQueue_->tick();

  EXPECT_EQ(normalTaskExecutionCount, 1); // It hasn't executed again
  EXPECT_FALSE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, immediateTaskYieldsToSynchronousAccess) {
  // Only for modern runtime scheduler
  if (!GetParam()) {
    return;
  }

  uint syncTaskExecutionCount = 0;
  uint normalTaskExecutionCount = 0;

  std::binary_semaphore signalTaskToSync{0};

  // No tasks scheduled, not yielding necessary.
  EXPECT_FALSE(runtimeScheduler_->getShouldYield());

  std::thread t1([this, &signalTaskToSync, &syncTaskExecutionCount]() {
    // Wait for the normal task to start executing
    signalTaskToSync.acquire();

    runtimeScheduler_->executeNowOnTheSameThread(
        [&syncTaskExecutionCount](jsi::Runtime& /*runtime*/) {
          syncTaskExecutionCount++;
        });
  });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::ImmediatePriority,
      [this, &normalTaskExecutionCount, &signalTaskToSync](
          jsi::Runtime& /*unused*/) {
        // Notify the "main" thread that it should request sync access.
        signalTaskToSync.release();

        // Wait for the sync access to request yielding
        while (!runtimeScheduler_->getShouldYield()) {
          // This is just to avoid the loop to take significant CPU while
          // waiting for the yield request.
          std::chrono::duration<int, std::milli> timespan(10);
          std::this_thread::sleep_for(timespan);
        }

        normalTaskExecutionCount++;
      });

  EXPECT_EQ(normalTaskExecutionCount, 0);
  EXPECT_EQ(syncTaskExecutionCount, 0);
  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  // Only the normal task has been scheduled at this point.
  EXPECT_EQ(stubQueue_->size(), 1);

  // This will start executing the normal task, which will unblock the thread
  // that will request sync access
  stubQueue_->tick();

  // The normal task yielded to the synchronous access, which is now waiting
  // to execute
  EXPECT_EQ(normalTaskExecutionCount, 1);
  EXPECT_EQ(syncTaskExecutionCount, 0);
  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 1);

  // Execute the synchronous access and wait for completion
  stubQueue_->tick();
  t1.join();

  EXPECT_EQ(syncTaskExecutionCount, 1);
  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  // The previous task is still in the queue (although it was executed already),
  // so the sync task scheduled the work loop to process it.
  EXPECT_EQ(stubQueue_->size(), 1);

  // Just empty the queue
  stubQueue_->tick();

  EXPECT_EQ(normalTaskExecutionCount, 1); // It hasn't executed again
  EXPECT_FALSE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, scheduleTaskFromTask) {
  bool didRunFirstTask = false;
  bool didRunSecondTask = false;
  auto firstCallback = createHostFunctionFromLambda(
      [this, &didRunFirstTask, &didRunSecondTask](bool didUserCallbackTimeout) {
        didRunFirstTask = true;
        EXPECT_FALSE(didUserCallbackTimeout);

        auto secondCallback = createHostFunctionFromLambda(
            [&didRunSecondTask](bool didUserCallbackTimeout) {
              didRunSecondTask = true;
              EXPECT_TRUE(didUserCallbackTimeout);
              return jsi::Value::undefined();
            });

        runtimeScheduler_->scheduleTask(
            SchedulerPriority::ImmediatePriority, std::move(secondCallback));
        return jsi::Value::undefined();
      });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority, std::move(firstCallback));

  EXPECT_FALSE(didRunFirstTask);
  EXPECT_FALSE(didRunSecondTask);
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  EXPECT_TRUE(didRunFirstTask);
  EXPECT_TRUE(didRunSecondTask);
  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, handlingError) {
  bool didRunTask = false;
  auto firstCallback =
      createHostFunctionFromLambda([this, &didRunTask](bool /*unused*/) {
        didRunTask = true;
        throw jsi::JSError(*runtime_, "Test error");
        return jsi::Value::undefined();
      });

  runtimeScheduler_->scheduleTask(
      SchedulerPriority::NormalPriority, std::move(firstCallback));

  EXPECT_FALSE(didRunTask);
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  EXPECT_TRUE(didRunTask);
  EXPECT_EQ(stubQueue_->size(), 0);
  EXPECT_EQ(stubErrorUtils_->getReportFatalCallCount(), 1);
}

TEST_P(RuntimeSchedulerTest, basicSameThreadExecution) {
  bool didRunSynchronousTask = false;
  std::thread t1([this, &didRunSynchronousTask]() {
    runtimeScheduler_->executeNowOnTheSameThread(
        [this, &didRunSynchronousTask](jsi::Runtime& /*rt*/) {
          EXPECT_TRUE(runtimeScheduler_->getIsSynchronous());
          didRunSynchronousTask = true;
        });
    EXPECT_FALSE(runtimeScheduler_->getIsSynchronous());
  });

  auto hasTask = stubQueue_->waitForTask();

  EXPECT_TRUE(hasTask);
  EXPECT_FALSE(didRunSynchronousTask);
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  t1.join();

  EXPECT_TRUE(didRunSynchronousTask);
}

TEST_P(RuntimeSchedulerTest, sameThreadTaskCreatesImmediatePriorityTask) {
  bool didRunSynchronousTask = false;
  bool didRunSubsequentTask = false;
  std::thread t1([this, &didRunSynchronousTask, &didRunSubsequentTask]() {
    runtimeScheduler_->executeNowOnTheSameThread(
        [this, &didRunSynchronousTask, &didRunSubsequentTask](
            jsi::Runtime& runtime) {
          didRunSynchronousTask = true;

          auto callback = createHostFunctionFromLambda(
              [&didRunSubsequentTask](bool didUserCallbackTimeout) {
                didRunSubsequentTask = true;
                EXPECT_TRUE(didUserCallbackTimeout);
                return jsi::Value::undefined();
              });

          runtimeScheduler_->scheduleTask(
              SchedulerPriority::ImmediatePriority, std::move(callback));

          runtimeScheduler_->callExpiredTasks(runtime);
        });
  });

  auto hasTask = stubQueue_->waitForTask();

  EXPECT_TRUE(hasTask);
  EXPECT_FALSE(didRunSynchronousTask);
  EXPECT_FALSE(didRunSubsequentTask);
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  t1.join();

  EXPECT_TRUE(didRunSynchronousTask);
  EXPECT_TRUE(didRunSubsequentTask);
}

TEST_P(RuntimeSchedulerTest, sameThreadTaskCreatesLowPriorityTask) {
  bool didRunSynchronousTask = false;
  bool didRunSubsequentTask = false;
  std::thread t1([this, &didRunSynchronousTask, &didRunSubsequentTask]() {
    runtimeScheduler_->executeNowOnTheSameThread(
        [this, &didRunSynchronousTask, &didRunSubsequentTask](
            jsi::Runtime& runtime) {
          didRunSynchronousTask = true;

          auto callback = createHostFunctionFromLambda(
              [&didRunSubsequentTask](bool didUserCallbackTimeout) {
                didRunSubsequentTask = true;
                EXPECT_FALSE(didUserCallbackTimeout);
                return jsi::Value::undefined();
              });

          runtimeScheduler_->scheduleTask(
              SchedulerPriority::LowPriority, std::move(callback));

          // Only for legacy runtime scheduler
          if (!GetParam()) {
            runtimeScheduler_->callExpiredTasks(runtime);
          }

          EXPECT_FALSE(didRunSubsequentTask);
        });
  });

  auto hasTask = stubQueue_->waitForTask();

  EXPECT_TRUE(hasTask);
  EXPECT_FALSE(didRunSynchronousTask);
  EXPECT_FALSE(didRunSubsequentTask);
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  t1.join();

  EXPECT_TRUE(didRunSynchronousTask);
  EXPECT_FALSE(didRunSubsequentTask);

  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  EXPECT_TRUE(didRunSubsequentTask);

  EXPECT_EQ(stubQueue_->size(), 0);
}

TEST_P(RuntimeSchedulerTest, legacyTwoThreadsRequestAccessToTheRuntime) {
  // Only for legacy runtime scheduler
  if (GetParam()) {
    return;
  }

  bool didRunSynchronousTask = false;
  bool didRunWork = false;

  runtimeScheduler_->scheduleWork(
      [&didRunWork](jsi::Runtime& /*unused*/) { didRunWork = true; });

  std::thread t1([this, &didRunSynchronousTask]() {
    runtimeScheduler_->executeNowOnTheSameThread(
        [&didRunSynchronousTask](jsi::Runtime& /*runtime*/) {
          didRunSynchronousTask = true;
        });
  });

  auto hasTask = stubQueue_->waitForTasks(2);

  EXPECT_TRUE(hasTask);
  EXPECT_FALSE(didRunWork);
  EXPECT_FALSE(didRunSynchronousTask);
  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 2);

  stubQueue_->tick();

  EXPECT_TRUE(didRunWork);
  EXPECT_FALSE(didRunSynchronousTask);
  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();

  t1.join();

  EXPECT_TRUE(didRunWork);
  EXPECT_TRUE(didRunSynchronousTask);
  EXPECT_FALSE(runtimeScheduler_->getShouldYield());
}

TEST_P(RuntimeSchedulerTest, modernTwoThreadsRequestAccessToTheRuntime) {
  // Only for modern runtime scheduler
  if (!GetParam()) {
    return;
  }

  std::binary_semaphore signalTask1ToScheduleTask2{0};
  std::binary_semaphore signalTask2ToResumeTask1{0};

  bool didRunSynchronousTask1 = false;
  bool didRunSynchronousTask2 = false;

  std::thread t1([this,
                  &didRunSynchronousTask1,
                  &signalTask1ToScheduleTask2,
                  &signalTask2ToResumeTask1]() {
    runtimeScheduler_->executeNowOnTheSameThread(
        [&didRunSynchronousTask1,
         &signalTask1ToScheduleTask2,
         &signalTask2ToResumeTask1](jsi::Runtime& /*runtime*/) {
          // Notify that the second task can be scheduled.
          signalTask1ToScheduleTask2.release();

          // Wait for the second task to be scheduled before finishing this task
          signalTask2ToResumeTask1.acquire();

          didRunSynchronousTask1 = true;
        });
  });

  std::thread t2([this,
                  &didRunSynchronousTask2,
                  &signalTask1ToScheduleTask2,
                  &signalTask2ToResumeTask1]() {
    // Wait for the first task to start executing before scheduling this one.
    signalTask1ToScheduleTask2.acquire();

    // Notify the first task that it can resume execution.
    // As we can't do this after the task this from thread has been scheduled
    // (because it's synchronous), we can just do a short wait instead in a new
    // thread.
    std::thread t3([&signalTask2ToResumeTask1]() {
      std::chrono::duration<int, std::milli> timespan(50);
      std::this_thread::sleep_for(timespan);
      signalTask2ToResumeTask1.release();
    });

    runtimeScheduler_->executeNowOnTheSameThread(
        [&didRunSynchronousTask2](jsi::Runtime& /*runtime*/) {
          didRunSynchronousTask2 = true;
        });

    t3.join();
  });

  auto hasTask = stubQueue_->waitForTasks(1);

  EXPECT_TRUE(hasTask);
  EXPECT_FALSE(didRunSynchronousTask1);
  EXPECT_FALSE(didRunSynchronousTask2);
  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  // Only the first task would have been scheduled at this point.
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();
  t1.join();

  EXPECT_TRUE(didRunSynchronousTask1);
  EXPECT_FALSE(didRunSynchronousTask2);
  EXPECT_TRUE(runtimeScheduler_->getShouldYield());
  // Now we've scheduled the second task.
  EXPECT_EQ(stubQueue_->size(), 1);

  stubQueue_->tick();
  t2.join();

  EXPECT_TRUE(didRunSynchronousTask1);
  EXPECT_TRUE(didRunSynchronousTask2);
  EXPECT_FALSE(runtimeScheduler_->getShouldYield());
  EXPECT_EQ(stubQueue_->size(), 0);
}

INSTANTIATE_TEST_SUITE_P(
    UseModernRuntimeScheduler,
    RuntimeSchedulerTest,
    testing::Values(false, true));

} // namespace facebook::react
