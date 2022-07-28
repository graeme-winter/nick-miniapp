#include "kernel.hpp"

#include <sycl/ext/intel/fpga_extensions.hpp>

#include "common.hpp"

using namespace sycl;

class ToModulePipe;

using ProducerPipeToModule = SYCL_INTEL::pipe<class ToModulePipe, PipedPixelsArray, 5>;

template <int Index>
class Module;

class Producer;

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
  PipedPixelsArray::value_type[BLOCK_SIZE * 2 + KERNEL_WIDTH];
// This two-block solution only works if kernel width < block size
static_assert(KERNEL_WIDTH < BLOCK_SIZE);

const sycl::stream& operator<<(const sycl::stream& os,
                               const BufferedPipedPixelsArray& obj) {
    os << "[ ";
    for (int i = 0; i < KERNEL_WIDTH; ++i) {
        if (i != 0) os << ", ";
        os << setw(2) << obj[i];
    }
    os << " | ";
    for (int i = KERNEL_WIDTH; i < BLOCK_SIZE + KERNEL_WIDTH; ++i) {
        if (i != KERNEL_WIDTH) os << ", ";
        os << setw(2) << obj[i];
    }
    os << " | ";
    for (int i = KERNEL_WIDTH + BLOCK_SIZE; i < BLOCK_SIZE * 2 + KERNEL_WIDTH; ++i) {
        if (i != KERNEL_WIDTH + BLOCK_SIZE) os << ", ";
        os << setw(2) << obj[i];
    }
    os << " ]";
    return os;
}

PipedPixelsArray sum_buffered_block_0(BufferedPipedPixelsArray* buffer) {
    // Now we can calculate the sums for block 0
    PipedPixelsArray sum{};
#pragma unroll
    for (int center = 0; center < BLOCK_SIZE; ++center) {
#pragma unroll
        for (int i = -KERNEL_WIDTH; i <= KERNEL_WIDTH; ++i) {
            sum[center] += (*buffer)[KERNEL_WIDTH + center + i];
        }
    }
    return sum;
}

auto run_producer(sycl::queue& Q,
                  sycl::host_ptr<uint16_t> image_data,
                  std::size_t slow,
                  std::size_t fast) -> sycl::event {
    return Q.submit([&](handler& h) {
        h.single_task<class Producer>([=]() {
            // For now, send every pixel into one pipe
            // We are using blocks based on the pipe width - this is
            // likely not an exact divisor of the fast width, so for
            // now just ignore the excess pixels
            for (size_t y = 0; y < slow; ++y) {
                for (size_t block = 0; block < FULL_BLOCKS; ++block) {
                    auto image_data_h = host_ptr<PipedPixelsArray>(
                      reinterpret_cast<PipedPixelsArray*>(image_data.get() + y * fast));
                    ProducerPipeToModule::write(image_data_h[block]);
                }
            }
        });
    });
}

// #ifdef FPGA_EMULATOR
//             auto out = sycl::stream(10e6, 65535, h);
// #endif
auto run_module(sycl::queue& Q,
                device_ptr<uint8_t> mask_data,
                host_ptr<uint16_t> destination_data

                ) -> sycl::event {
    return Q.submit([&](handler& h) {
            h.single_task<class Module<0>>([=](){
                auto destination_data_h = host_ptr<uint16_t>(destination_data);

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

                for (size_t y = 0; y < SLOW; ++y) {
                    // The per-pixel buffer array to accumulate the blocks
                    BufferedPipedPixelsArray interim_pixels{};

                    // Have a "block" view of this pixel buffer for easy access
                    auto* interim_blocks = reinterpret_cast<PipedPixelsArray*>(
                      &interim_pixels[KERNEL_WIDTH]);

                    // Read the first block into initial position in the array
                    interim_blocks[0] = ProducerPipeToModule::read();

                    for (size_t block = 0; block < FULL_BLOCKS - 1; ++block) {
                        // Read this into the right of the array...
                        interim_blocks[1] = ProducerPipeToModule::read();

                        // Now we can calculate the sums for block 0
                        PipedPixelsArray sum = sum_buffered_block_0(&interim_pixels);

                    // Now shift everything in the row buffer to the left
                    // to make room for the next pipe read
#pragma unroll
                        for (int i = 0; i < KERNEL_WIDTH + BLOCK_SIZE; ++i) {
                            interim_pixels[i] = interim_pixels[BLOCK_SIZE + i];
                        }

                        // Now we can insert this into the row accumulation store and
                        // do per-row calculations

                        // Calculate the previously written row index, and get the row
                        int prev_row_index =
                          (y + FULL_KERNEL_HEIGHT - 1) % FULL_KERNEL_HEIGHT;
                        auto prev_row = rows[prev_row_index][block];
                        // And the oldest row index and row (which we will replace)
                        int oldest_row_index = y % FULL_KERNEL_HEIGHT;
                        auto oldest_row = rows[oldest_row_index][block];

                        // Write the new running total over the oldest data
                        PipedPixelsArray new_row = sum + prev_row;

                        rows[oldest_row_index][block] = new_row;

                        // Now, calculate the kernel sum for each of these
                        auto kernel_sum = new_row - oldest_row;

                        // Write this into the output data block
                        if (y >= KERNEL_HEIGHT) {
                            // Write a really simple loop.
                            size_t offset =
                              (y - KERNEL_HEIGHT) * FAST + block * BLOCK_SIZE;
#pragma unroll
                            for (size_t i = 0; i < BLOCK_SIZE; ++i) {
                                destination_data_h[offset + i] = kernel_sum[i];
                                // if (y == 5 && block == 0) {
                                //     result_dest[BLOCK_SIZE] =
                                //       (uintptr_t)destination_data_h.get();
                                //     result_dest[i] = (uintptr_t)(offset + i);
                                //     result_mini[i] = kernel_sum[i];
                                // }
                            }
                            // *reinterpret_cast<PipedPixelsArray*>(
                            //   &destination_data_h[(y - KERNEL_HEIGHT) * fast
                            //                       + block * BLOCK_SIZE]) = kernel_sum;
                        }
                    }
                }
                });
    });
}