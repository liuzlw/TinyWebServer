/**
 * Phase 1 — 生产者-消费者模型测试
 *
 * 这个程序验证 lock/locker.h 的正确性。
 * 多个生产者生产数据 → 多个消费者消费数据，全程无竞态。
 *
 * [C++ 语法要点]
 * - pthread_create: 创建线程。参数：线程ID指针、属性、函数、函数参数
 * - pthread_join: 等待线程结束（类似 waitpid）
 * - void* 作为通用指针：C 风格的多态。C++ 中更推荐用模板或 std::function。
 * - static_cast<T*>: C++ 的安全类型转换（比 C 的 (T*) 安全）
 */
#include <iostream>
#include <pthread.h>
#include <unistd.h>
#include <vector>
#include "locker.h"

// === 全局共享资源 ===
#define MAX_ITEMS 100    // 缓冲区最大容量
#define PRODUCER_COUNT 2 // 生产者数量
#define CONSUMER_COUNT 3 // 消费者数量
#define ITEMS_PER_PRODUCER 50 // 每个生产者生产多少个

int buffer[MAX_ITEMS];   // 共享缓冲区（循环数组）
int buffer_count = 0;    // 当前缓冲区中的元素数量
int produced_total = 0;  // 总共生产了（用于验证）
int consumed_total = 0;  // 总共消费了（用于验证）

// 同步原语
locker m_mutex;  // 保护 buffer_count、buffer
cond m_cond;     // 条件变量：缓冲区非空时通知消费者，缓冲区非满时通知生产者

/**
 * 生产者线程函数
 *
 * [理论] 生产者-消费者的同步逻辑：
 * 生产者：缓冲区满了 → 等待（wait）→ 被消费者唤醒 → 继续生产
 *
 * [C++ 语法]
 * void* 是 C 风格的"万能指针"。在 pthread_create 中，
 * 线程函数的参数和返回值都用 void* 表示任意类型。
 * 函数内部用 static_cast 转回真正的类型。
 */
void* producer(void* arg) {
    int id = *static_cast<int*>(arg);  // [语法] 把 void* 转回 int*

    for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
        m_mutex.lock();

        // 缓冲区满 → 等待消费者腾出空间
        // [语法] while 而不是 if！被唤醒后重新检查条件
        while (buffer_count >= MAX_ITEMS) {
            m_cond.wait(m_mutex.get());
        }

        // 生产一个数据
        int item = id * 1000 + i;  // 用 id 编码，方便追踪
        buffer[buffer_count] = item;
        buffer_count++;
        produced_total++;

        std::cout << "[Producer " << id << "] produced item " << item
                  << " (buffer: " << buffer_count << "/" << MAX_ITEMS << ")"
                  << std::endl;

        // 通知消费者：有新数据了
        m_cond.broadcast();
        m_mutex.unlock();

        // 模拟生产耗时（让线程切换更明显）
        usleep(10000);  // 10ms
    }
    return NULL;
}

/**
 * 消费者线程函数
 */
void* consumer(void* arg) {
    int id = *static_cast<int*>(arg);

    while (true) {
        m_mutex.lock();

        // 缓冲区空 → 等待生产者生产
        while (buffer_count <= 0) {
            // 检查是否所有生产都完成了
            // 注意：这个检查在锁内进行，安全
            if (produced_total >= PRODUCER_COUNT * ITEMS_PER_PRODUCER) {
                m_mutex.unlock();
                std::cout << "[Consumer " << id << "] exiting (all produced items consumed)"
                          << std::endl;
                return NULL;
            }
            m_cond.wait(m_mutex.get());
        }

        // 消费一个数据
        int item = buffer[--buffer_count];
        consumed_total++;

        std::cout << "[Consumer " << id << "] consumed item " << item
                  << " (buffer: " << buffer_count << "/" << MAX_ITEMS << ")"
                  << std::endl;

        // 通知生产者：有空位了
        m_cond.broadcast();
        m_mutex.unlock();

        usleep(15000);  // 15ms（消费比生产稍慢，更容易触发"缓冲区满"）
    }
    return NULL;
}

int main() {
    std::cout << "=== Producer-Consumer Test ===" << std::endl;
    std::cout << "Producers: " << PRODUCER_COUNT << ", each produces "
              << ITEMS_PER_PRODUCER << " items" << std::endl;
    std::cout << "Consumers: " << CONSUMER_COUNT << std::endl;
    std::cout << "Buffer size: " << MAX_ITEMS << std::endl;
    std::cout << std::endl;

    // 创建线程
    pthread_t producers[PRODUCER_COUNT];
    pthread_t consumers[CONSUMER_COUNT];
    int producer_ids[PRODUCER_COUNT];
    int consumer_ids[CONSUMER_COUNT];

    for (int i = 0; i < PRODUCER_COUNT; ++i) {
        producer_ids[i] = i + 1;
        pthread_create(&producers[i], NULL, producer, &producer_ids[i]);
    }

    for (int i = 0; i < CONSUMER_COUNT; ++i) {
        consumer_ids[i] = i + 1;
        pthread_create(&consumers[i], NULL, consumer, &consumer_ids[i]);
    }

    // 等待所有线程完成
    for (int i = 0; i < PRODUCER_COUNT; ++i) {
        pthread_join(producers[i], NULL);
    }

    for (int i = 0; i < CONSUMER_COUNT; ++i) {
        pthread_join(consumers[i], NULL);
    }

    // === 验证 ===
    std::cout << std::endl << "=== Results ===" << std::endl;
    std::cout << "Total produced:  " << produced_total << std::endl;
    std::cout << "Total consumed:  " << consumed_total << std::endl;
    std::cout << "Buffer remaining: " << buffer_count << std::endl;

    if (produced_total == consumed_total && buffer_count == 0) {
        std::cout << "✅ PASS: All items consumed exactly once!" << std::endl;
        return 0;
    } else {
        std::cout << "❌ FAIL: Production/consumption mismatch!" << std::endl;
        return 1;
    }
}
