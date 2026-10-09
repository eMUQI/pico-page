#pragma once
#include <cstddef>
#include <cstdint>
constexpr std::uint32_t MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_SPIRAM = 4;
std::size_t heap_caps_get_free_size(std::uint32_t caps);
std::size_t heap_caps_get_largest_free_block(std::uint32_t caps);
std::size_t heap_caps_get_minimum_free_size(std::uint32_t caps);
