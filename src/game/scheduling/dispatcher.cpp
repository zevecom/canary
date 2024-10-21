/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2024 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "game/scheduling/dispatcher.hpp"
#include "lib/thread/thread_pool.hpp"
#include "lib/di/container.hpp"
#include "utils/tools.hpp"

thread_local DispatcherContext Dispatcher::dispacherContext;

Dispatcher &Dispatcher::getInstance() {
	return inject<Dispatcher>();
}

void Dispatcher::init() {
	UPDATE_OTSYS_TIME();

	threadPool.detach_task([this] {
		std::unique_lock asyncLock(dummyMutex);

		while (!threadPool.isStopped()) {
			UPDATE_OTSYS_TIME();

			executeEvents();
			executeScheduledEvents();
			mergeEvents();

			if (!hasPendingTasks) {
				signalSchedule.wait_for(asyncLock, timeUntilNextScheduledTask());
			}
		}
	});
}

void Dispatcher::executeSerialEvents(std::vector<Task> &tasks) {
	dispacherContext.group = TaskGroup::Serial;
	dispacherContext.type = DispatcherType::Event;

	for (const auto &task : tasks) {
		dispacherContext.taskName = task.getContext();
		if (task.execute()) {
			++dispatcherCycle;
		}
	}
	tasks.clear();

	dispacherContext.reset();
}

void Dispatcher::executeParallelEvents(std::vector<Task> &tasks, const uint8_t groupId) {
	asyncWait(tasks.size(), [groupId, &tasks](size_t i) {
		dispacherContext.type = DispatcherType::AsyncEvent;
		dispacherContext.group = static_cast<TaskGroup>(groupId);
		tasks[i].execute();

		dispacherContext.reset();
	});

	tasks.clear();
}

void Dispatcher::asyncWait(size_t requestSize, std::function<void(size_t i)> &&f) {
	if (requestSize == 0) {
		return;
	}

	// This prevents an async call from running inside another async call.
	if (asyncWaitDisabled) {
		for (uint_fast64_t i = 0; i < requestSize; ++i) {
			f(i);
		}
		return;
	}

	const auto &partitions = generatePartition(requestSize);
	const auto pSize = partitions.size();

	BS::multi_future<void> retFuture;

	if (pSize > 1) {
		asyncWaitDisabled = true;
		const auto min = partitions[1].first;
		const auto max = partitions[partitions.size() - 1].second;
		retFuture = threadPool.submit_loop(min, max, [&f](const unsigned int i) { f(i); });
	}

	const auto &[min, max] = partitions[0];
	for (uint_fast64_t i = min; i < max; ++i) {
		f(i);
	}

	if (pSize > 1) {
		retFuture.wait();
		asyncWaitDisabled = false;
	}
}

void Dispatcher::executeEvents(const TaskGroup startGroup) {
	for (uint_fast8_t groupId = static_cast<uint8_t>(startGroup); groupId < static_cast<uint8_t>(TaskGroup::Last); ++groupId) {
		auto &tasks = m_tasks[groupId];
		if (tasks.empty()) {
			return;
		}

		if (groupId == static_cast<uint8_t>(TaskGroup::Serial)) {
			executeSerialEvents(tasks);
			mergeAsyncEvents();
		} else {
			executeParallelEvents(tasks, groupId);
		}
	}
}

void Dispatcher::executeScheduledEvents() {
	auto &threadScheduledTasks = getThreadTask()->scheduledTasks;

	auto it = scheduledTasks.begin();
	while (it != scheduledTasks.end()) {
		const auto &task = *it;
		if (task->getTime() > OTSYS_TIME()) {
			break;
		}

		dispacherContext.type = task->isCycle() ? DispatcherType::CycleEvent : DispatcherType::ScheduledEvent;
		dispacherContext.group = TaskGroup::Serial;
		dispacherContext.taskName = task->getContext();

		if (task->execute() && task->isCycle()) {
			task->updateTime();
			threadScheduledTasks.emplace_back(task);
		} else {
			scheduledTasksRef.erase(task->getId());
		}

		++it;
	}

	if (it != scheduledTasks.begin()) {
		scheduledTasks.erase(scheduledTasks.begin(), it);
	}

	dispacherContext.reset();

	mergeAsyncEvents(); // merge async events requested by scheduled events
	executeEvents(TaskGroup::GenericParallel); // execute async events requested by scheduled events
}

