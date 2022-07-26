#ifndef _DPCPP_COMMON_H
#define _DPCPP_COMMON_H

#include <fmt/core.h>

#include <CL/sycl.hpp>
#include <argparse/argparse.hpp>
#include <optional>

#if __INTEL_LLVM_COMPILER < 20220000
#include <CL/sycl/INTEL/fpga_extensions.hpp>
#define SYCL_INTEL sycl::INTEL
#else
#include <sycl/ext/intel/fpga_extensions.hpp>
#define SYCL_INTEL sycl::ext::intel
#endif

constexpr auto R = "\033[31m";
constexpr auto G = "\033[32m";
constexpr auto Y = "\033[33m";
constexpr auto B = "\033[34m";
constexpr auto GRAY = "\033[37m";
constexpr auto BOLD = "\033[1m";
constexpr auto NC = "\033[0m";

sycl::queue initialize_queue() {
#ifdef FPGA
// Select either:
//  - the FPGA emulator device (CPU emulation of the FPGA)
//  - the FPGA device (a real FPGA)
#if defined(FPGA_EMULATOR)
    SYCL_INTEL::fpga_emulator_selector device_selector;
#else
    SYCL_INTEL::fpga_selector device_selector;
#endif
    sycl::queue Q(device_selector, sycl::property::queue::enable_profiling{});
#else
    sycl::queue Q{sycl::property::queue::enable_profiling{}};
#endif

    // Print information about the device we are using
    std::string device_kind = Q.get_device().is_cpu()           ? "CPU"
                              : Q.get_device().is_gpu()         ? "GPU"
                              : Q.get_device().is_accelerator() ? "FPGA"
                                                                : "Unknown";
    printf("Using %s%s%s Device: %s%s%s\n\n",
           BOLD,
           device_kind.c_str(),
           NC,
           BOLD,
           Q.get_device().get_info<sycl::info::device::name>().c_str(),
           NC);
    return Q;
}

std::string device_kind(const sycl::device &device) {
    return device.is_cpu()           ? "CPU"
           : device.is_gpu()         ? "GPU"
           : device.is_accelerator() ? "FPGA"
                                     : "Unknown";
}
/**
 * FPGA Selector that allows choosing a specific indexed FPGA.
 * 
 * A list is built of all Intel FPGA platform devices, and these are
 * sorted by name to make an ordered list. An index can be used to
 * reference a particular, consistently ordered FPGA.
 */
class fpga_index_selector : public sycl::device_selector {
    // The intel::fpga_selector uses this to discover devices
    static constexpr auto HARDWARE_PLATFORM_NAME = "Intel(R) FPGA SDK for OpenCL(TM)";
    static constexpr auto EMULATION_PLATFORM_NAME =
      "Intel(R) FPGA Emulation Platform for OpenCL(TM)";
    std::string indexed_device_name;

  public:
    static std::vector<std::string> get_device_list(void) {
        std::vector<std::string> devices;
        // Iterate over all platforms to find the FPGA ones
        for (auto const &platform : sycl::platform::get_platforms()) {
            if (platform.get_info<sycl::info::platform::name>()
                != HARDWARE_PLATFORM_NAME) {
                continue;
            }
            // We've found an FPGA platform. Get the name of all devices
            for (auto &device : platform.get_devices()) {
                auto device_name = device.get_info<sycl::info::device::name>();
                if (std::find(devices.begin(), devices.end(), device_name)
                    != devices.end()) {
                    // We've already enumerated this device. Skip.
                    continue;
                }
                devices.push_back(device_name);
            }
        }
        // Sort the list of all FPGAs in the system
        std::sort(devices.begin(), devices.end());
        return devices;
    }
    /**
     * @param selector The index of FPGA card to select.
     */
    fpga_index_selector(int selector = 0) {
        auto devices = get_device_list();

        if (selector + 1 > devices.size()) {
            throw std::runtime_error(
              fmt::format("Error: Asked for device ({}) that is higher than the number "
                          "of devices ({})",
                          selector + 1,
                          devices.size()));
        }
        indexed_device_name = devices[selector];
    }

    /// Used by SYCL to choose a device. Only scores devices that match the expected names.
    virtual int operator()(const sycl::device &device) const {
#if defined(FPGA_EMULATOR)
        // if we've specified FPGA emulator, then let's always choose that, regardless
        // of system-FPGA-platform-order.
        const sycl::platform &pf = device.get_platform();
        const std::string &platform_name = pf.get_info<sycl::info::platform::name>();
        if (platform_name == EMULATION_PLATFORM_NAME) {
            return 10000;
        }
#else
        if (device.get_info<sycl::info::device::name>() == indexed_device_name) {
            return 10000;
        }
#endif
        return -1;
    };
};

template <class T>
class FPGAArgumentParser;

struct FPGAArguments {
  public:
    auto device() -> sycl::device {
        if (!_device.has_value()) {
            _device = fpga_index_selector(this->device_index).select_device();
        }
        return _device.value();
    }

  private:
    std::optional<sycl::device> _device{};
    int device_index = 0;

    template <class T>
    friend class FPGAArgumentParser;
};

template <class ARGS = FPGAArguments>
class FPGAArgumentParser : public argparse::ArgumentParser {
    static_assert(std::is_base_of<ARGS, FPGAArguments>::value,
                  "Must be templated against a subclass of FPGAArgument");

  public:
    typedef ARGS ArgumentType;

    FPGAArgumentParser(std::string version = "0.1.0") : ArgumentParser("", version) {
        this->add_argument("-d", "--device")
          .help("Index of the FPGA device to target.")
          .default_value(0)
          .metavar("INDEX")
          .action([&](const std::string &value) {
              _arguments.device_index = std::stoi(value);
              return _arguments.device_index;
          });
        this->add_argument("--list-devices")
          .help("List the order of FPGA devices.")
          .implicit_value(false)
          .action([](const std::string &value) {
              auto devices = fpga_index_selector::get_device_list();
              int index = 0;
              fmt::print("System devices:\n");
              for (auto &device : devices) {
                  fmt::print("  {:2d}: {}{}{}\n", index, BOLD, device, NC);
                  ++index;
              }
              std::exit(0);
          });
    }
    /** Retrieve the selected device.
    *
    * Will fail if parsing has not yet happened.
    */
    auto device() -> sycl::device {
        if (!_device.has_value()) {
            _device = fpga_index_selector(this->get<int>("--device")).select_device();
        }
        return _device.value();
    }

    auto parse_args(int argc, char **argv) -> ARGS {
        ArgumentParser::parse_args(argc, argv);
        FPGAArguments &args = static_cast<FPGAArguments &>(_arguments);
        // Print information about the device we are using
        fmt::print("Using {}{}{} Device: {}{}{}\n\n",
                   BOLD,
                   device_kind(_arguments.device()),
                   NC,
                   BOLD,
                   args.device().get_info<sycl::info::device::name>(),
                   NC);

        return _arguments;
    }

  private:
    std::optional<sycl::device> _device{};
    ARGS _arguments{};
};
#endif