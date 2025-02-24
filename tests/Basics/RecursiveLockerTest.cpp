////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2015, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "Basics/RecursiveLocker.h"
#include "Basics/ReadWriteLock.h"

#include "Basics/ThreadGuard.h"
#include "gtest/gtest.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace arangodb;
using namespace arangodb::basics;

// RecursiveWriteLocker

TEST(RecursiveLockerTest, testRecursiveWriteLockNoAcquire) {
  arangodb::basics::ReadWriteLock rwlock;
  std::atomic<std::thread::id> owner;

  RECURSIVE_WRITE_LOCKER_NAMED(locker, rwlock, owner, false);
  ASSERT_FALSE(locker.isLocked());

  locker.lock();
  ASSERT_TRUE(locker.isLocked());

  locker.unlock();
  ASSERT_FALSE(locker.isLocked());
}

TEST(RecursiveLockerTest, testRecursiveWriteLockAcquire) {
  arangodb::basics::ReadWriteLock rwlock;
  std::atomic<std::thread::id> owner;

  RECURSIVE_WRITE_LOCKER_NAMED(locker, rwlock, owner, true);
  ASSERT_TRUE(locker.isLocked());

  locker.unlock();
  ASSERT_FALSE(locker.isLocked());
}

TEST(RecursiveLockerTest, testRecursiveWriteLockUnlock) {
  arangodb::basics::ReadWriteLock rwlock;
  std::atomic<std::thread::id> owner;

  RECURSIVE_WRITE_LOCKER_NAMED(locker, rwlock, owner, true);
  ASSERT_TRUE(locker.isLocked());

  for (int i = 0; i < 100; ++i) {
    locker.unlock();
    ASSERT_FALSE(locker.isLocked());
    locker.lock();
    ASSERT_TRUE(locker.isLocked());
  }

  ASSERT_TRUE(locker.isLocked());
  locker.unlock();
  ASSERT_FALSE(locker.isLocked());
}

TEST(RecursiveLockerTest, testRecursiveWriteLockNested) {
  arangodb::basics::ReadWriteLock rwlock;
  std::atomic<std::thread::id> owner;

  RECURSIVE_WRITE_LOCKER_NAMED(locker1, rwlock, owner, true);
  ASSERT_TRUE(locker1.isLocked());

  {
    RECURSIVE_WRITE_LOCKER_NAMED(locker2, rwlock, owner, true);
    ASSERT_TRUE(locker2.isLocked());

    {
      RECURSIVE_WRITE_LOCKER_NAMED(locker3, rwlock, owner, true);
      ASSERT_TRUE(locker3.isLocked());
    }

    ASSERT_TRUE(locker2.isLocked());
  }

  ASSERT_TRUE(locker1.isLocked());

  locker1.unlock();
  ASSERT_FALSE(locker1.isLocked());
}

TEST(RecursiveLockerTest, testRecursiveWriteLockMultiThreaded) {
  arangodb::basics::ReadWriteLock rwlock;
  std::atomic<std::thread::id> owner;

  // number of threads started
  std::atomic<int> started{0};

  // shared variables, only protected by rw-locks
  uint64_t total = 0;
  uint64_t x = 0;

  constexpr int n = 4;
  constexpr int iterations = 100000;

  auto threads = ThreadGuard(n);

  for (int i = 0; i < n; ++i) {
    threads.emplace([&]() {
      ++started;
      while (started < n) { /*spin*/
      }

      for (int i = 0; i < iterations; ++i) {
        RECURSIVE_WRITE_LOCKER_NAMED(locker1, rwlock, owner, true);
        ASSERT_TRUE(locker1.isLocked());

        total++;
        x++;

        {
          RECURSIVE_WRITE_LOCKER_NAMED(locker2, rwlock, owner, true);
          ASSERT_TRUE(locker2.isLocked());

          x++;
        }
      }
    });
  }

  threads.joinAll();

  ASSERT_EQ(n * iterations, total);
  ASSERT_EQ(n * iterations * 2, x);
}

TEST(RecursiveLockerTest, testRecursiveWriteWithNestedRead) {
  arangodb::basics::ReadWriteLock rwlock;
  std::atomic<std::thread::id> owner;

  RECURSIVE_WRITE_LOCKER_NAMED(locker, rwlock, owner, true);
  ASSERT_TRUE(locker.isLocked());

  {
    // should not block
    RECURSIVE_READ_LOCKER(rwlock, owner);
  }

  locker.unlock();
  ASSERT_FALSE(locker.isLocked());
}

