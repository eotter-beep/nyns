// C++ port of bin/nyns.sh, with minimal external dependencies

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <cerrno>
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

    if (command_type == "echo") {
        if (!arg1.empty()) {
            std::cout << arg1;
        }
        if (!arg2.empty()) {
            if (!arg1.empty()) {
                std::cout << ' ';
            }
            std::cout << arg2;
        }
        std::cout << '\n';
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
        std::cout << "adm: Run a command as admin (not supported)\n";
        std::cout << "partition: Partition a device (not supported)\n";
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
    } else if (command_type == "partition") {
        std::cerr << "Error: 'partition' is not supported in this build (no fdisk dependency)\n";
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
