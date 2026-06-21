/**
 * Phase 1 — 线程同步原语封装
 *
 * 这个文件封装了 pthread 的三种同步原语：
 * - sem:   信号量（控制资源访问数量）
 * - locker: 互斥锁（保证互斥访问临界区）
 * - cond:  条件变量（线程间的等待/通知机制）
 *
 * [C++ 语法]
 * - class: C++ 的类，默认成员是 private
 * - public/private: 访问控制。public 成员外部可访问，private 只能类内部访问
 * - 构造函数: 与类同名的函数，对象创建时自动调用。可以有参数。
 * - 析构函数: ~类名()，对象销毁时自动调用。做清理工作（释放锁等）。
 * - 初始化列表: 构造函数冒号后面的部分，直接初始化成员变量（高效）。
 * - throw: 抛出异常。C++ 用异常处理错误，而不是 C 的返回 -1。
 */
#ifndef LOCKER_H
#define LOCKER_H

#include <exception>   // std::exception
#include <pthread.h>   // pthread 多线程 API
#include <semaphore.h> // POSIX 信号量 API

/**
 * [设计模式] 封装（Encapsulation）
 * 把 C 的 pthread API 包装成 C++ 类，好处：
 * 1. 构造时自动初始化，析构时自动销毁 — RAII
 * 2. 语义清晰：sem.wait() 比 sem_wait(&s) 更直观
 * 3. 类型安全：编译器能检查类型匹配
 */

/**
 * 信号量（Semaphore）
 *
 * [理论]
 * 信号量是一个整数计数器，控制"同时最多 N 个线程访问资源"：
 * - sem(0): 初始值为 0 → wait() 会阻塞，直到有人 post()
 * - sem(N): 初始值为 N → 最多 N 个线程可以同时 wait() 通过
 *
 * 两个原子操作（不会被中断）：
 * - wait() / P 操作: 值减 1。如果值 < 0，阻塞。
 * - post() / V 操作: 值加 1。如果有线程在阻塞，唤醒一个。
 *
 * 典型用途：
 * - 用 0 初始化的信号量做"通知"（生产者通知消费者）
 * - 用 N 初始化的信号量做"资源池"（N 个数据库连接）
 */
class sem {
public:
    // [语法] 构造函数 — 对象创建时调用
    // sem() : 信号量初始值 0（用于通知语义）
    sem() {
        // sem_init 的第二个参数 0 表示"不在进程间共享"
        if (sem_init(&m_sem, 0, 0) != 0) {
            throw std::exception();  // [语法] 初始化失败抛异常
        }
    }

    // 带参数的构造函数 — 信号量初始值由调用者指定
    // [语法] 构造函数可以重载（overload）—— 同名的多个构造函数，参数不同
    sem(int num) {
        if (sem_init(&m_sem, 0, num) != 0) {
            throw std::exception();
        }
    }

    // [语法] 析构函数 — 对象销毁时调用
    ~sem() {
        sem_destroy(&m_sem);
    }

    // P 操作 — 等待（可能阻塞）
    bool wait() {
        return sem_wait(&m_sem) == 0;
    }

    // V 操作 — 释放（唤醒等待者）
    bool post() {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;  // [语法] POSIX 信号量类型
};

/**
 * 互斥锁（Mutex）
 *
 * [理论]
 * 互斥锁保证"同一时刻只有一个线程访问临界区"。
 *
 * lock() 和 unlock() 之间的代码就是"临界区"。
 * 临界区应该越短越好 — 锁持有时间越长，其他线程等得越久。
 *
 * [C++ 语法] 内联实现
 * 在类定义内部直接写函数体，编译器会尽量内联（inline）展开，
 * 减少函数调用开销。适用于短小的函数。
 */
class locker {
public:
    locker() {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }

    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    // 获取原始 pthread_mutex_t 指针
    // 供条件变量使用（cond::wait 需要传入 mutex 指针）
    pthread_mutex_t* get() {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

/**
 * 条件变量（Condition Variable）
 *
 * [理论]
 * 条件变量用于"等待某个条件成立"。
 * 它总是和互斥锁配合使用：
 *
 * 消费者线程：
 *   m_mutex.lock();
 *   while (queue.empty()) {    // 条件不满足
 *       m_cond.wait(m_mutex);  // 释放锁 + 阻塞，直到被唤醒
 *   }                           // 被唤醒后自动重新获取锁
 *   item = queue.pop();
 *   m_mutex.unlock();
 *
 * 生产者线程：
 *   m_mutex.lock();
 *   queue.push(item);
 *   m_cond.broadcast();  // 唤醒所有等待的消费者
 *   m_mutex.unlock();
 *
 * [关键理解] wait() 做了三件事（原子地）：
 * 1. 释放 mutex  ← 这样生产者才能拿到锁
 * 2. 阻塞自己
 * 3. 被唤醒后重新获取 mutex ← 这样消费者拿到锁后能安全访问队列
 *
 * [为什么用 while 而不是 if 检查条件？]
 * 条件变量可能被"虚假唤醒"（spurious wakeup），
 * 即 wait() 返回了但条件并不满足。用 while 确保被唤醒后重新检查条件。
 */
class cond {
public:
    cond() {
        if (pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }

    ~cond() {
        pthread_cond_destroy(&m_cond);
    }

    // 等待条件变量（无限等待）
    bool wait(pthread_mutex_t* m_mutex) {
        return pthread_cond_wait(&m_cond, m_mutex) == 0;
    }

    // 带超时的等待（毫秒）
    bool timewait(pthread_mutex_t* m_mutex, struct timespec t) {
        return pthread_cond_timedwait(&m_cond, m_mutex, &t) == 0;
    }

    // 唤醒一个等待线程
    bool signal() {
        return pthread_cond_signal(&m_cond) == 0;
    }

    // 唤醒所有等待线程
    bool broadcast() {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    pthread_cond_t m_cond;
};

#endif
