#pragma once
#include <opencv2/core.hpp>

namespace mosaicraft
{
// 管线互斥锁内使用，应用线程池负责并行，避免 OpenCV 再创建嵌套工作线程。
class ExecutionScope
{
public:
    ExecutionScope() : previous(cv::getNumThreads())
    {
        cv::setNumThreads(1);
    }
    ~ExecutionScope()
    {
        cv::setNumThreads(previous);
    }
private:
    int previous;
};
}
