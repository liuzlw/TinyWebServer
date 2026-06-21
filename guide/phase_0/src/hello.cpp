/**
 * Phase 0 — Hello World
 *
 * 这个文件是 C++ 入门的第一个程序。
 *
 * C++ 语法知识点在本文件中标注：
 *
 * [语法] #include — 预处理指令，把其他文件的内容"粘贴"到这里
 *   - <iostream> 是 C++ 标准库的头文件，提供 std::cout（控制台输出）
 *   - 尖括号 <> 告诉编译器去系统路径找
 *   - 双引号 "" 告诉编译器先在当前目录找，再去系统路径找
 */
#include <iostream>   // [语法] 输入输出流
#include <string>     // [语法] std::string — C++ 的字符串类（比 C 的 char[] 安全）

/**
 * [语法] main 函数 — 程序的入口点
 *   - argc: 命令行参数个数（argument count）
 *   - argv: 命令行参数值（argument vector），char* 数组
 *   - 返回 int: 0 表示正常退出，非 0 表示异常
 *
 * [语法] int main() 和 int main(int argc, char* argv[]) 都是合法的
 */
int main(int argc, char* argv[]) {
    /**
     * [语法] std::string — C++ 字符串
     *   - 自动管理内存（不会像 char[] 那样溢出）
     *   - 支持 + 拼接、== 比较
     *   - 用 .c_str() 获取 C 风格字符串（const char*）
     */
    std::string name = "TinyWebServer";

    /**
     * [语法] std::cout — 标准输出流
     *   - << 是流插入运算符（把数据"推入"输出流）
     *   - std::endl 表示换行 + 刷新缓冲区
     *   - 也可以用 '\n' 换行（但不刷新缓冲区）
     */
    std::cout << "Hello, " << name << "!" << std::endl;

    /**
     * [语法] 条件编译 — #ifdef / #endif
     *   如果编译时定义了 DEBUG 宏，就输出这条额外信息
     *   学习 cmake 时会看到如何定义这个宏
     */
#ifdef DEBUG
    std::cout << "[DEBUG] Program compiled in debug mode." << std::endl;
#endif

    // 打印命令行参数
    std::cout << "Arguments: ";
    for (int i = 0; i < argc; ++i) {
        std::cout << "[" << i << "] " << argv[i] << "  ";
    }
    std::cout << std::endl;

    return 0;  // [语法] return 0 表示程序正常结束
}
