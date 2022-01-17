
#include <CL/sycl.hpp>
#include <CL/sycl/INTEL/fpga_extensions.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>

#include "common.hpp"
#include "eiger2xe.h"
#include "h5read.h"

using namespace sycl;

// From https://stackoverflow.com/a/10585543/1118662
constexpr bool is_power_of_two(int x) {
    return x && ((x & (x - 1)) == 0);
}

/// One-direction width of kernel. Total kernel span is (K_W * 2 + 1)
constexpr int KERNEL_WIDTH = 3;
/// One-direction height of kernel. Total kernel span is (K_H * 2 + 1)
constexpr int KERNEL_HEIGHT = 3;

constexpr int FULL_KERNEL_HEIGHT = KERNEL_HEIGHT * 2 + 1;

// Width of this array determines how many pixels we read at once
using PipedPixelsArray = std::array<H5Read::image_type, 16>;
// A convenience assignment for size of a single block
constexpr size_t BLOCK_SIZE = std::tuple_size<PipedPixelsArray>::value;
static_assert(is_power_of_two(BLOCK_SIZE));

// Convenience sum for PipedPixelsArray
auto operator+(const PipedPixelsArray& l, const PipedPixelsArray& r)
  -> PipedPixelsArray {
    PipedPixelsArray sum;
    for (int i = 0; i < BLOCK_SIZE; ++i) {
        sum[i] = l[i] + r[i];
    }
    return sum;
}
auto operator-(const PipedPixelsArray& l, const PipedPixelsArray& r)
  -> PipedPixelsArray {
    PipedPixelsArray sum;
    for (int i = 0; i < BLOCK_SIZE; ++i) {
        sum[i] = l[i] - r[i];
    }
    return sum;
}

// We need to buffer two blocks + kernel, because the pixels
// on the beginning of the block depend on the tail of the
// previous block, and the pixels at the end of the block
// depend on the start of the next block.
//
// Let's make a rolling buffer of:
//
//      | <KERNEL_WIDTH> | Block 0 | Block 1 |
//
// We read a block into block 1 - at which point we are
// ready to calculate all of the local-kernel sums for
// block 0 e.g.:
//
//      | K-2 | K-1 | k-0 | B0_0 | B0_1 | B0_2 | B0_3
//         └─────┴─────┴──────┼──────┴──────┴─────┘
//                            +
//                            │
//                         | S_0 | S_1 | S_2 | S_3 | ...
//
// Once we've calculated the per-pixel kernel sum for a
// single block, we can shift the entire array left by
// BLOCK_SIZE + KERNEL_WIDTH pixels to read the next
// block into the right of the buffer.
//
// Since we only need the raw pixel values of the
// buffer+block, this process can be pipelined.
using BufferedPipedPixelsArray =
  std::array<PipedPixelsArray::value_type, BLOCK_SIZE * 2 + KERNEL_WIDTH>;
// This two-block solution only works if kernel width < block size
static_assert(KERNEL_WIDTH < BLOCK_SIZE);

template <int blocks>
using ModuleRowStore =
  std::array<std::array<PipedPixelsArray, blocks>, FULL_KERNEL_HEIGHT>;

template <int id>
class ToModulePipe;

template <int id>
using ProducerPipeToModule = INTEL::pipe<class ToModulePipe<id>, PipedPixelsArray, 5>;

template <int Index>
class Module;

class Producer;

/// Return the profiling event time, in milliseconds, for an event
double event_ms(const sycl::event& e) {
    return 1e-6
           * (e.get_profiling_info<info::event_profiling::command_end>()
              - e.get_profiling_info<info::event_profiling::command_start>());
}

/// Calculate the GigaBytes per second given bytes, time
double GBps(size_t bytes, double ms) {
    return (static_cast<double>(bytes) / 1e9) / (ms / 1000.0);
}

/// Return value of event in terms of GigaBytes per second
double event_GBps(const sycl::event& e, size_t bytes) {
    const double ms = event_ms(e);
    return GBps(bytes, ms);
}

PipedPixelsArray sum_buffered_block_0(BufferedPipedPixelsArray& buffer) {
    // Now we can calculate the sums for block 0
    PipedPixelsArray sum{};
#pragma unroll
    for (int center = 0; center < BLOCK_SIZE; ++center) {
#pragma unroll
        for (int i = -KERNEL_WIDTH; i <= KERNEL_WIDTH; ++i) {
            sum[center] += buffer[KERNEL_WIDTH + center + i];
        }
    }
    return sum;
}

