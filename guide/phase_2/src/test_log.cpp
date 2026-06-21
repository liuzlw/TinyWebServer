/**
 * Phase 2 — 日志系统测试
 *
 * 测试同步/异步日志写入、按行数切分文件
 */
#include "log.h"

// 日志系统的 close_log 标志：0=开启日志
int m_close_log = 0;

int main() {
    // init(文件名, close_log, 缓冲区大小, 切分行数, 队列长度)
    // max_queue_size = 0  → 同步日志（直接写文件）
    // max_queue_size >= 1 → 异步日志（推入阻塞队列）
    Log::get_instance()->init("./TestLog", m_close_log,
                              8192, 100, 10);  // 每 100 行切文件，队列长度 10

    for (int i = 0; i < 500; ++i) {
        LOG_INFO("This is log line %d — testing log system phase 2", i);
    }

    return 0;
}
