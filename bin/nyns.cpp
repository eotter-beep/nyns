// C++ port of bin/nyns.sh, with minimal external dependencies

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>

static bool remove_recursive(const std::string &path, bool force = false) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT && force) {
            return true;
        }
        std::perror(("Error stating '" + path + "'").c_str());
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path.c_str());
        if (!dir) {
            std::perror(("Error opening directory '" + path + "'").c_str());
            return false;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr) {
            const char *name = entry->d_name;
            if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
                continue;
            }
            std::string child = path;
            if (!child.empty() && child.back() != '/') {
                child += '/';
            }
            child += name;
            remove_recursive(child, force);
        }
        closedir(dir);

        if (rmdir(path.c_str()) != 0) {
            if (!force) {
                std::perror(("Error removing directory '" + path + "'").c_str());
            }
            return force;
        }
        return true;
    }

    if (std::remove(path.c_str()) != 0) {
        if (!force) {
            std::perror(("Error removing file '" + path + "'").c_str());
        }
        return force;
    }
    return true;
}

static bool mkdir_p(const std::string &path) {
    if (path.empty() || path == ".") {
        return true;
    }
    if (path == "/") {
        return true;
    }

    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    std::size_t pos = path.rfind('/');
    if (pos != std::string::npos && pos > 0) {
        std::string parent = path.substr(0, pos);
        if (!mkdir_p(parent)) {
            return false;
        }
    }

    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        std::perror(("Error creating directory '" + path + "'").c_str());
        return false;
    }
    return true;
}

static bool is_block_device(const std::string &path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISBLK(st.st_mode);
}

struct PartitionEntry {
    std::uint8_t boot_indicator;
    std::uint8_t start_chs[3];
    std::uint8_t partition_type;
    std::uint8_t end_chs[3];
    std::uint32_t start_lba;
    std::uint32_t size_sectors;
} __attribute__((packed));

static constexpr std::size_t MBR_PART_TABLE_OFFSET = 446;
static constexpr std::size_t MBR_MAX_PARTITIONS = 4;

// Simple in-memory representation of a TUI menu consisting of buttons
// and a display text area.
static std::vector<std::string> g_buttons;
static int g_selected_button = -1;
static std::string g_display_text;

static void draw_tui_menu() {
    // Clear screen and move cursor to top-left for a full-screen effect
    std::cout << "\033[2J\033[H";

    std::cout << "==== DISPLAY ====\n";
    if (!g_display_text.empty()) {
        std::cout << g_display_text << '\n';
    } else {
        std::cout << "(no display text)\n";
    }
    std::cout << "=================\n\n";

    std::cout << "==== MENU ====\n";
    if (g_buttons.empty()) {
        std::cout << "(no buttons)\n";
    } else {
        for (std::size_t i = 0; i < g_buttons.size(); ++i) {
            bool selected = (static_cast<int>(i) == g_selected_button);
            const char *marker = selected ? "> " : "  ";
            std::cout << marker << (i + 1) << ") [" << g_buttons[i] << "]\n";
        }
    }
    std::cout << "==============\n";
}

static void print_ip_addresses() {
    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        std::perror("Error getting network interfaces");
        return;
    }

    for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }
        int family = ifa->ifa_addr->sa_family;
        char host[NI_MAXHOST];

        if (family == AF_INET) {
            auto *addr = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
            if (inet_ntop(AF_INET, &addr->sin_addr, host, sizeof(host))) {
                std::cout << ifa->ifa_name << " IPv4 " << host << '\n';
            }
        } else if (family == AF_INET6) {
            auto *addr6 = reinterpret_cast<sockaddr_in6 *>(ifa->ifa_addr);
            if (inet_ntop(AF_INET6, &addr6->sin6_addr, host, sizeof(host))) {
                std::cout << ifa->ifa_name << " IPv6 " << host << '\n';
            }
        }
    }

    freeifaddrs(ifaddr);
}