/// Draw a subset of the pixel values for a 2D image array
/// fast, slow, width, height - describe the bounding box to draw
/// data_width, data_height - describe the full data array size
void draw_image_data(const uint16_t* data,
                     size_t fast,
                     size_t slow,
                     size_t width,
                     size_t height,
                     size_t data_width,
                     size_t data_height) {
    for (int y = slow; y < slow + height; ++y) {
        if (y == slow) {
            printf("y = %2d │", y);
        } else {
            printf("    %2d │", y);
        }
        for (int i = fast; i < fast + width; ++i) {
            printf("%3d  ", data[i + data_width * y]);
        }
        printf("│\n");
    }
}

int main(int argc, char** argv) {
    auto start_time = std::chrono::high_resolution_clock::now();
    auto reader = H5Read(argc, argv);

    auto Q = initialize_queue();

    printf("Running with %s%zu-bit%s wide blocks\n", BOLD, BLOCK_SIZE * 16, NC);

    auto slow = reader.get_image_slow();
    auto fast = reader.get_image_fast();
    const size_t num_pixels = reader.get_image_slow() * reader.get_image_fast();

    // Mask data is the same for all images, so we copy it to device early
    auto mask_data = device_ptr<uint8_t>(malloc_device<uint8_t>(num_pixels, Q));
    // Declare the image data that will be remotely accessed
    auto image_data = host_ptr<uint16_t>(malloc_host<uint16_t>(num_pixels, Q));
    assert(image_data % 64 == 0);

    // Result data - this is accessed remotely but only at the end, to
    // return the results.
    // PipedPixelsArray* result = malloc_host<PipedPixelsArray>(4, Q);
    PipedPixelsArray* result = malloc_host<PipedPixelsArray>(2, Q);

    printf("Uploading mask data to accelerator.... ");
    auto e_mask_upload = Q.submit(
      [&](handler& h) { h.memcpy(mask_data, reader.get_mask().data(), num_pixels); });
    Q.wait();
    printf("done in %.1f ms (%.2f GBps)\n",
           event_ms(e_mask_upload),
           event_GBps(e_mask_upload, num_pixels));

    // Module/detector compile-time calculations
    /// The number of pixels left over when we divide the image into blocks of BLOCK_SIZE
    constexpr size_t BLOCK_REMAINDER = E2XE_16M_FAST % BLOCK_SIZE;
    // The number of full blocks that we can fit on an image
    constexpr size_t FULL_BLOCKS = (E2XE_16M_FAST - BLOCK_REMAINDER) / BLOCK_SIZE;

    printf(
      "Block data:\n"
      "         SIZE: %zu px per block\n"
      "    REMAINDER: %zu px unprocessed per row\n"
      "  FULL_BLOCKS: %zu blocks across image width\n",
      BLOCK_SIZE,
      BLOCK_REMAINDER,
      FULL_BLOCKS);

    std::array<uint16_t, FULL_BLOCKS>* totalblocksum =
      malloc_host<std::array<uint16_t, FULL_BLOCKS>>(slow, Q);

    uint16_t* destination_data = malloc_device<uint16_t>(num_pixels, Q);

    // auto* rows_ptr = malloc_device<ModuleRowStore<FULL_BLOCKS>>(1, Q);
    // auto  rows = malloc_device<
    //                 //                        FULL_KERNEL_HEIGHT>{};

    printf("Starting image loop:\n");
    for (int i = 0; i < reader.get_number_of_images(); ++i) {
        printf("\nReading Image %d\n", i);
        reader.get_image_into(i, image_data);

        // Precalculate host-side the answers we expect, so we can validate
        printf("Calculating host sum\n");
        // Now we are using blocks and discarding excess, do that here
        size_t host_sum = 0;
        for (int i = 0; i < FULL_BLOCKS * BLOCK_SIZE; ++i) {
            host_sum += image_data[i];
        }
        printf("Starting Kernels\n");
        auto t1 = std::chrono::high_resolution_clock::now();

        event e_producer = Q.submit([&](handler& h) {
            h.single_task<class Producer>([=]() {
                // For now, send every pixel into one pipe
                // We are using blocks based on the pipe width - this is
                // likely not an exact divisor of the fast width, so for
                // now just ignore the excess pixels
                for (size_t y = 0; y < slow; ++y) {
                    for (size_t block = 0; block < FULL_BLOCKS; ++block) {
                        auto image_data_h = host_ptr<PipedPixelsArray>(
                          reinterpret_cast<PipedPixelsArray*>(image_data.get()
                                                              + y * fast));
                        ProducerPipeToModule<0>::write(image_data_h[block]);
                    }
                }
            });
        });

        // Launch a module kernel for every module
        event e_module = Q.submit([&](handler& h) {
            h.single_task<class Module<0>>([=](){
                auto result_h = host_ptr<PipedPixelsArray>(result);
                auto destination_data_d = device_ptr<uint16_t>(destination_data);

                size_t sum_pixels = 0;

                // Make a buffer for full rows so we can store them as we go
                ModuleRowStore<FULL_BLOCKS> rows;
                // Initialise this to zeros
                for (int zr = 0; zr < FULL_KERNEL_HEIGHT; ++zr) {
                    for (int zb = 0; zb < FULL_BLOCKS; ++zb) {
#pragma unroll
                        for (int zp = 0; zp < BLOCK_SIZE; ++zp) {
                            rows[zr][zb][zp] = 0;
                        }
                    }
                }

                for (size_t y = 0; y < slow; ++y) {
                    // The per-pixel buffer array to accumulate the blocks
                    BufferedPipedPixelsArray interim_pixels{};

                    // Have a "block" view of this pixel buffer for easy access
                    auto* interim_blocks = reinterpret_cast<PipedPixelsArray*>(
                      &interim_pixels[KERNEL_WIDTH]);

                    // Read the first block into initial position in the array
                    interim_blocks[0] = ProducerPipeToModule<0>::read();

                    for (size_t block = 0; block < FULL_BLOCKS - 1; ++block) {
                        // Read this into the right of the array...
                        interim_blocks[1] = ProducerPipeToModule<0>::read();

                        // Now we can calculate the sums for block 0
                        PipedPixelsArray sum = sum_buffered_block_0(interim_pixels);

                        // Now shift everything in the row buffer to the left
                        // to make room for the next pipe read
#pragma unroll
                        for (int i = 0; i < KERNEL_WIDTH + BLOCK_SIZE; ++i) {
                            interim_pixels[i] = interim_pixels[BLOCK_SIZE + i];
                        }

                        // Now we can insert this into the row accumulation store and
                        // do per-row calculations

                        // Calculate the previously written row index, and get the row
                        int prev_row_store = (y - 1) % FULL_KERNEL_HEIGHT;
                        auto prev_row = rows[prev_row_store][block];
                        // And the oldest row index and row (which we will replace)
                        int swap_row_store = y % FULL_KERNEL_HEIGHT;
                        auto oldest_row = rows[swap_row_store][block];

                        // Write the new running total over the oldest data
                        PipedPixelsArray new_row = sum + prev_row;
                        rows[swap_row_store][block] = new_row;
                        // Now, calculate the kernel sum for each of these
                        auto kernel_sum = new_row - oldest_row;

                        // Write this into the output data block
                        if (y >= KERNEL_HEIGHT) {
                            *reinterpret_cast<PipedPixelsArray*>(
                              &destination_data_d[(y - KERNEL_HEIGHT) * fast
                                                  + block * BLOCK_SIZE]) = kernel_sum;
                        }
                    }
                }
                });
        });

        Q.wait();
        auto t2 = std::chrono::high_resolution_clock::now();
        double ms_all =
          std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1).count()
          * 1000;

        printf(" ... produced in %.2f ms (%.3g GBps)\n",
               event_ms(e_producer),
               event_GBps(e_producer, num_pixels * sizeof(uint16_t) / 2));
        printf(" ... consumed in %.2f ms (%.3g GBps)\n",
               event_ms(e_module),
               event_GBps(e_module, num_pixels * sizeof(uint16_t) / 2));
        printf(" ... Total consumed + piped in host time %.2f ms (%.3g GBps)\n",
               ms_all,
               GBps(num_pixels * sizeof(uint16_t), ms_all));

        // Copy the device destination buffer back
        // Print a section of the image and "destination" arrays
        auto host_sum_data = host_ptr<uint16_t>(malloc_host<uint16_t>(num_pixels, Q));
        auto e_dest_download = Q.submit([&](handler& h) {
            h.memcpy(host_sum_data, destination_data, num_pixels * sizeof(uint16_t));
        });
        e_dest_download.wait();
        Q.wait();

        printf("Data:\n");
        draw_image_data(image_data.get(), 0, 0, 16, 16, fast, slow);

        printf("\nSum:\n");
        draw_image_data(host_sum_data.get(), 0, 0, 16, 16, fast, slow);
        free(host_sum_data, Q);
    }

    free(result, Q);
    free(image_data, Q);
    free(mask_data, Q);
    auto end_time = std::chrono::high_resolution_clock::now();

    printf(
      "Total run duration: %.2f s\n",
      std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time)
        .count());
}