// Merge only async thread events with main dispatch events
void Dispatcher::mergeAsyncEvents() {
	constexpr uint8_t start = static_cast<uint8_t>(TaskGroup::GenericParallel);
	constexpr uint8_t end = static_cast<uint8_t>(TaskGroup::Last);

	for (const auto &thread : threads) {
		std::scoped_lock lock(thread->mutex);
		for (uint_fast8_t i = start; i < end; ++i) {
			if (!thread->tasks[i].empty()) {
				m_tasks[i].insert(m_tasks[i].end(), make_move_iterator(thread->tasks[i].begin()), make_move_iterator(thread->tasks[i].end()));
				thread->tasks[i].clear();
			}
		}
	}
}

// Merge thread events with main dispatch events
void Dispatcher::mergeEvents() {
	constexpr uint8_t serial = static_cast<uint8_t>(TaskGroup::Serial);

	for (const auto &thread : threads) {
		std::scoped_lock lock(thread->mutex);
		if (!thread->tasks[serial].empty()) {
			m_tasks[serial].insert(m_tasks[serial].end(), make_move_iterator(thread->tasks[serial].begin()), make_move_iterator(thread->tasks[serial].end()));
			thread->tasks[serial].clear();
		}

		if (!thread->scheduledTasks.empty()) {
			scheduledTasks.insert(make_move_iterator(thread->scheduledTasks.begin()), make_move_iterator(thread->scheduledTasks.end()));
			thread->scheduledTasks.clear();
		}
	}

	checkPendingTasks();
}

std::chrono::milliseconds Dispatcher::timeUntilNextScheduledTask() const {
	constexpr auto CHRONO_0 = std::chrono::milliseconds(0);
	constexpr auto CHRONO_MILI_MAX = std::chrono::milliseconds::max();

	if (scheduledTasks.empty()) {
		return CHRONO_MILI_MAX;
	}

	const auto &task = *scheduledTasks.begin();
	const auto timeRemaining = std::chrono::milliseconds(task->getTime() - OTSYS_TIME());
	return std::max<std::chrono::milliseconds>(timeRemaining, CHRONO_0);
}

void Dispatcher::addEvent(std::function<void(void)> &&f, std::string_view context, uint32_t expiresAfterMs) {
	const auto &thread = getThreadTask();
	std::scoped_lock lock(thread->mutex);
	thread->tasks[static_cast<uint8_t>(TaskGroup::Serial)].emplace_back(expiresAfterMs, std::move(f), context);
	notify();
}

uint64_t Dispatcher::scheduleEvent(const std::shared_ptr<Task> &task) {
	const auto &thread = getThreadTask();
	std::scoped_lock lock(thread->mutex);

	auto eventId = scheduledTasksRef
					   .emplace(task->getId(), thread->scheduledTasks.emplace_back(task))
					   .first->first;

	notify();
	return eventId;
}

void Dispatcher::asyncEvent(std::function<void(void)> &&f, TaskGroup group) {
	const auto &thread = getThreadTask();
	std::scoped_lock lock(thread->mutex);
	thread->tasks[static_cast<uint8_t>(group)].emplace_back(0, std::move(f), dispacherContext.taskName);
	notify();
}

void Dispatcher::stopEvent(uint64_t eventId) {
	auto it = scheduledTasksRef.find(eventId);
	if (it != scheduledTasksRef.end()) {
		it->second->cancel();
		scheduledTasksRef.erase(it);
	}
}

void DispatcherContext::addEvent(std::function<void(void)> &&f, std::string_view context) const {
	g_dispatcher().addEvent(std::move(f), context);
}

void DispatcherContext::tryAddEvent(std::function<void(void)> &&f, std::string_view context) const {
	if (!f) {
		return;
	}

	if (isAsync()) {
		g_dispatcher().addEvent(std::move(f), context);
	} else {
		f();
	}
}

bool DispatcherContext::isOn() {
	return OTSYS_TIME() != 0;
}
