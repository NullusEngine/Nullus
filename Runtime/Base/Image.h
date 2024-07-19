#pragma once
#include <string>
#include "UDRefl/config.hpp"
namespace NLS
{
    class NLS_BASE_API Image {
    private:
        int width, height, channels;
        unsigned char* data;

    public:
        Image(const std::string& filename, bool flipVertically = false);
        Image(int width, int height, int channels); // 创建指定大小的空白图像
        Image(const Image& other, bool copyData); // 从另一个图像对象复制数据
        Image(const Image& other); // 拷贝构造函数
        Image(Image&& other) noexcept; // 移动构造函数
        Image& operator=(const Image& other); // 赋值运算符重载
        ~Image();

        void Load(const std::string& filename, bool flipVertically);
        void Free();
        void SetData(const unsigned char* newData); // 设置图像的像素数据
        int GetWidth() const;
        int GetHeight() const;
        int GetChannels() const;
        const unsigned char* GetData() const;
        unsigned char* GetData();
        void Save(const std::string& filename) const;
    };
}
