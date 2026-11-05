#include <Detail/SvgRasterizer.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace
{
void Write16(std::ostream& output, uint16_t value)
{
    output.put(static_cast<char>(value & 0xffu));
    output.put(static_cast<char>((value >> 8u) & 0xffu));
}

void Write32(std::ostream& output, uint32_t value)
{
    Write16(output, static_cast<uint16_t>(value));
    Write16(output, static_cast<uint16_t>(value >> 16u));
}
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: NullusBrandIconBaker <logo.svg> <output.ico>\n";
        return 1;
    }

    try
    {
        std::ifstream input(std::filesystem::u8path(argv[1]), std::ios::binary);
        if (!input)
            throw std::runtime_error("Cannot read SVG logo");
        const std::vector<uint8_t> source((std::istreambuf_iterator<char>(input)), {});
        constexpr std::array<int, 7> sizes { 16, 24, 32, 48, 64, 128, 256 };
        std::vector<std::vector<uint8_t>> images;
        for (const int size : sizes)
        {
            NLS::Detail::SvgPixels pixels;
            if (!NLS::Detail::RasterizeSvg(source.data(), source.size(), pixels, size) ||
                pixels.width != size || pixels.height != size)
                throw std::runtime_error("Application icon must be a valid square SVG");

            int pngSize = 0;
            auto* png = stbi_write_png_to_mem(pixels.rgba.data(), size * 4, size, size, 4, &pngSize);
            if (png == nullptr)
                throw std::runtime_error("Cannot encode application icon");
            images.emplace_back(png, png + pngSize);
            STBIW_FREE(png);
        }

        const auto destination = std::filesystem::u8path(argv[2]);
        if (!destination.parent_path().empty())
            std::filesystem::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Cannot write application icon");
        Write16(output, 0);
        Write16(output, 1);
        Write16(output, static_cast<uint16_t>(sizes.size()));
        uint32_t offset = 6u + 16u * static_cast<uint32_t>(sizes.size());
        for (size_t i = 0; i < sizes.size(); ++i)
        {
            output.put(static_cast<char>(sizes[i] == 256 ? 0 : sizes[i]));
            output.put(static_cast<char>(sizes[i] == 256 ? 0 : sizes[i]));
            output.put(0);
            output.put(0);
            Write16(output, 1);
            Write16(output, 32);
            Write32(output, static_cast<uint32_t>(images[i].size()));
            Write32(output, offset);
            offset += static_cast<uint32_t>(images[i].size());
        }
        for (const auto& png : images)
            output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!output)
            throw std::runtime_error("Incomplete application icon write");
        std::cout << "Generated application icon from " << argv[1] << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