static void print_mbr_partitions(const std::string &device) {
    std::ifstream dev(device, std::ios::binary);
    if (!dev) {
        std::cerr << "Error: cannot open device '" << device << "'\n";
        return;
    }

    unsigned char sector[512];
    dev.read(reinterpret_cast<char *>(sector), sizeof(sector));
    if (dev.gcount() != static_cast<std::streamsize>(sizeof(sector))) {
        std::cerr << "Error: could not read MBR from '" << device << "'\n";
        return;
    }

    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        std::cerr << "Warning: '" << device << "' does not appear to have a valid MBR signature\n";
    }

    for (std::size_t i = 0; i < MBR_MAX_PARTITIONS; ++i) {
        auto *entry = reinterpret_cast<const PartitionEntry *>(
            sector + MBR_PART_TABLE_OFFSET + i * sizeof(PartitionEntry));

        if (entry->partition_type == 0 || entry->size_sectors == 0) {
            continue;
        }

        std::cout << "Partition " << (i + 1) << ": "
                  << "boot=" << (entry->boot_indicator == 0x80 ? "yes" : "no")
                  << ", type=0x" << std::hex << static_cast<int>(entry->partition_type)
                  << std::dec
                  << ", start_lba=" << entry->start_lba
                  << ", sectors=" << entry->size_sectors
                  << '\n';
    }
}

static bool wipe_mbr_partition_table(const std::string &device) {
    if (device.rfind("/dev/", 0) == 0 || is_block_device(device)) {
        std::cerr << "Refusing to modify real block device '" << device
                  << "'. Use a disk image file instead.\n";
        return false;
    }

    std::fstream dev(device, std::ios::in | std::ios::out | std::ios::binary);
    if (!dev) {
        std::cerr << "Error: cannot open device/image '" << device << "' for writing\n";
        return false;
    }

    unsigned char sector[512];
    dev.read(reinterpret_cast<char *>(sector), sizeof(sector));
    if (dev.gcount() != static_cast<std::streamsize>(sizeof(sector))) {
        std::cerr << "Error: could not read MBR from '" << device << "'\n";
        return false;
    }

    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        std::cerr << "Warning: '" << device
                  << "' does not have a valid MBR signature; writing anyway\n";
    }

    std::memset(sector + 446, 0, 64);
    dev.seekp(0);
    dev.write(reinterpret_cast<const char *>(sector), sizeof(sector));
    if (!dev) {
        std::cerr << "Error: failed to write updated MBR to '" << device << "'\n";
        return false;
    }

    dev.flush();
    return true;
}

static bool add_single_partition(const std::string &device) {
    if (device.rfind("/dev/", 0) == 0 || is_block_device(device)) {
        std::cerr << "Refusing to modify real block device '" << device
                  << "'. Use a disk image file instead.\n";
        return false;
    }

    std::fstream dev(device, std::ios::in | std::ios::out | std::ios::binary);
    if (!dev) {
        std::cerr << "Error: cannot open device/image '" << device << "' for writing\n";
        return false;
    }

    dev.seekg(0, std::ios::end);
    std::streamoff file_size = dev.tellg();
    if (file_size <= 0) {
        std::cerr << "Error: could not determine size of '" << device << "'\n";
        return false;
    }
    if (file_size < static_cast<std::streamoff>(512 * 2)) {
        std::cerr << "Error: image '" << device << "' is too small for a partition table\n";
        return false;
    }

    std::uint64_t total_sectors64 = static_cast<std::uint64_t>(file_size / 512);
    if (total_sectors64 > 0xFFFFFFFFu) {
        std::cerr << "Error: image '" << device << "' is too large for 32-bit LBA\n";
        return false;
    }
    std::uint32_t total_sectors = static_cast<std::uint32_t>(total_sectors64);

    dev.seekg(0);
    unsigned char sector[512];
    dev.read(reinterpret_cast<char *>(sector), sizeof(sector));
    if (dev.gcount() != static_cast<std::streamsize>(sizeof(sector))) {
        std::cerr << "Error: could not read MBR from '" << device << "'\n";
        return false;
    }

    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        std::memset(sector, 0, sizeof(sector));
        sector[510] = 0x55;
        sector[511] = 0xAA;
    }

    auto *entries = reinterpret_cast<PartitionEntry *>(sector + MBR_PART_TABLE_OFFSET);
    for (std::size_t i = 0; i < MBR_MAX_PARTITIONS; ++i) {
        if (entries[i].partition_type != 0 && entries[i].size_sectors != 0) {
            std::cerr << "Error: existing partition entries found on '" << device
                      << "'. Use 'partition " << device << " clean' first.\n";
            return false;
        }
    }

    PartitionEntry &p = entries[0];
    std::memset(&p, 0, sizeof(p));
    p.boot_indicator = 0x00;
    p.partition_type = 0x83; // Linux filesystem
    p.start_lba = 1;
    p.size_sectors = total_sectors - 1;

    dev.seekp(0);
    dev.write(reinterpret_cast<const char *>(sector), sizeof(sector));
    if (!dev) {
        std::cerr << "Error: failed to write updated MBR to '" << device << "'\n";
        return false;
    }

    dev.flush();
    return true;
}

