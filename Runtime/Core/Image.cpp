// image.cpp
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>
#include "Core/Image.h"
#include <iostream>
#include <cstring>
namespace NLS
{
Image::Image(const std::string& filename)
    : data(nullptr)
{
    data = stbi_load(filename.c_str(), &width, &height, &channels, 0);
    if (!data)
    {
        std::cerr << "Failed to load image: " << filename << std::endl;
        width = height = channels = 0;
    }
}
// 创建指定大小的空白图像
Image::Image(int width, int height, int channels) : width(width), height(height), channels(channels), data(nullptr) {
    size_t dataSize = width * height * channels;
    data = new unsigned char[dataSize]();
}

// 从另一个图像对象复制数据
Image::Image(const Image& other, bool copyData) : width(other.width), height(other.height), channels(other.channels), data(nullptr) {
    if (copyData) {
        size_t dataSize = width * height * channels;
        data = new unsigned char[dataSize];
        std::memcpy(data, other.data, dataSize);
    }
}
// 拷贝构造函数的实现
Image::Image(const Image& other)
    : width(other.width), height(other.height), channels(other.channels)
{
    size_t dataSize = width * height * channels;
    data = new unsigned char[dataSize];
    std::memcpy(data, other.data, dataSize);
}
// 移动构造函数的实现
Image::Image(Image&& other) noexcept
    : width(other.width), height(other.height), channels(other.channels), data(other.data)
{
    other.width = other.height = other.channels = 0;
    other.data = nullptr;
}
// 赋值运算符重载的实现
Image& Image::operator=(const Image& other)
{
    if (this != &other)
    { // 避免自我赋值
        width = other.width;
        height = other.height;
        channels = other.channels;

        delete[] data; // 删除当前对象的原始数据

        size_t dataSize = width * height * channels;
        data = new unsigned char[dataSize];
        std::memcpy(data, other.data, dataSize);
    }
    return *this;
}

Image::~Image()
{
    if (data)
        stbi_image_free(data);
}

int Image::getWidth() const
{
    return width;
}
int Image::getHeight() const
{
    return height;
}
int Image::getChannels() const
{
    return channels;
}
const unsigned char* Image::getData() const
{
    return data;
}
void Image::setData(const unsigned char* newData) {
    if (!newData) {
        std::cerr << "Invalid image data provided." << std::endl;
        return;
    }
    if (data)
    {
        stbi_image_free(data);
    }
    size_t dataSize = width * height * channels;
    data = new unsigned char[dataSize];
    std::memcpy(data, newData, dataSize);
}
unsigned char* Image::getData() 
{
    return data;
}
void Image::save(const std::string& filename) const
{
    if (data)
    {
        std::string format = filename.substr(filename.find_last_of(".") + 1);
        if (format == "png" || format == "jpg" || format == "bmp")
        {
            stbi_write_png(filename.c_str(), width, height, channels, data, width * channels);
            std::cout << "Image saved as " << filename << std::endl;
        }
        else
        {
            std::cerr << "Unsupported image format for saving: " << format << std::endl;
        }
    }
    else
    {
        std::cerr << "No image data to save." << std::endl;
    }
}
} // namespace NLS