TEST(RecursiveLockerTest, testRecursiveWriteLockMultiThreadedWriteRead) {
  arangodb::basics::ReadWriteLock rwlock;
  std::atomic<std::thread::id> owner;

  // number of threads started
  std::atomic<int> started{0};

  // shared variables, only protected by rw-locks
  uint64_t total = 0;
  uint64_t x = 0;

  constexpr int n = 4;
  constexpr int iterations = 100000;

  auto threads = ThreadGuard(n);

  for (int i = 0; i < n; ++i) {
    threads.emplace([&]() {
      ++started;
      while (started < n) { /*spin*/
      }

      for (int i = 0; i < iterations; ++i) {
        RECURSIVE_WRITE_LOCKER_NAMED(locker, rwlock, owner, true);
        ASSERT_TRUE(locker.isLocked());

        total++;
        x++;

        {
          RECURSIVE_READ_LOCKER(rwlock, owner);
          ASSERT_EQ(x, total);
        }

        ASSERT_EQ(x, total);
      }
    });
  }

  threads.joinAll();

  ASSERT_EQ(n * iterations, total);
  ASSERT_EQ(n * iterations, x);
}

TEST(RecursiveLockerTest, testRecursiveWriteLockMultiThreadedWriteAndReadMix) {
  arangodb::basics::ReadWriteLock rwlock;
  std::atomic<std::thread::id> owner;

  // number of threads started
  std::atomic<int> started{0};

  // shared variables, only protected by rw-locks
  uint64_t total = 0;
  uint64_t x = 0;

  constexpr int n = 4;
  constexpr int iterations = 100000;

  auto threads = ThreadGuard(n);

  for (int i = 0; i < n; ++i) {
    threads.emplace(
        [&](int id) {
          ++started;
          while (started < n) { /*spin*/
          }

          if (id % 2 == 0) {
            // read threads
            for (int i = 0; i < iterations; ++i) {
              RECURSIVE_READ_LOCKER(rwlock, owner);
              ASSERT_EQ(x, total);
            }
          } else {
            // write threads
            for (int i = 0; i < iterations; ++i) {
              RECURSIVE_WRITE_LOCKER_NAMED(locker, rwlock, owner, true);
              ASSERT_TRUE(locker.isLocked());

              total++;
              x++;
              ASSERT_EQ(x, total);
            }
          }
        },
        i);
  }

  threads.joinAll();

  ASSERT_EQ((n / 2) * iterations, total);
  ASSERT_EQ((n / 2) * iterations, x);
}

TEST(RecursiveLockerTest, testRecursiveReadLockMultiThreadedWriteAndReadMix) {
  arangodb::basics::ReadWriteLock rwlock;
  std::atomic<std::thread::id> owner;

  // number of threads started
  std::atomic<int> started{0};

  // shared variables, only protected by rw-locks
  uint64_t total = 0;
  uint64_t x = 0;

  constexpr int n = 4;
  constexpr int iterations = 100000;

  auto threads = ThreadGuard(n);

  for (int i = 0; i < n; ++i) {
    threads.emplace(
        [&](int id) {
          ++started;
          while (started < n) { /*spin*/
          }

          if (id != 0) {
            // non-modifying threads
            for (int i = 0; i < iterations; ++i) {
              RECURSIVE_WRITE_LOCKER(rwlock, owner);
              ASSERT_EQ(x, total);

              // add a few nested lockers here, just to see if we get into
              // issues
              {
                RECURSIVE_READ_LOCKER(rwlock, owner);
                ASSERT_EQ(x, total);

                {
                  RECURSIVE_READ_LOCKER(rwlock, owner);
                  ASSERT_EQ(x, total);
                }
              }
            }
          } else {
            // write thread
            for (int i = 0; i < iterations; ++i) {
              RECURSIVE_WRITE_LOCKER_NAMED(locker, rwlock, owner, true);
              ASSERT_TRUE(locker.isLocked());

              total++;
              x++;
              ASSERT_EQ(x, total);

              // add a few nested lockers here, just to see if we get into
              // issues
              {
                RECURSIVE_WRITE_LOCKER(rwlock, owner);
                ASSERT_EQ(x, total);

                {
                  RECURSIVE_WRITE_LOCKER(rwlock, owner);
                  ASSERT_EQ(x, total);
                }
              }
            }
          }
        },
        i);
  }

  threads.joinAll();

  ASSERT_EQ(iterations, total);
  ASSERT_EQ(iterations, x);
}