static bool create_image_with_partition(const std::string &image) {
    if (image.rfind("/dev/", 0) == 0 || is_block_device(image)) {
        std::cerr << "Refusing to create image on real block device path '" << image
                  << "'. Use a regular file path instead.\n";
        return false;
    }

    struct stat st{};
    if (stat(image.c_str(), &st) == 0) {
        std::cerr << "Error: image '" << image << "' already exists\n";
        return false;
    }

    std::size_t slash_pos = image.rfind('/');
    if (slash_pos != std::string::npos && slash_pos > 0) {
        std::string parent = image.substr(0, slash_pos);
        if (!mkdir_p(parent)) {
            return false;
        }
    }

    constexpr std::uint32_t sectors = 1024; // 512 KiB image
    constexpr std::uint64_t image_size = static_cast<std::uint64_t>(sectors) * 512u;

    std::fstream dev(image, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    if (!dev) {
        std::cerr << "Error: cannot create image '" << image << "'\n";
        return false;
    }

    unsigned char sector[512];
    std::memset(sector, 0, sizeof(sector));
    sector[510] = 0x55;
    sector[511] = 0xAA;

    auto *entries = reinterpret_cast<PartitionEntry *>(sector + MBR_PART_TABLE_OFFSET);
    std::memset(entries, 0, sizeof(PartitionEntry) * MBR_MAX_PARTITIONS);

    PartitionEntry &p = entries[0];
    p.boot_indicator = 0x00;
    p.partition_type = 0x83; // Linux filesystem
    p.start_lba = 1;
    p.size_sectors = sectors - 1;

    dev.write(reinterpret_cast<const char *>(sector), sizeof(sector));
    if (!dev) {
        std::cerr << "Error: failed to write MBR to new image '" << image << "'\n";
        return false;
    }

    dev.seekp(static_cast<std::streamoff>(image_size) - 1);
    char zero = 0;
    dev.write(&zero, 1);
    if (!dev) {
        std::cerr << "Error: failed to resize image '" << image << "'\n";
        return false;
    }

    dev.flush();
    return true;
}

static void interpret_command(const std::string &line);

static void run_script(const std::string &script_path) {
    std::ifstream in(script_path);
    if (!in) {
        std::cerr << "Error: cannot open '" << script_path << "'\n";
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || (!line.empty() && line[0] == '#')) {
            continue;
        }
        interpret_command(line);
    }
}

