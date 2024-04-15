#pragma once
#include <string>
#include "CoreDef.h"
namespace NLS
{
    class NLS_CORE_API Image {
    private:
        int width, height, channels;
        unsigned char* data;

    public:
        Image(const std::string& filename);
        Image(int width, int height, int channels); // 创建指定大小的空白图像
        Image(const Image& other, bool copyData); // 从另一个图像对象复制数据
        Image(const Image& other); // 拷贝构造函数
        Image(Image&& other) noexcept; // 移动构造函数
        Image& operator=(const Image& other); // 赋值运算符重载
        ~Image();

        void setData(const unsigned char* newData); // 设置图像的像素数据
        int getWidth() const;
        int getHeight() const;
        int getChannels() const;
        const unsigned char* getData() const;
        unsigned char* getData();
        void save(const std::string& filename) const;
    };
}
