// ======================================================================== //
// Copyright 2009-2016 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "taskschedulerinternal.h"
#include "../math/math.h"
#include "../sys/sysinfo.h"
#include <algorithm>

namespace embree
{
  size_t TaskScheduler::g_numThreads = 0;
  __thread TaskScheduler* TaskScheduler::g_instance = nullptr;
  __thread TaskScheduler::Thread* TaskScheduler::thread_local_thread = nullptr;
  TaskScheduler::ThreadPool* TaskScheduler::threadPool = nullptr;

  template<typename Predicate, typename Body>
  __forceinline void TaskScheduler::steal_loop(Thread& thread, const Predicate& pred, const Body& body)
  {
    while (true)
    {
      /*! some rounds that yield */
      for (size_t i=0; i<32; i++)
      {
        /*! some spinning rounds */
        const size_t threadCount = thread.threadCount();
        for (size_t j=0; j<1024; j+=threadCount)
        {
          if (!pred()) return;
          if (thread.scheduler->steal_from_other_threads(thread)) {
            i=j=0;
            body();
          }
        }
        yield();
      }
    }
  }

  /*! run this task */
  __dllexport void TaskScheduler::Task::run (Thread& thread) // FIXME: avoid as many __dllexports as possible
  {
    /* try to run if not already stolen */
    if (try_switch_state(INITIALIZED,DONE))
    {
      Task* prevTask = thread.task; 
      thread.task = this;
      try {
        if (thread.scheduler->cancellingException == nullptr)
          closure->execute();
      } catch (...) {
        if (thread.scheduler->cancellingException == nullptr)
          thread.scheduler->cancellingException = std::current_exception();
      }
      thread.task = prevTask;
      add_dependencies(-1);
    }
    
    /* steal until all dependencies have completed */
    steal_loop(thread,
               [&] () { return dependencies>0; },
               [&] () { while (thread.tasks.execute_local(thread,this)); });

    /* now signal our parent task that we are finished */
    if (parent) 
      parent->add_dependencies(-1);
  }

  __dllexport bool TaskScheduler::TaskQueue::execute_local(Thread& thread, Task* parent)
  {
    /* stop if we run out of local tasks or reach the waiting task */
    if (right == 0 || &tasks[right-1] == parent)
      return false;
    
    /* execute task */
    size_t oldRight = right;
    tasks[right-1].run(thread);
    if (right != oldRight) {
      THROW_RUNTIME_ERROR("you have to wait for spawned subtasks");
    }
    
    /* pop task and closure from stack */
    right--;
    if (tasks[right].stackPtr != size_t(-1))
      stackPtr = tasks[right].stackPtr;
    
    /* also move left pointer */
    if (left >= right) left.store(right.load());
    
    return right != 0;
  }
  
  bool TaskScheduler::TaskQueue::steal(Thread& thread) 
  {
    size_t l = left;
    if (l < right) 
      l = left++;
    else 
      return false;
    
    if (!tasks[l].try_steal(thread.tasks.tasks[thread.tasks.right]))
      return false;
    
    thread.tasks.right++;
    return true;
  }
  
  /* we steal from the left */
  size_t TaskScheduler::TaskQueue::getTaskSizeAtLeft() 
  {	
    if (left >= right) return 0;
    return tasks[left].N;
  }

  static MutexSys g_mutex;
  static BarrierSys g_barrier(2);

  void threadPoolFunction(std::pair<TaskScheduler::ThreadPool*,size_t>* pair)
  {
    TaskScheduler::ThreadPool* pool = pair->first;
    size_t threadIndex = pair->second;
    g_barrier.wait();
    pool->thread_loop(threadIndex);
  }

  TaskScheduler::ThreadPool::ThreadPool(bool set_affinity)
    : numThreads(0), numThreadsRunning(0), set_affinity(set_affinity), running(false) {}

  __dllexport void TaskScheduler::ThreadPool::startThreads()
  {
    if (running) return;
    setNumThreads(numThreads,true);
  }

  void TaskScheduler::ThreadPool::setNumThreads(size_t newNumThreads, bool startThreads)
  {
    Lock<MutexSys> lock(g_mutex);
    
    if (newNumThreads == 0)
      newNumThreads = getNumberOfLogicalThreads();

    numThreads = newNumThreads;
    if (!startThreads && !running) return;
    running = true;
    size_t numThreadsActive = numThreadsRunning;

    mutex.lock();
    numThreadsRunning = newNumThreads;
    mutex.unlock();
    condition.notify_all();

    /* start new threads */
    for (size_t t=numThreadsActive; t<numThreads; t++) 
    {
      if (t == 0) continue;
      auto pair = std::make_pair(this,t);
      threads.push_back(createThread((thread_func)threadPoolFunction,&pair,4*1024*1024,set_affinity ? t : -1));
      g_barrier.wait();
    }

    /* stop some threads if we reduce the number of threads */
    for (ssize_t t=numThreadsActive-1; t>=ssize_t(numThreadsRunning); t--) {
      if (t == 0) continue;
      embree::join(threads.back());
      threads.pop_back();
    }
  }

  TaskScheduler::ThreadPool::~ThreadPool()
  {
    /* leave all taskschedulers */
    mutex.lock();
    numThreadsRunning = 0;
    mutex.unlock();
    condition.notify_all();

    /* wait for threads to terminate */
    for (size_t i=0; i<threads.size(); i++) 
      embree::join(threads[i]);
  }

  __dllexport void TaskScheduler::ThreadPool::add(const Ref<TaskScheduler>& scheduler)
  {
    mutex.lock();
    schedulers.push_back(scheduler);
    mutex.unlock();
    condition.notify_all();
  }

