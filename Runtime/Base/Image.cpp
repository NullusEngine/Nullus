// image.cpp
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>
#include "Image.h"
#include "Detail/SvgRasterizer.h"
#include <algorithm>
#include <iostream>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>
namespace NLS
{
namespace
{
bool ReadFileBytes(const std::string& filename, std::vector<uint8_t>& bytes)
{
    std::ifstream input(filename, std::ios::binary);
    if (!input)
        return false;

    input.seekg(0, std::ios::end);
    const auto fileSize = input.tellg();
    if (fileSize <= 0)
        return false;

    if (static_cast<uint64_t>(fileSize) > static_cast<uint64_t>(std::numeric_limits<int>::max()))
        return false;

    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(fileSize));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return input.good() || input.eof();
}

bool DecodeImage(const uint8_t* encodedData, size_t encodedSize, bool flipVertically,
    int& width, int& height, int& channels, std::vector<unsigned char>& data)
{
    stbi_set_flip_vertically_on_load_thread(flipVertically);
    int decodedWidth = 0;
    int decodedHeight = 0;
    int decodedChannels = 0;
    if (auto* loadedData = stbi_load_from_memory(encodedData, static_cast<int>(encodedSize),
        &decodedWidth, &decodedHeight, &decodedChannels, 0))
    {
        const size_t dataSize = static_cast<size_t>(decodedWidth) * decodedHeight * decodedChannels;
        data.assign(loadedData, loadedData + dataSize);
        stbi_image_free(loadedData);
        width = decodedWidth;
        height = decodedHeight;
        channels = decodedChannels;
        return true;
    }

    Detail::SvgPixels svg;
    if (!Detail::RasterizeSvg(encodedData, encodedSize, svg))
        return false;

    width = svg.width;
    height = svg.height;
    channels = 4;
    data = std::move(svg.rgba);
    if (flipVertically)
    {
        const size_t stride = static_cast<size_t>(width) * 4u;
        for (int y = 0; y < height / 2; ++y)
        {
            auto top = data.begin() + static_cast<size_t>(y) * stride;
            auto bottom = data.begin() + static_cast<size_t>(height - 1 - y) * stride;
            std::swap_ranges(top, top + stride, bottom);
        }
    }
    return true;
}
}

Image::Image(const std::string& filename, bool flipVertically)
    : width(0)
    , height(0)
    , channels(0)
{
    Load(filename, flipVertically);
}

Image::Image(const uint8_t* encodedData, size_t encodedDataSize, bool flipVertically)
    : width(0)
    , height(0)
    , channels(0)
{
    if (encodedData == nullptr || encodedDataSize == 0u || encodedDataSize > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        return;
    }

    if (!DecodeImage(encodedData, encodedDataSize, flipVertically, width, height, channels, data))
    {
        std::cerr << "Failed to load image from memory." << std::endl;
        return;
    }

}
// 创建指定大小的空白图像
Image::Image(int width, int height, int channels)
    : width(width)
    , height(height)
    , channels(channels)
    , data(static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels), 0u)
{
}

// 从另一个图像对象复制数据
Image::Image(const Image& other, bool copyData)
    : width(other.width)
    , height(other.height)
    , channels(other.channels)
    , data(copyData ? other.data : std::vector<unsigned char> {})
{
}
// 拷贝构造函数的实现
Image::Image(const Image& other)
    : width(other.width), height(other.height), channels(other.channels)
{
    data = other.data;
}
// 移动构造函数的实现
Image::Image(Image&& other) noexcept
    : width(other.width)
    , height(other.height)
    , channels(other.channels)
    , data(std::move(other.data))
{
    other.width = other.height = other.channels = 0;
    other.data.clear();
}

// 赋值运算符重载的实现
Image& Image::operator=(const Image& other)
{
    if (this != &other)
    {
        width = other.width;
        height = other.height;
        channels = other.channels;
        data = other.data;
    }
    return *this;
}

Image::~Image()
{
    Free();
}

void Image::Load(const std::string& filename, bool flipVertically)
{
    Free();

    std::vector<uint8_t> encodedBytes;
    if (!ReadFileBytes(filename, encodedBytes))
    {
        std::cerr << "Failed to read image file: " << filename << std::endl;
        width = height = channels = 0;
        return;
    }

    if (!DecodeImage(encodedBytes.data(), encodedBytes.size(), flipVertically, width, height, channels, data))
    {
        std::cerr << "Failed to load image: " << filename << std::endl;
        width = height = channels = 0;
        return;
    }

}


void Image::Free()
{
    data.clear();
    width = height = channels = 0;
}

int Image::GetWidth() const
{
    return width;
}
int Image::GetHeight() const
{
    return height;
}
int Image::GetChannels() const
{
    return channels;
}
const unsigned char* Image::GetData() const
{
    return data.empty() ? nullptr : data.data();
}
void Image::SetData(const unsigned char* newData) {
    if (!newData) {
        std::cerr << "Invalid image data provided." << std::endl;
        return;
    }
    const size_t dataSize = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels);
    const unsigned char* currentData = data.empty() ? nullptr : data.data();
    const bool overlapsCurrentData =
        currentData != nullptr &&
        newData >= currentData &&
        newData < currentData + data.size();
    if (overlapsCurrentData)
    {
        std::vector<unsigned char> copied(newData, newData + dataSize);
        data = std::move(copied);
        return;
    }

    data.assign(newData, newData + dataSize);
}
unsigned char* Image::GetData() 
{
    return data.empty() ? nullptr : data.data();
}
void Image::Save(const std::string& filename) const
{
    if (!data.empty())
    {
        std::string format = filename.substr(filename.find_last_of(".") + 1);
        if (format == "png" || format == "jpg" || format == "bmp")
        {
            stbi_write_png(filename.c_str(), width, height, channels, data.data(), width * channels);
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