static void interpret_command(const std::string &line) {
    std::istringstream iss(line);
    std::string command_type;
    std::string arg1;
    std::string arg2;

    if (!(iss >> command_type)) {
        return;
    }

    // Mimic: read -r command_type arg1 arg2 <<< "$1"
    iss >> arg1;
    iss >> arg2;

    // Capture the remaining text on the line (if any), typically used for
    // commands that need more than two arguments, like button labels or
    // display text.
    std::string rest_of_line;
    std::getline(iss, rest_of_line);
    if (!rest_of_line.empty()) {
        // Trim leading spaces from the leftover text.
        std::size_t first_non_space = rest_of_line.find_first_not_of(' ');
        if (first_non_space != std::string::npos) {
            rest_of_line.erase(0, first_non_space);
        } else {
            rest_of_line.clear();
        }
    }

    if (command_type == "echo") {
        std::string text;
        if (!arg1.empty()) {
            text += arg1;
        }
        if (!arg2.empty()) {
            if (!text.empty()) {
                text += ' ';
            }
            text += arg2;
        }
        g_display_text = text;
        std::cout << text << '\n';
        draw_tui_menu();
    } else if (command_type == "+") {
        try {
            long long a = std::stoll(arg1);
            long long b = std::stoll(arg2);
            std::cout << (a + b) << '\n';
        } catch (...) {
            std::cerr << "Error: invalid numbers for '+'\n";
        }
    } else if (command_type == "-") {
        try {
            long long a = std::stoll(arg1);
            long long b = std::stoll(arg2);
            std::cout << (a - b) << '\n';
        } catch (...) {
            std::cerr << "Error: invalid numbers for '-'\n";
        }
    } else if (command_type == "rem") {
        // Equivalent to: rm -rf path, with optional -f
        if (arg1.empty()) {
            std::cerr << "Error: 'rem' requires a path\n";
            return;
        }
        bool force = false;
        std::string target = arg1;
        if (arg1 == "-f") {
            force = true;
            target = arg2;
            if (target.empty()) {
                std::cerr << "Error: 'rem -f' requires a path\n";
                return;
            }
        }
        if (!remove_recursive(target, force) && !force) {
            std::cerr << "Error removing '" << target << "'\n";
        }
    } else if (command_type == "moveto") {
        if (arg1.empty()) {
            std::cerr << "Error: 'moveto' requires a directory\n";
            return;
        }
        if (chdir(arg1.c_str()) != 0) {
            std::perror(("Error changing directory to '" + arg1 + "'").c_str());
        }
    } else if (command_type == "help") {
        std::cout << "echo: Displays text on-screen\n";
        std::cout << "+: Addition\n";
        std::cout << "-: Removal of number\n";
        std::cout << "rem: Delete a path (irreversible)\n";
        std::cout << "rem arguments: -f: Forced deletion\n";
        std::cout << "moveto: CD into a directory\n";
        std::cout << "help: Get command help\n";
        std::cout << "ip: Get IP address information\n";
        std::cout << "create: Create a file\n";
        std::cout << "import: Import a script\n";
        std::cout << "adm: Run a command as admin (requires root)\n";
        std::cout << "partition: Show or modify MBR on a disk image\n";
        std::cout << "           Usage: partition <image> [clean|add|create]\n";
        std::cout << "button: TUI buttons and selection\n";
        std::cout << "        button add -text <label>\n";
        std::cout << "        button select <index>\n";
        std::cout << "        button next / button prev\n";
        std::cout << "display: Change TUI display text\n";
        std::cout << "         display -change <text>\n";
    } else if (command_type == "ip") {
        print_ip_addresses();
    } else if (command_type == "create") {
        if (arg1.empty()) {
            std::cerr << "Error: 'create' requires a filename\n";
            return;
        }
        std::ofstream ofs(arg1);
        if (!ofs) {
            std::cerr << "Error creating file '" << arg1 << "'\n";
        }
    } else if (command_type == "import") {
        if (arg1.empty()) {
            std::cerr << "Error: 'import' requires a script path\n";
            return;
        }
        run_script(arg1);
    } else if (command_type == "adm") {
        if (arg1.empty()) {
            std::cerr << "Error: 'adm' requires a command\n";
            return;
        }
        if (geteuid() != 0) {
            std::cerr << "Error: 'adm' requires root privileges (run nyns as root)\n";
            return;
        }
        int rc = std::system(arg1.c_str());
        if (rc == -1) {
            std::perror("Error running admin command");
        }
    } else if (command_type == "button") {
        if (arg1 == "add" && arg2 == "-text") {
            if (rest_of_line.empty()) {
                std::cerr << "Error: 'button add -text' requires a label\n";
                return;
            }
            g_buttons.push_back(rest_of_line);
            if (g_selected_button < 0) {
                g_selected_button = 0;
            }
            draw_tui_menu();
        } else if (arg1 == "select") {
            if (arg2.empty()) {
                std::cerr << "Error: 'button select' requires an index\n";
                return;
            }
            if (g_buttons.empty()) {
                std::cerr << "Error: no buttons to select\n";
                return;
            }
            try {
                int idx = std::stoi(arg2);
                if (idx < 1 || idx > static_cast<int>(g_buttons.size())) {
                    std::cerr << "Error: button index out of range\n";
                    return;
                }
                g_selected_button = idx - 1;
                draw_tui_menu();
            } catch (...) {
                std::cerr << "Error: invalid index for 'button select'\n";
            }
        } else if (arg1 == "next") {
            if (g_buttons.empty()) {
                std::cerr << "Error: no buttons to navigate\n";
                return;
            }
            if (g_selected_button < 0 ||
                g_selected_button >= static_cast<int>(g_buttons.size())) {
                g_selected_button = 0;
            } else {
                g_selected_button =
                    (g_selected_button + 1) % static_cast<int>(g_buttons.size());
            }
            draw_tui_menu();
        } else if (arg1 == "prev") {
            if (g_buttons.empty()) {
                std::cerr << "Error: no buttons to navigate\n";
                return;
            }
            if (g_selected_button < 0 ||
                g_selected_button >= static_cast<int>(g_buttons.size())) {
                g_selected_button = 0;
            } else {
                g_selected_button =
                    (g_selected_button - 1 + static_cast<int>(g_buttons.size())) %
                    static_cast<int>(g_buttons.size());
            }
            draw_tui_menu();
        } else {
            std::cerr << "Error: unknown 'button' usage. Expected one of:\n";
            std::cerr << "  button add -text <label>\n";
            std::cerr << "  button select <index>\n";
            std::cerr << "  button next\n";
            std::cerr << "  button prev\n";
        }
    } else if (command_type == "display") {
        if (arg1 == "-change") {
            std::string new_text = !rest_of_line.empty() ? rest_of_line : arg2;
            if (new_text.empty()) {
                std::cerr << "Error: 'display -change' requires text\n";
                return;
            }
            g_display_text = new_text;
            draw_tui_menu();
        } else {
            std::cerr << "Error: unknown 'display' usage. Expected: display -change <text>\n";
        }
    } else if (command_type == "partition") {
        if (arg1.empty()) {
            std::cerr << "Error: 'partition' requires a device or image path\n";
            return;
        }
        if (arg2 == "wipe" || arg2 == "clean") {
            if (wipe_mbr_partition_table(arg1)) {
                std::cout << "MBR partition table cleaned on '" << arg1 << "'\n";
            }
        } else if (arg2 == "add") {
            if (add_single_partition(arg1)) {
                std::cout << "Single primary partition added on '" << arg1 << "'\n";
            }
        } else if (arg2 == "create") {
            if (create_image_with_partition(arg1)) {
                std::cout << "Disk image created with single primary partition at '" << arg1 << "'\n";
            }
        } else if (arg2.empty()) {
            print_mbr_partitions(arg1);
        } else {
            std::cerr << "Error: unknown partition action '" << arg2
                      << "'. Use no action, 'clean', or 'add'.\n";
        }
    } else {
        std::cerr << "Error: Unknown command '" << command_type << "'\n";
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <script.nyns>\n";
        return 1;
    }

    run_script(argv[1]);
    return 0;
}