  __dllexport void TaskScheduler::ThreadPool::remove(const Ref<TaskScheduler>& scheduler)
  {
    Lock<MutexSys> lock(mutex);
    for (std::list<Ref<TaskScheduler> >::iterator it = schedulers.begin(); it != schedulers.end(); it++) {
      if (scheduler == *it) {
        schedulers.erase(it);
        return;
      }
    }
  }

  void TaskScheduler::ThreadPool::thread_loop(size_t globalThreadIndex)
  {
    while (globalThreadIndex < numThreadsRunning)
    {
      Ref<TaskScheduler> scheduler = NULL;
      ssize_t threadIndex = -1;
      {
        Lock<MutexSys> lock(mutex);
        condition.wait(mutex, [&] () { return globalThreadIndex >= numThreadsRunning || !schedulers.empty(); });
        if (globalThreadIndex >= numThreadsRunning) break;
        scheduler = schedulers.front();
        threadIndex = scheduler->allocThreadIndex();
      }
      scheduler->thread_loop(threadIndex);
    }
  }
  
  TaskScheduler::TaskScheduler()
    : threadCounter(0), anyTasksRunning(0), hasRootTask(false) 
  {
    threadLocal.resize(2*getNumberOfLogicalThreads()); // FIXME: this has to be 2x as in the join mode the worker threads also join
    for (size_t i=0; i<threadLocal.size(); i++)
      threadLocal[i].store(nullptr);
  }
  
  TaskScheduler::~TaskScheduler() 
  {
    assert(threadCounter == 0);
  }

  __dllexport size_t TaskScheduler::threadIndex() 
  {
    Thread* thread = TaskScheduler::thread();
    if (thread) return thread->threadIndex;
    else        return 0;
  }

  __dllexport size_t TaskScheduler::threadCount() {
    return threadPool->size();
  }

  __dllexport TaskScheduler* TaskScheduler::instance() 
  {
    if (g_instance == NULL) {
      g_instance = new TaskScheduler;
      g_instance->refInc();
    }
    return g_instance;
  }

  void TaskScheduler::create(size_t numThreads, bool set_affinity)
  {
    if (!threadPool) threadPool = new TaskScheduler::ThreadPool(set_affinity);
    threadPool->setNumThreads(numThreads,false);
  }

  void TaskScheduler::destroy() {
    delete threadPool; threadPool = nullptr;
  }

  __dllexport ssize_t TaskScheduler::allocThreadIndex()
  {
    size_t threadIndex = threadCounter++;
    assert(threadIndex < threadLocal.size());
    return threadIndex;
  }

  void TaskScheduler::join()
  {
    mutex.lock();
    size_t threadIndex = allocThreadIndex();
    condition.wait(mutex, [&] () { return hasRootTask.load(); });
    mutex.unlock();
    std::exception_ptr except = thread_loop(threadIndex);
    if (except != nullptr) std::rethrow_exception(except);
  }

  void TaskScheduler::reset() {
    hasRootTask = false;
  }

  void TaskScheduler::wait_for_threads(size_t threadCount)
  {
    while (threadCounter < threadCount-1)
      __pause_cpu();
  }

  __dllexport TaskScheduler::Thread* TaskScheduler::thread() {
    return thread_local_thread;
  }

  __dllexport TaskScheduler::Thread* TaskScheduler::swapThread(Thread* thread) 
  {
    Thread* old = thread_local_thread;
    thread_local_thread = thread;
    return old;
  }

  __dllexport bool TaskScheduler::wait() 
  {
    Thread* thread = TaskScheduler::thread();
    if (thread == nullptr) return true;
    while (thread->tasks.execute_local(*thread,thread->task)) {};
    return thread->scheduler->cancellingException == nullptr;
  }

  std::exception_ptr TaskScheduler::thread_loop(size_t threadIndex)
  {
    /* allocate thread structure */
    std::unique_ptr<Thread> mthread(new Thread(threadIndex,this)); // too large for stack allocation
    Thread& thread = *mthread;
    threadLocal[threadIndex].store(&thread);
    Thread* oldThread = swapThread(&thread);

    /* main thread loop */
    while (anyTasksRunning)
    {
      steal_loop(thread,
                 [&] () { return anyTasksRunning > 0; },
                 [&] () { 
                   anyTasksRunning++;
                   while (thread.tasks.execute_local(thread,nullptr));
                   anyTasksRunning--;
                 });
    }
    threadLocal[threadIndex].store(nullptr);
    swapThread(oldThread);

    /* remember exception to throw */
    std::exception_ptr except = nullptr;
    if (cancellingException != nullptr) except = cancellingException;

    /* wait for all threads to terminate */
    threadCounter--;
    while (threadCounter > 0) yield();
    return except;
  }

  bool TaskScheduler::steal_from_other_threads(Thread& thread)
  {
    const size_t threadIndex = thread.threadIndex;
    const size_t threadCount = this->threadCounter;

    for (size_t i=1; i<threadCount; i++) 
    {
      __pause_cpu(32);
      size_t otherThreadIndex = threadIndex+i;
      if (otherThreadIndex >= threadCount) otherThreadIndex -= threadCount;

      Thread* othread = threadLocal[otherThreadIndex].load();
      if (!othread)
        continue;

      if (othread->tasks.steal(thread)) 
        return true;      
    }

    return false;
  }

  __dllexport void TaskScheduler::startThreads() {
    threadPool->startThreads();
  }

  __dllexport void TaskScheduler::addScheduler(const Ref<TaskScheduler>& scheduler) {
    threadPool->add(scheduler);
  }

  __dllexport void TaskScheduler::removeScheduler(const Ref<TaskScheduler>& scheduler) {
    threadPool->remove(scheduler);
  }
}
